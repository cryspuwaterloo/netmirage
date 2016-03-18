#include "work.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "net.h"
#include "netcache.h"
#include "topology.h"

// TODO: preliminary implementation of this module is single-threaded

static netCache* nc;
static netContext* rootNet; // Outside of the cache because used frequently

// Converts a node identifier into a namespace name. buffer should be large
// enough to hold the namespace prefix, the identifier in decimal
// representation, and the NUL terminator.
static void idToNsName(nodeId id, char* buffer) {
	sprintf(buffer, "%u", id);
}

#define MAX_INTERFACE_BUFLEN (5+(MAX_NODE_ID_BUFLEN-1)+1+(MAX_NODE_ID_BUFLEN-1)+1)
static void interfaceName(nodeId sourceId, nodeId targetId, char* buffer) {
	sprintf(buffer, "veth-%u-%u", sourceId, targetId);
}

static const char* RootName = "root";

int workInit(const char* nsPrefix, uint64_t softMemCap) {
	nc = ncNewCache(softMemCap);
	rootNet = NULL;
	return netInit(nsPrefix);
}

int workCleanup() {
	if (rootNet) netCloseNamespace(rootNet, false);
	ncFreeCache(nc);
	return 0;
}

int workAddRoot() {
	lprintf(LogDebug, "Creating a private 'root' namespace\n");

	int err;
	rootNet = netOpenNamespace(RootName, true, &err);
	if (rootNet == NULL) return err;

	err = netSetForwarding(true);
	if (err != 0) return err;

	return 0;
}

int workAddHost(nodeId id, const TopoNode* node) {
	char nodeName[MAX_NODE_ID_BUFLEN];
	idToNsName(id, nodeName);

	lprintf(LogDebug, "Creating host %s\n", nodeName);

	int err;
	netContext* net = ncOpenNamespace(nc, id, nodeName, true, &err);
	if (net == NULL) return err;

	err = netSetForwarding(true);
	if (err != 0) return err;

	if (node->client) {
		lprintf(LogDebug, "Connecting host %s to root for edge node connectivity\n", nodeName);
	}

	return 0;
}

static int applyInterfaceParams(netContext* net, char* intfName, const TopoLink* link) {
	int err;
	int idx = netGetInterfaceIndex(net, intfName, &err);
	if (idx == -1) return err;

	err = netSetInterfaceGro(net, intfName, false);
	if (err != 0) return err;

	err = netSetEgressShaping(net, idx, link->latency, link->jitter, link->packetLoss, 0.0, link->queueLen, true);
	if (err != 0) return err;

	return 0;
}

int workAddLink(nodeId sourceId, nodeId targetId, const TopoLink* link) {
	char sourceName[MAX_NODE_ID_BUFLEN];
	char targetName[MAX_NODE_ID_BUFLEN];
	idToNsName(sourceId, sourceName);
	idToNsName(targetId, targetName);

	lprintf(LogDebug, "Creating virtual connection from host %s to host %s\n", sourceName, targetName);

	int err;
	netContext* sourceNet = ncOpenNamespace(nc, sourceId, sourceName, false, &err);
	if (sourceNet == NULL) return err;
	netContext* targetNet = ncOpenNamespace(nc, targetId, targetName, false, &err);
	if (targetNet == NULL) return err;

	char sourceIntf[MAX_INTERFACE_BUFLEN];
	char targetIntf[MAX_INTERFACE_BUFLEN];
	interfaceName(sourceId, targetId, sourceIntf);
	interfaceName(targetId, sourceId, targetIntf);

	err = netCreateVethPair(sourceIntf, targetIntf, sourceNet, targetNet, true);
	if (err != 0) return err;

	err = applyInterfaceParams(sourceNet, sourceIntf, link);
	if (err != 0) return err;
	err = applyInterfaceParams(targetNet, targetIntf, link);
	if (err != 0) return err;

	return 0;
}

int workJoin() {
	return 0;
}

static int workDestroyNamespace(const char* name, void* userData) {
	uint32_t* deletedHosts = userData;
	if (deletedHosts != NULL) ++*deletedHosts;
	return netDeleteNamespace(name);
}

int workDestroyHosts(uint32_t* deletedHosts) {
	if (deletedHosts != NULL) *deletedHosts = 0;
	return netEnumNamespaces(&workDestroyNamespace, deletedHosts);
}
