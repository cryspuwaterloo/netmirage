#define _GNU_SOURCE

#include "worker.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <grp.h>
#include <pwd.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ip.h"
#include "log.h"
#include "mem.h"
#include "net.h"
#include "netcache.h"
#include "ovs.h"
#include "topology.h"

static char ovsDir[PATH_MAX+1] = {0};
static char ovsSchema[PATH_MAX+1] = {0};

static netCache* nc;

// We keep these outside of the cache because they are used frequently:
static netContext* defaultNet;
static netContext* rootNet;

// We need to have two IP addresses for the root due to policy routing problems
// in kernel 3 (see workerAddClientRoutes for details)
static ip4Addr rootIpSelf;
static ip4Addr rootIpOther;

static ovsContext* rootSwitch;

// Converts a node identifier into a namespace name. buffer should be large
// enough to hold the identifier in decimal representation and the NUL
// terminator.
static void idToNsName(nodeId id, char* buffer) {
	sprintf(buffer, "%u", id);
}

static const char* RootName = "root";

// Must be at most (INTERFACE_BUF_LEN-MAX_NODE_ID_BUFLEN) characters (4)
static const char* SelfLinkPrefix = "self";
static const char* RootLinkPrefix = "root";
static const char* NodeLinkPrefix = "node";

static const char* RootBridgeName = "netmirage-br0";

// Arbitrary values, but chosen to leave the user plenty of space to customize
static const uint8_t CustomTableId = 120;
static const uint32_t CustomTablePriority = 9999;
static const uint32_t OvsPriorityArp = (1 << 15) - 100;
static const uint32_t OvsPrioritySelf = 1 << 14;
static const uint32_t OvsPriorityIn = 1 << 13;
static const uint32_t OvsPriorityOut = 1 << 7;

#define MAC_CLIENT_SELF  0
#define MAC_ROOT_SELF    1
#define MAC_CLIENT_OTHER 2
#define MAC_ROOT_OTHER   3

bool workerHaveCap(void) {
	// Capability checks are insufficient. The worker needs to run as UID 0
	// (root) in order to manipulate the network namespace files.
	return (getuid() == 0);
}

int workerInit(const char* nsPrefix, const char* ovsDirArg, const char* ovsSchemaArg, uint64_t softMemCap) {
	if (!workerHaveCap()) {
		lprintln(LogError, "BUG: attempted to start a worker thread with insufficient capabilities!");
		return 1;
	}

	char* ovsVer = ovsVersion();
	if (ovsVer == NULL) {
		lprintln(LogError, "Open vSwitch is not installed, is not accessible, or was not recognized. Ensure that Open vSwitch is installed and is accessible using the system PATH.");
		return 1;
	}
	lprintf(LogDebug, "Using Open vSwitch version '%s'\n", ovsVer);
	free(ovsVer);

	strncpy(ovsDir, ovsDirArg, PATH_MAX+1);
	if (ovsSchemaArg != NULL) strncpy(ovsSchema, ovsSchemaArg, PATH_MAX+1);

	nc = ncNewCache(softMemCap);
	int err = netInit(nsPrefix);
	if (err != 0) return err;
	defaultNet = netOpenNamespace(NULL, false, false, &err);
	if (defaultNet == NULL) return err;

	return 0;
}

int workerCleanup(void) {
	netCloseNamespace(defaultNet, false);
	if (rootNet) netCloseNamespace(rootNet, false);
	netCleanup();
	ncFreeCache(nc);
	return 0;
}

int workerGetEdgeRemoteMac(const char* intfName, ip4Addr ip, macAddr* edgeRemoteMac) {
	int res = netSwitchNamespace(defaultNet);
	if (res != 0) return res;

	char ipStr[IP4_ADDR_BUFLEN];
	ipStr[0] = '\0';
	for (int attempt = 0; attempt < 3; ++attempt) {
		res = netGetRemoteMacAddr(defaultNet, intfName, ip, edgeRemoteMac);
		if (res == 0) return 0;
		if (res != EAGAIN) return res;

		// The easiest way to force the kernel to update the ARP table is to
		// send a packet to the remote host. We'll use an ICMP echo request.
		if (attempt == 0) {
			ip4AddrToString(ip, ipStr);
		} else {
			// Don't spam
			sleep(1);
		}
		errno = 0;
		pid_t pingPid = fork();
		if (pingPid == -1) {
			lprintf(LogError, "Could not fork to ping: %s\n", strerror(errno));
			return errno;
		}
		if (pingPid == 0) {
			fclose(stdin);
			fclose(stdout);
			fclose(stderr);
			execlp("ping", "ping", "-c", "1", "-I", intfName, ipStr, NULL);
			exit(1);
		}
		int pingStatus;
		waitpid(pingPid, &pingStatus, 0);
		if (!WIFEXITED(pingStatus) || WEXITSTATUS(pingStatus) != 0) {
			lprintf(LogWarning, "Failed to ping edge node with IP %s on interface '%s'. Exit code: %d\n", ipStr, intfName, WEXITSTATUS(pingStatus));
		}
	}
	return 1;
}

int workerGetEdgeLocalMac(const char* intfName, macAddr* edgeLocalMac) {
	return netGetLocalMacAddr(rootNet, intfName, edgeLocalMac);
}

static int applyNamespaceParams(void) {
	int err = netSetForwarding(true);
	if (err != 0) return err;

	err = netSetMartians(true);
	if (err != 0) return err;

	err = netSetIPv6(false);
	return err;
}

static int applyInterfaceParams(netContext* net, const char* intfName, ip4Addr addr, int* idx) {
	int err;
	*idx = netGetInterfaceIndex(net, intfName, &err);
	if (*idx == -1) return err;

	err = netSetInterfaceGro(net, intfName, false);
	if (err != 0) return err;

	err = netAddInterfaceAddrIPv4(net, *idx, addr, 0, 0, 0, true);
	if (err != 0) return err;

	err = netSetInterfaceUp(net, intfName, true);
	if (err != 0) return err;

	return 0;
}

static int buildVethPair(netContext* sourceNet, netContext* targetNet,
		const char* sourceIntf, const char* targetIntf,
		ip4Addr sourceIp, ip4Addr targetIp,
		const macAddr* sourceMac, const macAddr* targetMac,
		int* sourceIntfIdx, int* targetIntfIdx) {

	int err = netCreateVethPair(sourceIntf, targetIntf, sourceNet, targetNet, sourceMac, targetMac, true);
	if (err != 0) return err;

	err = applyInterfaceParams(sourceNet, sourceIntf, sourceIp, sourceIntfIdx);
	if (err != 0) return err;
	err = applyInterfaceParams(targetNet, targetIntf, targetIp, targetIntfIdx);
	if (err != 0) return err;

	err = netAddStaticArp(sourceNet, sourceIntf, targetIp, targetMac);
	if (err != 0) return err;
	err = netAddStaticArp(targetNet, targetIntf, sourceIp, sourceMac);
	if (err != 0) return err;

	return 0;
}

// If existing is true, then we assume that the root instance has already been
// created and attempt to set up contexts for it. If existing is false, then the
// root namespace is created and configured. If existing is true but this worker
// has already created the namespace, then the command is ignored. This is
// useful because we can instruct a single worker to create the namespace, and
// tell all others to use the same one.
int workerAddRoot(ip4Addr addrSelf, ip4Addr addrOther, bool existing) {
	if (existing && rootNet != NULL) {
		lprintln(LogDebug, "Root creation command ignored because we created the namespace earlier");
		return 0;
	}

	lprintf(LogDebug, "Creating a private 'root' namespace\n");

	int err = 0;
	rootNet = netOpenNamespace(RootName, !existing, !existing, &err);
	if (rootNet == NULL) return err;

	if (!existing) {
		err = applyNamespaceParams();
		if (err != 0) return err;
	}

	rootIpSelf = addrSelf;
	rootIpOther = addrOther;

	const char* schemaPath = ovsSchema;
	if (schemaPath[0] == '\0') schemaPath = NULL;
	rootSwitch = ovsStart(rootNet, ovsDir, schemaPath, existing, &err);
	if (rootSwitch == NULL) return err;

	if (!existing) {
		err = ovsAddBridge(rootSwitch, RootBridgeName);
		if (err != 0) return err;

		// Reject everything initially, but switch ARP normally
		err = ovsClearFlows(rootSwitch, RootBridgeName);
		if (err != 0) return err;

		err = netSetInterfaceUp(rootNet, RootBridgeName, true);
		if (err != 0) return err;
	}

	return 0;
}

static int addArpResponses(ip4Addr addr, void* userData) {
	return ovsAddArpResponse(rootSwitch, RootBridgeName, addr, (const macAddr*)userData, OvsPriorityArp);
}

int workerAddEdgeInterface(const char* intfName) {
	lprintf(LogDebug, "Adding external interface '%s' to the switch in the root namespace\n", intfName);

	int err;

	int intfIdx = netGetInterfaceIndex(defaultNet, intfName, &err);
	if (intfIdx == -1) return err;

	err = netMoveInterface(defaultNet, intfName, intfIdx, rootNet, &intfIdx);
	if (err != 0) return err;

	err = netSetInterfaceUp(rootNet, intfName, true);
	if (err != 0) return err;

	err = ovsAddPort(rootSwitch, RootBridgeName, intfName);
	if (err != 0) return err;

	macAddr intfMac;
	err = netGetLocalMacAddr(rootNet, intfName, &intfMac);
	if (err != 0) return err;

	err = netEnumAddresses(&addArpResponses, rootNet, intfIdx, &intfMac);
	if (err != 0) return err;

	return 0;
}

static void sprintRootSelfIntf(char* buf, nodeId id) {
	sprintf(buf, "%s-%u", SelfLinkPrefix, id);
}

static void sprintRootUpIntf(char* buf, nodeId id) {
	sprintf(buf, "%s-%u", NodeLinkPrefix, id);
}

int workerAddHost(nodeId id, ip4Addr ip, macAddr macs[], const TopoNode* node) {
	char nodeName[MAX_NODE_ID_BUFLEN];
	idToNsName(id, nodeName);

	lprintf(LogDebug, "Creating host %s\n", nodeName);

	int err;
	netContext* net = ncOpenNamespace(nc, id, nodeName, true, true, &err);
	if (net == NULL) return err;

	err = applyNamespaceParams();
	if (err != 0) return err;

	if (node->client) {
		lprintf(LogDebug, "Connecting host %s to root for edge node connectivity\n", nodeName);

		char intfBuf[INTERFACE_BUF_LEN];
		sprintRootSelfIntf(intfBuf, id);

		int sourceIntfIdx, targetIntfIdx;

		// Self link (used for intra-client communication)
		err = buildVethPair(net, rootNet, SelfLinkPrefix, intfBuf, ip, rootIpSelf, &macs[MAC_CLIENT_SELF], &macs[MAC_ROOT_SELF], &sourceIntfIdx, &targetIntfIdx);
		if (err != 0) return err;
		// We don't apply shaping to the self link until we read a reflexive
		// edge from the input file (handled in workAddLink). However, we add
		// the link immediately so that traffic can flow between clients in the
		// same edge node even if no reflexive edge is present in the topology.

		sprintRootUpIntf(intfBuf, id);

		// Up / down link (used for inter-client communication)
		err = buildVethPair(net, rootNet, RootLinkPrefix, intfBuf, ip, rootIpOther, &macs[MAC_CLIENT_OTHER], &macs[MAC_ROOT_OTHER], &sourceIntfIdx, &targetIntfIdx);
		if (err != 0) return err;

		err = netSetEgressShaping(net, sourceIntfIdx, 0, 0, node->packetLoss, node->bandwidthDown, 0, true);
		if (err != 0) return err;
		err = netSetEgressShaping(rootNet, targetIntfIdx, 0, 0, node->packetLoss, node->bandwidthUp, 0, true);
		if (err != 0) return err;
	}

	return 0;
}

int workerSetSelfLink(nodeId id, const TopoLink* link) {
	char nodeName[MAX_NODE_ID_BUFLEN];
	idToNsName(id, nodeName);

	lprintf(LogDebug, "Applying self traffic shaping to client host %s\n", nodeName);

	int err;
	netContext* net = ncOpenNamespace(nc, id, nodeName, false, false, &err);
	if (net == NULL) return err;

	int intfIdx = netGetInterfaceIndex(net, SelfLinkPrefix, &err);
	if (intfIdx == -1) return err;

	// We apply the whole shaping in one direction in order to respect jitter
	return netSetEgressShaping(net, intfIdx, link->latency, link->jitter, link->packetLoss, 0.0, link->queueLen, true);
}

static int workGetLinkEndpoints(nodeId id1, nodeId id2, char* name1, char* name2, netContext** net1, netContext** net2, char* intf1, char* intf2) {
	idToNsName(id1, name1);
	idToNsName(id2, name2);

	int err;
	*net1 = ncOpenNamespace(nc, id1, name1, false, false, &err);
	if (*net1 == NULL) return err;
	*net2 = ncOpenNamespace(nc, id2, name2, false, false, &err);
	if (*net2 == NULL) return err;

	sprintf(intf1, "%s-%u", NodeLinkPrefix, id2);
	sprintf(intf2, "%s-%u", NodeLinkPrefix, id1);

	return 0;
}

int workerEnsureSystemScaling(uint64_t linkCount, nodeId nodeCount, nodeId clientNodes) {
	lprintf(LogDebug, "Preparing system to handle %u nodes (%u clients) and %lu links\n", nodeCount, clientNodes, linkCount);

	int err;
	err = netSwitchNamespace(defaultNet);
	if (err != 0) return err;

	// Ensure that the system-wide ARP hash table is large enough to hold static
	// routing entries for every interface in the network.

	int arpThresh1, arpThresh2, arpThresh3;
	err = netGetArpTableSize(&arpThresh1, &arpThresh2, &arpThresh3);
	if (err != 0) return err;

	uint64_t fudgeFactor = 100; // In case a few extras are needed
	uint64_t neededArpEntries = 2LU * linkCount + 3LU * (uint64_t)clientNodes + fudgeFactor;
	if (neededArpEntries > INT_MAX) {
		lprintf(LogError, "The topology is too large. The kernel cannot support the required number of static ARP entries (%lu)\n", neededArpEntries);
		return 1;
	}

	lprintf(LogDebug, "Existing ARP GC thresholds were (%d, %d, %d)\n", arpThresh1, arpThresh2, arpThresh3);

	if ((int)neededArpEntries > arpThresh2) {
		lprintf(LogWarning, "The system's ARP table size (garbage collection at %d entries) is too small to support this topology (expected entries %lu).\n", arpThresh2, neededArpEntries);

		int extraSpace = (int)neededArpEntries - arpThresh2;
		int newThresh1 = arpThresh1 + extraSpace;
		int newThresh2 = arpThresh2 + extraSpace;
		int newThresh3 = arpThresh3 + extraSpace;
		err = netSetArpTableSize(newThresh1, newThresh2, newThresh3);
		if (err != 0) {
			lprintln(LogError, "Could not modify the ARP table size to support the network topology.");
			return err;
		}
		lprintf(LogWarning, "The system's ARP thresholds have been set to (%d, %d, %d), which may degrade the performance of the system. After finishing the experiments, we recommend setting the values back to (%d, %d, %d).\n", newThresh1, newThresh2, newThresh3, arpThresh1, arpThresh2, arpThresh3);
	}

	return 0;
}

int workerAddLink(nodeId sourceId, nodeId targetId, ip4Addr sourceIp, ip4Addr targetIp, macAddr macs[], const TopoLink* link) {
	char sourceName[MAX_NODE_ID_BUFLEN];
	char targetName[MAX_NODE_ID_BUFLEN];
	netContext* sourceNet;
	netContext* targetNet;
	char sourceIntf[INTERFACE_BUF_LEN];
	char targetIntf[INTERFACE_BUF_LEN];

	int err;
	err = workGetLinkEndpoints(sourceId, targetId, sourceName, targetName, &sourceNet, &targetNet, sourceIntf, targetIntf);
	if (err != 0) return err;

	lprintf(LogDebug, "Creating virtual connection from host %s to host %s\n", sourceName, targetName);

	int sourceIntfIdx, targetIntfIdx;

	err = buildVethPair(sourceNet, targetNet, sourceIntf, targetIntf, sourceIp, targetIp, &macs[0], &macs[1], &sourceIntfIdx, &targetIntfIdx);
	if (err != 0) return err;

	err = netSetEgressShaping(sourceNet, sourceIntfIdx, link->latency, link->jitter, link->packetLoss, 0.0, link->queueLen, true);
	if (err != 0) return err;
	err = netSetEgressShaping(targetNet, targetIntfIdx, link->latency, link->jitter, link->packetLoss, 0.0, link->queueLen, true);
	if (err != 0) return err;

	err = netAddRoute(sourceNet, TableMain, ScopeLink, targetIp, 32, 0, sourceIntfIdx, true);
	if (err != 0) return err;
	err = netAddRoute(targetNet, TableMain, ScopeLink, sourceIp, 32, 0, targetIntfIdx, true);
	if (err != 0) return err;

	return 0;
}

int workerAddInternalRoutes(nodeId id1, nodeId id2, ip4Addr ip1, ip4Addr ip2, const ip4Subnet* subnet1, const ip4Subnet* subnet2) {
	char name1[MAX_NODE_ID_BUFLEN];
	char name2[MAX_NODE_ID_BUFLEN];
	netContext* net1;
	netContext* net2;
	char intf1[INTERFACE_BUF_LEN];
	char intf2[INTERFACE_BUF_LEN];

	int err;
	err = workGetLinkEndpoints(id1, id2, name1, name2, &net1, &net2, intf1, intf2);
	if (err != 0) return err;

	if (PASSES_LOG_THRESHOLD(LogDebug)) {
		char ip1Str[IP4_ADDR_BUFLEN];
		char ip2Str[IP4_ADDR_BUFLEN];
		char subnet1Str[IP4_CIDR_BUFLEN];
		char subnet2Str[IP4_CIDR_BUFLEN];
		ip4AddrToString(ip1, ip1Str);
		ip4AddrToString(ip2, ip2Str);
		ip4SubnetToString(subnet1, subnet1Str);
		ip4SubnetToString(subnet2, subnet2Str);
		lprintf(LogDebug, "Adding internal routes from %u / %s (for %s) to %u / %s (for %s)\n", id1, ip1Str, subnet1Str, id2, ip2Str, subnet2Str);
	}

	int intfIdx1 = netGetInterfaceIndex(net1, intf1, &err);
	if (intfIdx1 == -1) return err;
	int intfIdx2 = netGetInterfaceIndex(net2, intf2, &err);
	if (intfIdx2 == -1) return err;

	// We allow "route exists" errors because we might try to add routes to the
	// same internal nodes multiple times. In practice, this is rare.
	err = netAddRoute(net1, TableMain, ScopeGlobal, subnet2->addr, subnet2->prefixLen, ip2, intfIdx1, true);
	if (err != 0 && err != EEXIST) return err;
	err = netAddRoute(net2, TableMain, ScopeGlobal, subnet1->addr, subnet1->prefixLen, ip1, intfIdx2, true);
	if (err != 0 && err != EEXIST) return err;

	return 0;
}

int workerAddClientRoutes(nodeId clientId, macAddr clientMacs[], const ip4Subnet* subnet, uint32_t edgePort, uint32_t clientPorts[]) {
	lprintf(LogDebug, "Adding routes to root namespace for client node %u\n", clientId);

	// We have two objectives: packets for the subnet from other clients must be
	// routed to the root namespace through the "down" link, while packets for
	// the subnet from within the same client must be routed to the root
	// namespace through the "self" link. The key difference is that the latter
	// packets will originate from the "self" interface (and thus we simply need
	// to reflect them back), whereas packets in the former case will come from
	// our node-* interfaces.

	// We accomplish these objectives using policy routing. We use a separate
	// high-priority routing table for packets in the subnet from the "self"
	// interface.

	char name[MAX_NODE_ID_BUFLEN];
	idToNsName(clientId, name);

	int err;
	netContext* net = ncOpenNamespace(nc, clientId, name, false, false, &err);
	if (net == NULL) return err;

	int downIdx = netGetInterfaceIndex(net, RootLinkPrefix, &err);
	if (downIdx == -1) return err;
	int selfIdx = netGetInterfaceIndex(net, SelfLinkPrefix, &err);
	if (selfIdx == -1) return err;

	// Default route for packets from other clients
	err = netAddRoute(net, TableMain, ScopeLink, rootIpOther, 32, 0, downIdx, true);
	if (err != 0) return err;
	err = netAddRoute(net, TableMain, ScopeGlobal, subnet->addr, subnet->prefixLen, rootIpOther, downIdx, true);
	if (err != 0) return err;

	// Alternative route for packets from within the same subnet
	err = netAddRuleForTable(net, subnet, SelfLinkPrefix, CustomTableId, CustomTablePriority, true);
	if (err != 0) return err;
	// In kernel 4, we would assign the root only one IP address. We would set
	// the link route to be through the self interface in the custom table, and
	// the up/down interface in the main table. However, kernel 3 will not parse
	// this link route and will lead to an "network unreachable" error when
	// adding the subnet route to the custom table. The workaround is to use two
	// addresses and to place both link routes in the main table.
	err = netAddRoute(net, TableMain, ScopeLink, rootIpSelf, 32, 0, selfIdx, true);
	if (err != 0) return err;
	err = netAddRouteToTable(net, CustomTableId, ScopeGlobal, subnet->addr, subnet->prefixLen, rootIpSelf, selfIdx, true);
	if (err != 0) return err;

	// At this point, the client namespace is fully set up. Now we add flow
	// rules to the root switch

	char intfBuf[INTERFACE_BUF_LEN];

	// Incoming "self" link for intra-client communication
	sprintRootSelfIntf(intfBuf, clientId);
	err = ovsAddPort(rootSwitch, RootBridgeName, intfBuf);
	if (err != 0) return err;
	err = ovsAddIpFlow(rootSwitch, RootBridgeName, edgePort, subnet, subnet, &clientMacs[MAC_ROOT_SELF], &clientMacs[MAC_CLIENT_SELF], clientPorts[0], OvsPrioritySelf);
	if (err != 0) return err;

	// Incoming uplink for inter-client communication
	sprintRootUpIntf(intfBuf, clientId);
	err = ovsAddPort(rootSwitch, RootBridgeName, intfBuf);
	if (err != 0) return err;
	err = ovsAddIpFlow(rootSwitch, RootBridgeName, edgePort, subnet, NULL, &clientMacs[MAC_ROOT_OTHER], &clientMacs[MAC_CLIENT_OTHER], clientPorts[1], OvsPriorityIn);
	if (err != 0) return err;

	return 0;
}

int workerAddEdgeRoutes(const ip4Subnet* edgeSubnet, uint32_t edgePort, const macAddr* edgeLocalMac, const macAddr* edgeRemoteMac) {
	if (PASSES_LOG_THRESHOLD(LogDebug)) {
		char macStr[MAC_ADDR_BUFLEN];
		char subnetStr[IP4_CIDR_BUFLEN];
		macAddrToString(edgeRemoteMac, macStr);
		ip4SubnetToString(edgeSubnet, subnetStr);
		lprintf(LogDebug, "Adding egression route to root namespace for edge node with MAC %s responsible for subnet %s\n", macStr, subnetStr);
	}

	// Outgoing downlink and "self" link
	return ovsAddIpFlow(rootSwitch, RootBridgeName, 0, NULL, edgeSubnet, edgeLocalMac, edgeRemoteMac, edgePort, OvsPriorityOut);
}

typedef struct workerMoveIntfDirective_s {
	int idx;
	char name[INTERFACE_BUF_LEN+1];
	struct workerMoveIntfDirective_s* next;
} workerMoveIntfDirective;

static int workerRestoreInterface(const char* name, int idx, void* userData) {
	// Ignore any interfaces that we may have created
	if (strcmp(name, "lo") == 0) return 0;
	if (strcmp(name, RootBridgeName) == 0) return 0;
	if (strncmp(name, "ovs-", 4) == 0) return 0;
	if (strncmp(name, SelfLinkPrefix, strlen(SelfLinkPrefix)) == 0 && name[strlen(SelfLinkPrefix)] == '-') return 0;
	if (strncmp(name, NodeLinkPrefix, strlen(NodeLinkPrefix)) == 0 && name[strlen(NodeLinkPrefix)] == '-') return 0;

	// We cannot perform the move within the callback because this would result
	// in nested netlink calls. Instead, we save a linked list of interfaces to
	// move.
	workerMoveIntfDirective** intfToMove = userData;
	workerMoveIntfDirective* node = emalloc(sizeof(workerMoveIntfDirective));
	node->idx = idx;
	strncpy(node->name, name, INTERFACE_BUF_LEN);
	node->name[INTERFACE_BUF_LEN] = '\0';
	node->next = *intfToMove;
	*intfToMove = node;
	return 0;
}

static int workerDestroyNamespace(const char* name, void* userData) {
	uint32_t* deletedHosts = userData;
	if (deletedHosts != NULL) ++*deletedHosts;
	return netDeleteNamespace(name);
}

int workerDestroyHosts(void) {
	ovsDestroy(ovsDir);

	// We need to manually move external interfaces out of the root namespace.
	// While the kernel will do this automatically when the namespace is
	// deleted, this will cause all interface properties (e.g., IP addresses) to
	// be lost. Our function preserves these properties for convenience. We can
	// identify the root namespace without using the namespace prefix because we
	// ensure that the naming of the root cannot possible conflict with the
	// naming of node namespaces.
	netContext* ctx = netOpenNamespace(RootName, false, false, NULL);
	if (ctx != NULL) {
		workerMoveIntfDirective* intfToMove = NULL;
		int err = netEnumInterfaces(&workerRestoreInterface, ctx, &intfToMove);
		if (err != 0) {
			lprintf(LogWarning, "An error occurred while listing the interfaces for the previously created root network namespace. You may need to reconfigure physical network interfaces to restore edge node connectivity. Error code: %d\n", err);
		} else {
			while (intfToMove != NULL) {
				lprintf(LogDebug, "Restoring %p:'%s' (index %d) to default namespace\n", ctx, intfToMove->name, intfToMove->idx);
				if (netMoveInterface(ctx, intfToMove->name, intfToMove->idx, defaultNet, NULL) != 0) {
					lprintf(LogWarning, "Failed to restore interface '%s' to the default network namespace. You may need to reconfigure the interface's IP address so that edge nodes can be reached.\n", intfToMove->name);
				}

				workerMoveIntfDirective* prev = intfToMove;
				intfToMove = intfToMove->next;
				free(prev);
			}
		}
		netCloseNamespace(ctx, false);
	}

	uint32_t deletedHosts = 0;
	int res = netEnumNamespaces(&workerDestroyNamespace, &deletedHosts);
	if (deletedHosts > 0) {
		lprintf(LogInfo, "Destroyed an existing virtual network with %lu hosts\n", deletedHosts);
	}
	return res;
}
