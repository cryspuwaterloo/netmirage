#define _GNU_SOURCE

#include "setup.h"

#include <fenv.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "graphml.h"
#include "ip.h"
#include "log.h"
#include "mem.h"
#include "routeplanner.h"
#include "topology.h"
#include "work.h"

static const setupParams* globalParams;

int setupInit(const setupParams* params) {
	globalParams = params;
	int res = workInit(params->nsPrefix, params->softMemCap);
	if (res != 0) return res;

	if (params->edgeNodeCount < 1) {
		lprintln(LogError, "No edge nodes were specified. Configure them using a setup file or manually using --edge-node.");
		res = 1;
		goto cleanup;
	}

	// Complete definitions for edge nodes by filling in default / missing data
	size_t edgeSubnetsNeeded = 0;
	for (size_t i = 0; i < params->edgeNodeCount; ++i) {
		edgeNodeParams* edge = &params->edgeNodes[i];
		if (edge->intf == NULL) {
			if (!params->edgeNodeDefaults.intfSpecified) {
				char ip[IP4_ADDR_BUFLEN];
				ip4AddrToString(edge->ip, ip);
				lprintf(LogError, "No interface was specified for edge node with IP %s. Either specify an interface, or specify --iface if all edge nodes are behind the same one.\n", ip);
				res = 1;
				goto cleanup;
			}
			edge->intf = eamalloc(strlen(params->edgeNodeDefaults.intf), 1, 1);
			strcpy(edge->intf, params->edgeNodeDefaults.intf);
		}
		if (!edge->macSpecified) {
			res = workGetEdgeMac(edge->intf, edge->ip, &edge->mac);
			if (res != 0) {
				char ip[IP4_ADDR_BUFLEN];
				ip4AddrToString(edge->ip, ip);
				lprintf(LogError, "Could not locate the MAC address for edge node with IP %s on interface '%s'. Verify that the host is online, or configure the MAC address manually.\n", ip, edge->intf);
				goto cleanup;
			}
		}
		if (!edge->vsubnetSpecified) {
			++edgeSubnetsNeeded;
		}
	}

	// Automatically provide client subnets to unconfigured edge nodes
	bool subnetErr = false;
	if (edgeSubnetsNeeded > UINT32_MAX) {
		subnetErr = true;
	} else if (edgeSubnetsNeeded > 0) {
		ip4FragIter* fragIt = ip4FragmentSubnet(&params->edgeNodeDefaults.globalVSubnet, (uint32_t)edgeSubnetsNeeded);
		if (fragIt == NULL) {
			char subnet[IP4_CIDR_BUFLEN];
			ip4SubnetToString(&params->edgeNodeDefaults.globalVSubnet, subnet);
			lprintf(LogError, "The virtual client subnet %s is not large enough to provision %lu edge nodes. Either increase the subnet size or decrease the number of edge nodes.\n", subnet, edgeSubnetsNeeded);
			subnetErr = true;
		} else {
			for (size_t i = 0; i < params->edgeNodeCount; ++i) {
				edgeNodeParams* edge = &params->edgeNodes[i];
				if (!edge->vsubnetSpecified) {
					if (!ip4FragIterNext(fragIt)) {
						lprintln(LogError, "Failed to advance vsubnet fragment iterator\n");
						subnetErr = true;
						break;
					}
					ip4FragIterSubnet(fragIt, &edge->vsubnet);
				}
			}
			ip4FreeFragIter(fragIt);
		}
	}

	// TODO scan for subnet overlaps

	// Make sure that we have at least one bit to flip in the IP addresses
	for (size_t i = 0; i < params->edgeNodeCount; ++i) {
		edgeNodeParams* edge = &params->edgeNodes[i];
		char ip[IP4_ADDR_BUFLEN];
		char mac[MAC_ADDR_BUFLEN];
		char subnet[IP4_CIDR_BUFLEN];
		ip4AddrToString(edge->ip, ip);
		macAddrToString(&edge->mac, mac);
		ip4SubnetToString(&edge->vsubnet, subnet);
		lprintf(LogInfo, "Configured edge node: IP %s, interface %s, MAC %s, client subnet %s\n", ip, edge->intf, mac, subnet);
	}
	if (subnetErr) {
		res = 1;
		goto cleanup;
	}

	return 0;
cleanup:
	workCleanup();
	return res;
}

int setupCleanup(void) {
	return workCleanup();
}

int destroyNetwork(void) {
	lprintf(LogInfo, "Destroying any existing virtual network with namespace prefix '%s'\n", globalParams->nsPrefix);

	uint32_t deletedHosts;
	int err = workDestroyHosts(&deletedHosts);
	if (err != 0) return err;

	if (deletedHosts > 0) {
		lprintf(LogInfo, "Destroyed an existing virtual network with %lu hosts\n", deletedHosts);
	}
	return 0;
}


/******************************************************************************\
|                               GraphML Parsing                                |
\******************************************************************************/

typedef struct {
	ip4Addr addr; // Duplicated for all interfaces
	bool isClient;
	ip4Subnet clientSubnet;
} gmlNodeState;

typedef struct {
	bool finishedNodes;
	bool ignoreNodes;
	bool ignoreEdges;

	// Variable-sized buffer for storing all node states
	gmlNodeState* nodeStates;
	size_t nodeCount;   // Total number of nodes (client + non-client)
	size_t clientNodes; // Total number of client nodes
	size_t nodeCap;
	GHashTable* gmlToState; // Maps GraphML names to indices in nodeStates

	double clientsPerEdge;
	edgeNodeParams* currentEdge;
	nodeId currentEdgeClients;
	nodeId filledEdges;
	ip4FragIter* clientIter;

	ip4Iter* intfAddrIter;
	macAddr macAddrIter;

	routePlanner* routes;
} gmlContext;

static void gmlFreeData(gpointer data) { free(data); }

static void gmlGenerateIp(gmlContext* ctx, bool* addrExhausted, ip4Addr* addr) {
	if (*addrExhausted) return;
	if (!ip4IterNext(ctx->intfAddrIter)) {
		*addrExhausted = true;
		return;
	}
	*addr = ip4IterAddr(ctx->intfAddrIter);
}

// Looks up the node state for a given string identifier from the GraphML file.
// If the state does not exist, and "node" is not NULL, then a new state is
// created and cached. Otherwise, an error occurs. Returns true on success, in
// which case "id" and "state" are set. Otherwise, returns false and their
// values are undefined.
static bool gmlNameToState(gmlContext* ctx, const char* name, const TopoNode* node, nodeId* id, gmlNodeState** state) {
	gpointer ptr;
	gboolean exists = g_hash_table_lookup_extended(ctx->gmlToState, name, NULL, &ptr);
	size_t index = GPOINTER_TO_SIZE(ptr);
	*state = &ctx->nodeStates[index];
	if (!exists) {
		if (node == NULL) {
			lprintf(LogError, "Requested existing state for unknown host '%s'\n", name);
			return NULL;
		}
		bool addrExhausted;
		ip4Addr newAddr;
		gmlGenerateIp(ctx, &addrExhausted, &newAddr);
		if (addrExhausted) {
			lprintln(LogError, "Cannot set up all of the virtual hosts because the non-routable IPv4 address space has been exhausted. Either decrease the number of nodes in the topology, or assign fewer addresses to the edge nodes.");
			return NULL;
		}

		index = ctx->nodeCount++;

		flexBufferGrow((void**)&ctx->nodeStates, ctx->nodeCount, &ctx->nodeCap, 1, sizeof(gmlNodeState));
		g_hash_table_insert(ctx->gmlToState, (gpointer)strdup(name), GSIZE_TO_POINTER(index));

		*state = &ctx->nodeStates[index];
		(*state)->addr = newAddr;
		(*state)->isClient = node->client;
	}
	*id = (nodeId)index;
	return state;
}

static int gmlAddNode(const GmlNode* node, void* userData) {
	gmlContext* ctx = userData;
	if (ctx->ignoreNodes) return 0;
	if (ctx->finishedNodes) {
		lprintln(LogError, "The GraphML file contains some <node> elements after the <edge> elements. To parse this file, use the --two-pass option.");
		return 1;
	}

	nodeId id;
	gmlNodeState* state;
	if (!gmlNameToState(ctx, node->name, &node->t, &id, &state)) return 1;

	macAddr macs[NeededMacsClient];
	if (node->t.client) {
		if (!macNextAddrs(&ctx->macAddrIter, macs, NeededMacsClient)) {
			lprintln(LogError, "Ran out of MAC addresses when creating a new client node.");
			return 1;
		}
		++ctx->clientNodes;
	}

	if (PASSES_LOG_THRESHOLD(LogDebug)) {
		char ip[IP4_ADDR_BUFLEN];
		ip4AddrToString(state->addr, ip);
		lprintf(LogDebug, "GraphML node '%s' assigned identifier %u and IP address %s\n", node->name, id, ip);
	}

	return workAddHost(id, state->addr, macs, &node->t);
}

static int gmlAddLink(const GmlLink* link, void* userData) {
	gmlContext* ctx = userData;
	if (ctx->ignoreEdges) return 0;
	if (!ctx->finishedNodes) {
		ctx->finishedNodes = true;

		lprintln(LogInfo, "Host creation complete. Now adding virtual ethernet connections.");
		lprintf(LogDebug, "Encountered %u nodes (%u clients)\n", ctx->nodeCount, ctx->clientNodes);
		if (ctx->clientNodes < globalParams->edgeNodeCount) {
			lprintf(LogError, "There are fewer client nodes in the topology (%u) than edges nodes (%u). Either use a larger topology, or decrease the number of edge nodes.\n", ctx->clientNodes, globalParams->edgeNodeCount);
			return 1;
		}
		ctx->clientsPerEdge = (double)ctx->clientNodes / (double)globalParams->edgeNodeCount;
		ctx->routes = rpNewPlanner((nodeId)ctx->nodeCount);
	}

	nodeId sourceId, targetId;
	gmlNodeState* sourceState;
	gmlNodeState* targetState;
	if (!gmlNameToState(ctx, link->sourceName, NULL, &sourceId, &sourceState)) return 1;
	if (!gmlNameToState(ctx, link->targetName, NULL, &targetId, &targetState)) return 1;

	int res = 0;
	if (sourceId == targetId) {
		if (sourceState->isClient) {
			res = workSetSelfLink(sourceId, &link->t);
		}
	} else {
		macAddr macs[NeededMacsLink];
		if (!macNextAddrs(&ctx->macAddrIter, macs, NeededMacsLink)) {
			lprintln(LogError, "Ran out of MAC addresses when adding a new virtual ethernet connection.");
			return 1;
		}
		res = workAddLink(sourceId, targetId, sourceState->addr, targetState->addr, macs, &link->t);
		if (res == 0) {
			if (link->weight < 0.f) {
				lprintf(LogError, "The link from '%s' to '%s' in the topology has negative weight %f, which is not supported.\n", link->sourceName, link->targetName, link->weight);
				res = 1;
			} else {
				rpSetWeight(ctx->routes, sourceId, targetId, link->weight);
				rpSetWeight(ctx->routes, targetId, sourceId, link->weight);
			}
		}
	}
	return res;
}

static bool gmlNextEdge(gmlContext* ctx) {
	if (ctx->currentEdge == NULL) {
		ctx->currentEdge = globalParams->edgeNodes;
		ctx->filledEdges = 0;
	} else {
		++ctx->filledEdges;
		if (ctx->filledEdges >= globalParams->edgeNodeCount) {
			ctx->currentEdge = NULL;
			return false;
		}
		++ctx->currentEdge;
		ip4FreeFragIter(ctx->clientIter);
	}
	ctx->currentEdgeClients = 0;

	// This approach avoids numerical robustness problems
	double prevMarker = round(ctx->clientsPerEdge * ctx->filledEdges);
	double nextMarker = round(ctx->clientsPerEdge * (ctx->filledEdges+1));
	fesetround(FE_TONEAREST);
	nodeId currentEdgeCapacity = (nodeId)llrint(nextMarker - prevMarker);

	ctx->clientIter = ip4FragmentSubnet(&ctx->currentEdge->vsubnet, currentEdgeCapacity);
	if (!ip4FragIterNext(ctx->clientIter)) return false;

	if (PASSES_LOG_THRESHOLD(LogDebug)) {
		char edgeIp[IP4_ADDR_BUFLEN];
		char edgeSubnet[IP4_CIDR_BUFLEN];
		ip4AddrToString(ctx->currentEdge->ip, edgeIp);
		ip4SubnetToString(&ctx->currentEdge->vsubnet, edgeSubnet);
		lprintf(LogDebug, "Now allocating %u client subnets for edge %s (range %s)\n", currentEdgeCapacity, edgeIp, edgeSubnet);
	}
	return true;
}

static bool gmlNextClientSubnet(gmlContext* ctx, ip4Subnet* subnet) {
	if (ctx->clientIter == NULL || !ip4FragIterNext(ctx->clientIter)) {
		if (!gmlNextEdge(ctx)) return false;
	}
	ip4FragIterSubnet(ctx->clientIter, subnet);
	return true;
}

int setupGraphML(const setupGraphMLParams* gmlParams) {
	lprintf(LogInfo, "Reading network topology in GraphML format from %s\n", globalParams->srcFile ? globalParams->srcFile : "<stdin>");

	gmlContext ctx = {
		.finishedNodes = false,
		.ignoreNodes = false,
		.ignoreEdges = false,

		.clientNodes = 0,

		.clientIter = NULL,
		.macAddrIter = { .octets = { 0 } },

		.routes = NULL,
	};
	macNextAddr(&ctx.macAddrIter); // Skip all-zeroes address (unassignable)
	flexBufferInit((void**)&ctx.nodeStates, &ctx.nodeCount, &ctx.nodeCap);
	ctx.gmlToState = g_hash_table_new_full(&g_str_hash, &g_str_equal, &gmlFreeData, NULL);

	// We assign internal interface addresses from the full IPv4 space, but
	// avoid the subnets reserved for the edge nodes. The fact that the
	// addresses we use are publicly routable does not matter, since the
	// internal node namespaces are not connected to the Internet.
	const size_t ReservedSubnetCount = 3;
	ip4Subnet reservedSubnets[ReservedSubnetCount];
	ip4GetSubnet("0.0.0.0/8", &reservedSubnets[0]);
	ip4GetSubnet("127.0.0.0/8", &reservedSubnets[1]);
	ip4GetSubnet("255.255.255.255/32", &reservedSubnets[2]);
	const ip4Subnet* restrictedSubnets[globalParams->edgeNodeCount+ReservedSubnetCount+1];
	size_t subnets = 0;
	for (size_t i = 0; i < ReservedSubnetCount; ++i) {
		restrictedSubnets[subnets++] = &reservedSubnets[i];
	}
	for (size_t i = 0; i < globalParams->edgeNodeCount; ++i) {
		restrictedSubnets[subnets++] = &globalParams->edgeNodes[i].vsubnet;
	}
	restrictedSubnets[subnets++] = NULL;
	ip4Subnet everything;
	ip4GetSubnet("0.0.0.0/0", &everything);
	ctx.intfAddrIter = ip4NewIter(&everything, restrictedSubnets);

	bool addrExhausted;
	ip4Addr rootAddr;
	gmlGenerateIp(&ctx, &addrExhausted, &rootAddr);
	if (addrExhausted) {
		lprintln(LogError, "The edge node subnets completely fill the unreserved IPv4 space. Some addresses must be left for internal networking interfaces in the emulator.");
		goto cleanup;
	}

	int err = workAddRoot(rootAddr);
	if (err != 0) goto cleanup;

	if (globalParams->srcFile) {
		int passes = gmlParams->twoPass ? 2 : 1;

		// Setup based on number of passes
		if (passes > 1) ctx.ignoreEdges = true;

		for (int pass = passes; pass > 0; --pass) {
			err = gmlParseFile(globalParams->srcFile, &gmlAddNode, &gmlAddLink, &ctx, gmlParams->clientType, gmlParams->weightKey);
			if (err != 0) goto cleanup;

			// Transitions between passes
			if (pass == 2) {
				// Pretend that we've reached the end of the node section in a
				// sorted file, and ignore any future nodes rather than
				// raising an error.
				ctx.finishedNodes = true;
				ctx.ignoreNodes = true;
				ctx.ignoreEdges = false;
			}
		}
	} else {
		if (gmlParams->twoPass) {
			lprintln(LogError, "Cannot perform two passes when reading a GraphML file from stdin. Either ensure that all nodes appear before edges, or read from a file.");
			err = 1;
			goto cleanup;

		}
		err = gmlParse(stdin, &gmlAddNode, &gmlAddLink, &ctx, gmlParams->clientType, gmlParams->weightKey);
	}

	// Host and link construction is finished. Now we set up routing
	lprintln(LogInfo, "Setting up static routing for the network");

	if (ctx.routes == NULL) {
		lprintln(LogError, "Network topology did not contain any links");
		err = 1;
		goto cleanup;
	}
	rpPlanRoutes(ctx.routes);

	lprintf(LogDebug, "Assigning %u client nodes to %u edge nodes\n", ctx.clientNodes, globalParams->edgeNodeCount);
	for (size_t id = 0; id < ctx.nodeCount; ++id) {
		gmlNodeState* node = &ctx.nodeStates[id];
		if (!node->isClient) continue;

		if (!gmlNextClientSubnet(&ctx, &node->clientSubnet)) {
			lprintln(LogError, "BUG: exhausted client node subnet space");
			goto cleanup;
		}
		if (PASSES_LOG_THRESHOLD(LogDebug)) {
			char subnet[IP4_CIDR_BUFLEN];
			ip4SubnetToString(&node->clientSubnet, subnet);
			lprintf(LogDebug, "Assigned client node %u to subnet %s\n", id, subnet);
		}
	}

	// Build routes between every pair of client nodes
	lprintln(LogDebug, "Adding static routes along paths for all client node pairs");
	bool seenUnroutable = false;
	for (nodeId startId = 0; startId < ctx.nodeCount; ++startId) {
		gmlNodeState* start = &ctx.nodeStates[startId];
		if (!start->isClient) continue;

		for (nodeId endId = startId+1; endId < ctx.nodeCount; ++endId) {
			gmlNodeState* end = &ctx.nodeStates[endId];
			if (!end->isClient) continue;

			lprintf(LogDebug, "Constructing route from client %u to %u\n", startId, endId);
			nodeId* path;
			nodeId steps;
			if (!rpGetRoute(ctx.routes, startId, endId, &path, &steps)) {
				if (!seenUnroutable) {
					lprintf(LogWarning, "Topology contains unconnected client nodes (e.g., %u to %u is unroutable)\n", startId, endId);
					seenUnroutable = true;
				}
				continue;
			}
			if (steps < 2) {
				lprintf(LogError, "BUG: route from client %u to %u has %d steps\n", startId, endId, steps);
				continue;
			}

			nodeId prevId = path[0];
			for (nodeId step = 1; step < steps; ++step) {
				nodeId nextId = path[step];
				lprintf(LogDebug, "Hop %d for %u => %u: %u => %u\n", step, startId, endId, prevId, nextId);
				err = workAddInternalRoutes(prevId, nextId, ctx.nodeStates[prevId].addr, ctx.nodeStates[nextId].addr, &start->clientSubnet, &end->clientSubnet);
				if (err != 0) goto cleanup;

				prevId = nextId;
			}
		}
	}

cleanup:
	if (ctx.clientIter != NULL) ip4FreeFragIter(ctx.clientIter);
	if (ctx.routes != NULL) rpFreePlan(ctx.routes);
	g_hash_table_destroy(ctx.gmlToState);
	ip4FreeIter(ctx.intfAddrIter);
	flexBufferFree((void**)&ctx.nodeStates, &ctx.nodeCount, &ctx.nodeCap);
	return err;
}
