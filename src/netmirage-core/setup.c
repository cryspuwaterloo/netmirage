/*******************************************************************************
 * Copyright © 2016 Nik Unger, Ian Goldberg, Qatar University, and the Qatar
 * Foundation for Education, Science and Community Development.
 *
 * This file is part of NetMirage.
 *
 * NetMirage is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * NetMirage is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with NetMirage. If not, see <http://www.gnu.org/licenses/>.
 *******************************************************************************/
#define _GNU_SOURCE

#include "setup.h"

#include <errno.h>
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

static const setupParams* globalParams = NULL;

static bool edgeFileOpened = false;
static FILE* edgeFile = NULL;

#define DO_OR_GOTO(stmt, label, res) do{ \
	res = (stmt); \
	if (res != 0) { \
		goto label; \
	} \
}while(0)

#define DO_OR_RETURN(stmt) do{ \
	int ___err = (stmt); \
	if (___err != 0) { \
		return ___err; \
	} \
}while(0)

int setupInit(void) {
	DO_OR_RETURN(workInit());
	return 0;
}

int setupConfigure(const setupParams* params) {
	globalParams = params;
	DO_OR_RETURN(workConfigure(logThreshold(), logColorized(), params->nsPrefix, params->ovsDir, params->ovsSchema, params->softMemCap));
	DO_OR_RETURN(workJoin(false));

	if (params->destroyFirst) {
		int err = destroyNetwork();
		if (err != 0) return err;
	} else if (params->edgeNodeCount < 1) {
		lprintln(LogError, "No edge nodes were specified. Configure them using a setup file or manually using --edge-node.");
		return 1;
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
				return 1;
			}
			edge->intf = eamalloc(strlen(params->edgeNodeDefaults.intf), 1, 1);
			strcpy(edge->intf, params->edgeNodeDefaults.intf);
		}
		if (!edge->macSpecified) {
			DO_OR_RETURN(workGetEdgeRemoteMac(edge->intf, edge->ip, &edge->mac));
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
	if (subnetErr) return 1;

	if (!params->quiet) {
		if (params->edgeFile == NULL) {
			edgeFile = stdout;
			lprintln(LogDebug, "Writing edge node commands to stdout");
		} else {
			errno = 0;
			edgeFile = fopen(params->edgeFile, "we");
			if (edgeFile == NULL) {
				lprintf(LogError, "Failed to open edge node command file \"%s\": %s\n", edgeFile, strerror(errno));
				return 1;
			}
			edgeFileOpened = true;
			lprintf(LogDebug, "Writing edge node commands to '%s'\n", params->edgeFile);
		}
	}

	return 0;
}

int setupCleanup(void) {
	DO_OR_RETURN(workCleanup());
	if (edgeFileOpened) {
		fclose(edgeFile);
	}
	return 0;
}

int destroyNetwork(void) {
	DO_OR_RETURN(workJoin(true));
	lprintf(LogInfo, "Destroying any existing virtual network with namespace prefix '%s'\n", globalParams->nsPrefix);
	DO_OR_RETURN(workDestroyHosts());
	DO_OR_RETURN(workJoin(false));

	return 0;
}


/******************************************************************************\
|                               GraphML Parsing                                |
\******************************************************************************/

typedef struct {
	ip4Addr addr; // Duplicated for all interfaces
	bool isClient;
	ip4Subnet clientSubnet;
	macAddr clientMacs[NEEDED_MACS_CLIENT];
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
	nodeId currentEdgeIdx;
	nodeId currentEdgeClients;
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
		bool addrExhausted = false;
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

	if (node->t.client) {
		if (!macNextAddrs(&ctx->macAddrIter, state->clientMacs, NEEDED_MACS_CLIENT)) {
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

	DO_OR_RETURN(workAddHost(id, state->addr, state->clientMacs, &node->t));
	return 0;
}

static int gmlOnFinishedNodes(gmlContext* ctx) {
	lprintln(LogInfo, "Host creation complete. Now adding virtual ethernet connections.");
	lprintf(LogDebug, "Encountered %u nodes (%u clients)\n", ctx->nodeCount, ctx->clientNodes);
	if (ctx->clientNodes < globalParams->edgeNodeCount) {
		lprintf(LogError, "There are fewer client nodes in the topology (%u) than edges nodes (%u). Either use a larger topology, or decrease the number of edge nodes.\n", ctx->clientNodes, globalParams->edgeNodeCount);
		return 1;
	}

	uint64_t worstCaseLinkCount = (uint64_t)ctx->nodeCount * (uint64_t)ctx->nodeCount;
	DO_OR_RETURN(workJoin(false));
	DO_OR_RETURN(workEnsureSystemScaling(worstCaseLinkCount, (nodeId)ctx->nodeCount, (nodeId)ctx->clientNodes));
	DO_OR_RETURN(workJoin(false));

	ctx->clientsPerEdge = (double)ctx->clientNodes / (double)globalParams->edgeNodeCount;
	ctx->routes = rpNewPlanner((nodeId)ctx->nodeCount);
	return 0;
}

static int gmlAddLink(const GmlLink* link, void* userData) {
	gmlContext* ctx = userData;

	if (ctx->ignoreEdges) return 0;
	if (!ctx->finishedNodes) {
		ctx->finishedNodes = true;
		int res = gmlOnFinishedNodes(ctx);
		if (res != 0) return res;
	}

	nodeId sourceId, targetId;
	gmlNodeState* sourceState;
	gmlNodeState* targetState;
	if (!gmlNameToState(ctx, link->sourceName, NULL, &sourceId, &sourceState)) return 1;
	if (!gmlNameToState(ctx, link->targetName, NULL, &targetId, &targetState)) return 1;

	if (sourceId == targetId) {
		if (sourceState->isClient) {
			DO_OR_RETURN(workSetSelfLink(sourceId, &link->t));
		}
	} else {
		macAddr macs[NEEDED_MACS_LINK];
		if (!macNextAddrs(&ctx->macAddrIter, macs, NEEDED_MACS_LINK)) {
			lprintln(LogError, "Ran out of MAC addresses when adding a new virtual ethernet connection.");
			return 1;
		}
		DO_OR_RETURN(workAddLink(sourceId, targetId, sourceState->addr, targetState->addr, macs, &link->t));
		if (link->weight < 0.f) {
			lprintf(LogError, "The link from '%s' to '%s' in the topology has negative weight %f, which is not supported.\n", link->sourceName, link->targetName, link->weight);
			return 1;
		} else {
			rpSetWeight(ctx->routes, sourceId, targetId, link->weight);
			rpSetWeight(ctx->routes, targetId, sourceId, link->weight);
		}
	}
	return 0;
}

static bool gmlNextEdge(gmlContext* ctx) {
	if (ctx->clientIter == NULL) {
		ctx->currentEdgeIdx = 0;
	} else {
		++ctx->currentEdgeIdx;
		ip4FreeFragIter(ctx->clientIter);
		if (ctx->currentEdgeIdx >= globalParams->edgeNodeCount) {
			ctx->clientIter = NULL;
			return false;
		}
	}
	ctx->currentEdgeClients = 0;

	// This approach avoids numerical robustness problems
	double prevMarker = round(ctx->clientsPerEdge * ctx->currentEdgeIdx);
	double nextMarker = round(ctx->clientsPerEdge * (ctx->currentEdgeIdx+1));
	fesetround(FE_TONEAREST);
	nodeId currentEdgeCapacity = (nodeId)llrint(nextMarker - prevMarker);

	edgeNodeParams* edge = &globalParams->edgeNodes[ctx->currentEdgeIdx];
	ctx->clientIter = ip4FragmentSubnet(&edge->vsubnet, currentEdgeCapacity);
	if (!ip4FragIterNext(ctx->clientIter)) return false;

	if (PASSES_LOG_THRESHOLD(LogDebug)) {
		char edgeIp[IP4_ADDR_BUFLEN];
		char edgeSubnet[IP4_CIDR_BUFLEN];
		ip4AddrToString(edge->ip, edgeIp);
		ip4SubnetToString(&edge->vsubnet, edgeSubnet);
		lprintf(LogDebug, "Now allocating %u client subnets for edge %s (range %s)\n", currentEdgeCapacity, edgeIp, edgeSubnet);
	}

	if (edgeFile != NULL) {
		fprintf(edgeFile, "netmirage-edge");
		for (size_t i = 0; i < globalParams->edgeNodeCount; ++i) {
			edgeNodeParams* otherEdge = &globalParams->edgeNodes[i];
			char otherEdgeSubnet[IP4_CIDR_BUFLEN];
			ip4SubnetToString(&otherEdge->vsubnet, otherEdgeSubnet);
			fprintf(edgeFile, " -e %s", otherEdgeSubnet);
		}
		fprintf(edgeFile, " -c %u", currentEdgeCapacity);
		if (edge->remoteDev == NULL) {
			fprintf(edgeFile, " <iface>");
		} else {
			fprintf(edgeFile, " %s", edge->remoteDev);
		}
		if (globalParams->routingIp == 0) {
			fprintf(edgeFile, " <core-ip>");
		} else {
			char routingIpStr[IP4_ADDR_BUFLEN];
			ip4AddrToString(globalParams->routingIp, routingIpStr);
			fprintf(edgeFile, " %s", routingIpStr);
		}
		char edgeSubnet[IP4_CIDR_BUFLEN];
		ip4SubnetToString(&edge->vsubnet, edgeSubnet);
		fprintf(edgeFile, " %s", edgeSubnet);
		if (edge->remoteApps == 0) {
			fprintf(edgeFile, " <applications>");
		} else {
			fprintf(edgeFile, " %u", edge->remoteApps);
		}
		fprintf(edgeFile, "\n");
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
	ctx.intfAddrIter = ip4NewIter(&everything, false, restrictedSubnets);

	int err;
	uint32_t* edgePorts = eamalloc(globalParams->edgeNodeCount, sizeof(uint32_t), 0);
	uint32_t nextOvsPort = 1;

	ip4Addr rootAddrs[2];
	for (int i = 0; i < 2; ++i) {
		bool addrExhausted = false;
		gmlGenerateIp(&ctx, &addrExhausted, &rootAddrs[i]);
		if (addrExhausted) {
			lprintln(LogError, "The edge node subnets completely fill the unreserved IPv4 space. Some addresses must be left for internal networking interfaces in the emulator.");
			err = 1;
			goto cleanup;
		}
	}

	DO_OR_GOTO(workAddRoot(rootAddrs[0], rootAddrs[1], globalParams->rootIsInitNs), cleanup, err);
	DO_OR_GOTO(workJoin(false), cleanup, err);

	// Move all interfaces associated with edge nodes into the root namespace
	for (size_t i = 0; i < globalParams->edgeNodeCount; ++i) {
		edgeNodeParams* edge = &globalParams->edgeNodes[i];

		// Check to see if this is a duplicate interface. We simply perform
		// linear searches because the number of edge nodes should be relatively
		// small (typically less than 10).
		bool duplicateIntf = false;
		for (size_t j = 0; j < i; ++j) {
			edgeNodeParams* otherEdge = &globalParams->edgeNodes[j];
			if (strcmp(edge->intf, otherEdge->intf) == 0) {
				edgePorts[i] = edgePorts[j];
				duplicateIntf = true;
				break;
			}
		}
		if (!duplicateIntf) {
			DO_OR_GOTO(workAddEdgeInterface(edge->intf), cleanup, err);
			DO_OR_GOTO(workJoin(false), cleanup, err);
			edgePorts[i] = nextOvsPort++;
		}

		macAddr edgeLocalMac;
		DO_OR_GOTO(workGetEdgeLocalMac(edge->intf, &edgeLocalMac), cleanup, err);
		DO_OR_GOTO(workAddEdgeRoutes(&edge->vsubnet, edgePorts[i], &edgeLocalMac, &edge->mac), cleanup, err);
	}
	DO_OR_GOTO(workJoin(false), cleanup, err);

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

	DO_OR_GOTO(workJoin(false), cleanup, err);

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
		size_t edgeIdx = ctx.currentEdgeIdx;
		if (PASSES_LOG_THRESHOLD(LogDebug)) {
			char subnet[IP4_CIDR_BUFLEN];
			ip4SubnetToString(&node->clientSubnet, subnet);
			lprintf(LogDebug, "Assigned client node %u to subnet %s owned by edge %lu\n", id, subnet, edgeIdx);
		}
		DO_OR_GOTO(workAddClientRoutes((nodeId)id, node->clientMacs, &node->clientSubnet, edgePorts[edgeIdx], nextOvsPort), cleanup, err);
		nextOvsPort += NEEDED_PORTS_CLIENT;
		// We need to join here because Open vSwitch locks the database file
		// when processing commands. They cannot be parallelized.
		DO_OR_GOTO(workJoin(false), cleanup, err);
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
				DO_OR_GOTO(workAddInternalRoutes(prevId, nextId, ctx.nodeStates[prevId].addr, ctx.nodeStates[nextId].addr, &start->clientSubnet, &end->clientSubnet), cleanup, err);
				// Another join mandated by locking Open vSwitch commands
				DO_OR_GOTO(workJoin(false), cleanup, err);

				prevId = nextId;
			}
		}
	}
	DO_OR_GOTO(workJoin(false), cleanup, err);

cleanup:
	if (ctx.clientIter != NULL) ip4FreeFragIter(ctx.clientIter);
	if (ctx.routes != NULL) rpFreePlan(ctx.routes);
	g_hash_table_destroy(ctx.gmlToState);
	ip4FreeIter(ctx.intfAddrIter);
	flexBufferFree((void**)&ctx.nodeStates, &ctx.nodeCount, &ctx.nodeCap);
	free(edgePorts);
	return err;
}
