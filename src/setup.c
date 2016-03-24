#define _GNU_SOURCE

#include "setup.h"

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
#include "topology.h"
#include "work.h"

static const setupParams* globalParams;

int setupInit(const setupParams* params) {
	globalParams = params;
	int res = workInit(params->nsPrefix, params->softMemCap);
	if (res != 0) return res;

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
			res = workGetEdgeMac(edge->intf, edge->ip, &edge->mac);
			if (res != 0) {
				char ip[IP4_ADDR_BUFLEN];
				ip4AddrToString(edge->ip, ip);
				lprintf(LogError, "Could not locate the MAC address for edge node with IP %s on interface '%s'. Verify that the host is online, or configure the MAC address manually.\n", ip, edge->intf);
				return res;
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
		if (ip4SubnetSize(&edge->vsubnet, true) < 2) {
			lprintf(LogError, "Edge node with IP %s has subnet %s, which is not large enough to use libipaddr to forward traffic to the emulator\n", ip, subnet);
			subnetErr = true;
			break;
		}
		lprintf(LogInfo, "Configured edge node: IP %s, interface %s, MAC %s, client subnet %s\n", ip, edge->intf, mac, subnet);
	}
	if (subnetErr) return 1;

	return 0;
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
	nodeId id;
	ip4Addr addr; // Duplicated for all interfaces
	bool isClient;
} gmlNodeState;

typedef struct {
	bool finishedNodes;
	bool ignoreNodes;
	bool ignoreEdges;
	GHashTable* gmlToState; // Maps GraphML names to gmlNodeState pointers
	nodeId nextId;
	ip4Iter* intfAddrIter;
	macAddr macAddrIter;
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
// created and cached. Otherwise, an error occurs. Returns NULL on error.
static gmlNodeState* gmlNameToState(gmlContext* ctx, const char* name, const TopoNode* node) {
	gpointer ptr;
	gboolean exists = g_hash_table_lookup_extended(ctx->gmlToState, name, NULL, &ptr);
	gmlNodeState* state = ptr;
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

		state = emalloc(sizeof(gmlNodeState));
		state->id = ctx->nextId++;
		state->addr = newAddr;
		state->isClient = node->client;
		g_hash_table_insert(ctx->gmlToState, (gpointer)strdup(name), state);
	}
	return state;
}

static int gmlAddNode(const GmlNode* node, void* userData) {
	gmlContext* ctx = userData;
	if (ctx->ignoreNodes) return 0;
	if (ctx->finishedNodes) {
		lprintln(LogError, "The GraphML file contains some <node> elements after the <edge> elements. To parse this file, use the --two-pass option.");
		return 1;
	}

	gmlNodeState* state = gmlNameToState(ctx, node->name, &node->t);
	if (state == NULL) return 1;

	macAddr macs[NeededMacsClient];
	if (node->t.client) {
		if (!macNextAddrs(&ctx->macAddrIter, macs, NeededMacsClient)) {
			lprintln(LogError, "Ran out of MAC addresses when creating a new client node.");
			return 1;
		}
	}

	if (PASSES_LOG_THRESHOLD(LogDebug)) {
		char ip[IP4_ADDR_BUFLEN];
		ip4AddrToString(state->addr, ip);
		lprintf(LogDebug, "GraphML node '%s' assigned identifier %u and IP address %s\n", node->name, state->id, ip);
	}

	return workAddHost(state->id, state->addr, macs, &node->t);
}

static int gmlAddLink(const GmlLink* link, void* userData) {
	gmlContext* ctx = userData;
	if (ctx->ignoreEdges) return 0;
	if (!ctx->finishedNodes) {
		ctx->finishedNodes = true;
		lprintln(LogDebug, "Host creation complete. Now adding virtual ethernet connections.");
	}

	gmlNodeState* sourceState = gmlNameToState(ctx, link->sourceName, NULL);
	gmlNodeState* targetState = gmlNameToState(ctx, link->targetName, NULL);
	if (sourceState == NULL || targetState == NULL) return 1;

	int res = 0;
	if (sourceState == targetState) {
		if (sourceState->isClient) {
			res = workSetSelfLink(sourceState->id, &link->t);
		}
	} else {
		macAddr macs[NeededMacsLink];
		if (!macNextAddrs(&ctx->macAddrIter, macs, NeededMacsLink)) {
			lprintln(LogError, "Ran out of MAC addresses when adding a new virtual ethernet connection.");
			return 1;
		}
		res = workAddLink(sourceState->id, targetState->id, sourceState->addr, targetState->addr, macs, &link->t);
	}
	return res;
}

int setupGraphML(const setupGraphMLParams* gmlParams) {
	lprintf(LogInfo, "Reading network topology in GraphML format from %s\n", globalParams->srcFile ? globalParams->srcFile : "<stdin>");

	gmlContext ctx = {
		.finishedNodes = false,
		.ignoreNodes = false,
		.ignoreEdges = false,
		.nextId = 0,
		.macAddrIter = { .octets = { 0 } },
	};
	macNextAddr(&ctx.macAddrIter); // Skip all-zeroes address (unassignable)
	ctx.gmlToState = g_hash_table_new_full(&g_str_hash, &g_str_equal, &gmlFreeData, &gmlFreeData);

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
			err = gmlParseFile(globalParams->srcFile, &gmlAddNode, &gmlAddLink, &ctx, gmlParams->clientType);
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
		err = gmlParse(stdin, &gmlAddNode, &gmlAddLink, &ctx, gmlParams->clientType);
	}

cleanup:
	g_hash_table_destroy(ctx.gmlToState);
	return err;
}
