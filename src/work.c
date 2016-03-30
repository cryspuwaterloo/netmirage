#include "work.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sys/capability.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ip.h"
#include "log.h"
#include "net.h"
#include "netcache.h"
#include "ovs.h"
#include "topology.h"

// TODO: preliminary implementation of this module is single-threaded

static const char* ovsDir;
static const char* ovsSchema;

static netCache* nc;

// We keep these outside of the cache because they are used frequently:
static netContext* defaultNet;
static netContext* rootNet;
static ip4Addr rootIp;

static ovsContext* rootSwitch;

// Converts a node identifier into a namespace name. buffer should be large
// enough to hold the identifier in decimal representation and the NUL
// terminator.
static void idToNsName(nodeId id, char* buffer) {
	sprintf(buffer, "%u", id);
}

static const char* RootName = "root";

// Must be at most (InterfaceBufLen-MAX_NODE_ID_BUFLEN) characters (4)
static const char* SelfLinkPrefix = "self";
static const char* RootLinkPrefix = "root";
static const char* NodeLinkPrefix = "node";

static const char* RootBridgeName = "sneac-br0";

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

int workInit(const char* nsPrefix, const char* ovsDirArg, const char* ovsSchemaArg, uint64_t softMemCap) {
	if (!CAP_IS_SUPPORTED(CAP_NET_ADMIN) || !CAP_IS_SUPPORTED(CAP_SYS_ADMIN)) {
		lprintf(LogError, "The system does not support the required capabilities.");
		return 1;
	}

	cap_t caps = cap_get_proc();
	if (caps == NULL) goto restricted;
	cap_flag_value_t capVal;
	if (cap_get_flag(caps, CAP_NET_ADMIN, CAP_EFFECTIVE, &capVal) == -1 || capVal != CAP_SET) goto restricted;
	if (cap_get_flag(caps, CAP_SYS_ADMIN, CAP_EFFECTIVE, &capVal) == -1 || capVal != CAP_SET) goto restricted;
	cap_free(caps);

	char* ovsVer = ovsVersion();
	if (ovsVer == NULL) {
		lprintln(LogError, "Open vSwitch is not installed, is not accessible, or was not recognized. Ensure that Open vSwitch is installed and is accessible using the system PATH.");
		return 1;
	}
	lprintf(LogDebug, "Using Open vSwitch version '%s'\n", ovsVer);
	free(ovsVer);

	ovsDir = ovsDirArg;
	ovsSchema = ovsSchemaArg;

	nc = ncNewCache(softMemCap);
	int err = netInit(nsPrefix);
	if (err != 0) return err;
	defaultNet = netOpenNamespace(NULL, false, &err);
	if (defaultNet == NULL) return err;
	return 0;
restricted:
	lprintln(LogError, "The worker process does not have authorization to perform its function. Please run the process with the CAP_NET_ADMIN and CAP_SYS_ADMIN capabilities (e.g., as root).");
	if (caps != NULL) cap_free(caps);
	return 1;
}

int workCleanup(void) {
	netCloseNamespace(defaultNet, false);
	if (rootNet) netCloseNamespace(rootNet, false);
	netCleanup();
	ncFreeCache(nc);
	return 0;
}

int workGetEdgeRemoteMac(const char* intfName, ip4Addr ip, macAddr* edgeRemoteMac) {
	int res = netSwitchNamespace(defaultNet);
	if (res != 0) return res;

	char ipStr[IP4_ADDR_BUFLEN];
	ipStr[0] = '\0';
	for (int attempt = 0; attempt < 5; ++attempt) {
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
			lprintf(LogWarning, "Failed to ping edge node with IP %s on interface '%s'. The edge node may be unreachable. Exit code: %d\n", ipStr, intfName, WEXITSTATUS(pingStatus));
		}
	}
	return 1;
}

int workGetEdgeLocalMac(const char* intfName, macAddr* edgeLocalMac) {
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

int workAddRoot(ip4Addr addr) {
	lprintf(LogDebug, "Creating a private 'root' namespace\n");

	int err;
	rootNet = netOpenNamespace(RootName, true, &err);
	if (rootNet == NULL) return err;

	err = applyNamespaceParams();
	if (err != 0) return err;

	rootIp = addr;

	rootSwitch = NULL;
	rootSwitch = ovsStart(rootNet, ovsDir, ovsSchema, &err);
	if (rootSwitch == NULL) return err;

	err = ovsAddBridge(rootSwitch, RootBridgeName);
	if (err != 0) return err;

	// Reject everything initially, but switch ARP normally
	err = ovsArpOnly(rootSwitch, RootBridgeName, OvsPriorityArp);
	if (err != 0) return err;

	return 0;
}

int workAddEdgeInterface(const char* intfName, uint32_t* portId) {
	lprintf(LogDebug, "Adding external interface '%s' to the switch in the root namespace\n", intfName);

	int err;

	int intfIdx = netGetInterfaceIndex(defaultNet, intfName, &err);
	if (intfIdx == -1) return err;

	err = netMoveInterface(defaultNet, intfIdx, rootNet, true);
	if (err != 0) return err;

	err = ovsAddPort(rootSwitch, RootBridgeName, intfName, portId);
	if (err != 0) return err;

	return 0;
}

static void sprintRootSelfIntf(char* buf, nodeId id) {
	sprintf(buf, "%s-%u", SelfLinkPrefix, id);
}

static void sprintRootUpIntf(char* buf, nodeId id) {
	sprintf(buf, "%s-%u", NodeLinkPrefix, id);
}

int workAddHost(nodeId id, ip4Addr ip, macAddr macs[], const TopoNode* node) {
	char nodeName[MAX_NODE_ID_BUFLEN];
	idToNsName(id, nodeName);

	lprintf(LogDebug, "Creating host %s\n", nodeName);

	int err;
	netContext* net = ncOpenNamespace(nc, id, nodeName, true, &err);
	if (net == NULL) return err;

	err = applyNamespaceParams();
	if (err != 0) return err;

	if (node->client) {
		lprintf(LogDebug, "Connecting host %s to root for edge node connectivity\n", nodeName);

		char intfBuf[InterfaceBufLen];
		sprintRootSelfIntf(intfBuf, id);

		int sourceIntfIdx, targetIntfIdx;

		// Self link (used for intra-client communication)
		err = buildVethPair(net, rootNet, SelfLinkPrefix, intfBuf, ip, rootIp, &macs[MAC_CLIENT_SELF], &macs[MAC_ROOT_SELF], &sourceIntfIdx, &targetIntfIdx);
		if (err != 0) return err;
		// We don't apply shaping to the self link until we read a reflexive
		// edge from the input file (handled in workAddLink). However, we add
		// the link immediately so that traffic can flow between clients in the
		// same edge node even if no reflexive edge is present in the topology.

		sprintRootUpIntf(intfBuf, id);

		// Up / down link (used for inter-client communication)
		err = buildVethPair(net, rootNet, RootLinkPrefix, intfBuf, ip, rootIp, &macs[MAC_CLIENT_OTHER], &macs[MAC_ROOT_OTHER], &sourceIntfIdx, &targetIntfIdx);
		if (err != 0) return err;

		err = netSetEgressShaping(net, sourceIntfIdx, 0, 0, node->packetLoss, node->bandwidthDown, 0, true);
		if (err != 0) return err;
		err = netSetEgressShaping(rootNet, targetIntfIdx, 0, 0, node->packetLoss, node->bandwidthUp, 0, true);
		if (err != 0) return err;
	}

	return 0;
}

int workSetSelfLink(nodeId id, const TopoLink* link) {
	char nodeName[MAX_NODE_ID_BUFLEN];
	idToNsName(id, nodeName);

	lprintf(LogDebug, "Applying self traffic shaping to client host %s\n", nodeName);

	int err;
	netContext* net = ncOpenNamespace(nc, id, nodeName, false, &err);
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
	*net1 = ncOpenNamespace(nc, id1, name1, false, &err);
	if (*net1 == NULL) return err;
	*net2 = ncOpenNamespace(nc, id2, name2, false, &err);
	if (*net2 == NULL) return err;

	sprintf(intf1, "%s-%u", NodeLinkPrefix, id2);
	sprintf(intf2, "%s-%u", NodeLinkPrefix, id1);

	return 0;
}

int workEnsureSystemScaling(uint64_t linkCount, nodeId nodeCount, nodeId clientNodes) {
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

int workAddLink(nodeId sourceId, nodeId targetId, ip4Addr sourceIp, ip4Addr targetIp, macAddr macs[], const TopoLink* link) {
	char sourceName[MAX_NODE_ID_BUFLEN];
	char targetName[MAX_NODE_ID_BUFLEN];
	netContext* sourceNet;
	netContext* targetNet;
	char sourceIntf[InterfaceBufLen];
	char targetIntf[InterfaceBufLen];

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

int workAddInternalRoutes(nodeId id1, nodeId id2, ip4Addr ip1, ip4Addr ip2, const ip4Subnet* subnet1, const ip4Subnet* subnet2) {
	char name1[MAX_NODE_ID_BUFLEN];
	char name2[MAX_NODE_ID_BUFLEN];
	netContext* net1;
	netContext* net2;
	char intf1[InterfaceBufLen];
	char intf2[InterfaceBufLen];

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

	err = netAddRoute(net1, TableMain, ScopeGlobal, subnet2->addr, subnet2->prefixLen, ip2, intfIdx1, true);
	if (err != 0) return err;
	err = netAddRoute(net2, TableMain, ScopeGlobal, subnet1->addr, subnet1->prefixLen, ip1, intfIdx2, true);
	if (err != 0) return err;

	return 0;
}

int workAddClientRoutes(nodeId clientId, macAddr clientMacs[], const ip4Subnet* subnet, uint32_t edgePort) {
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
	netContext* net = ncOpenNamespace(nc, clientId, name, false, &err);
	if (net == NULL) return err;

	int downIdx = netGetInterfaceIndex(net, RootLinkPrefix, &err);
	if (downIdx == -1) return err;
	int selfIdx = netGetInterfaceIndex(net, SelfLinkPrefix, &err);
	if (selfIdx == -1) return err;

	// Default route for packets from other clients
	err = netAddRoute(net, TableMain, ScopeLink, rootIp, 32, 0, downIdx, true);
	if (err != 0) return err;
	err = netAddRoute(net, TableMain, ScopeGlobal, subnet->addr, subnet->prefixLen, rootIp, downIdx, true);
	if (err != 0) return err;

	// Alternative route for packets from within the same subnet
	err = netAddRuleForTable(net, subnet, ScopeGlobal, SelfLinkPrefix, CustomTableId, CustomTablePriority, true);
	if (err != 0) return err;
	err = netAddRouteToTable(net, CustomTableId, ScopeLink, rootIp, 32, 0, selfIdx, true);
	if (err != 0) return err;
	err = netAddRouteToTable(net, CustomTableId, ScopeGlobal, subnet->addr, subnet->prefixLen, rootIp, selfIdx, true);
	if (err != 0) return err;

	// At this point, the client namespace is fully set up. Now we add flow
	// rules to the root switch

	char intfBuf[InterfaceBufLen];
	uint32_t portId;

	// Incoming "self" link for intra-client communication
	sprintRootSelfIntf(intfBuf, clientId);
	err = ovsAddPort(rootSwitch, RootBridgeName, intfBuf, &portId);
	if (err != 0) return err;
	err = ovsAddIpFlow(rootSwitch, RootBridgeName, edgePort, subnet, subnet, &clientMacs[MAC_ROOT_SELF], &clientMacs[MAC_CLIENT_SELF], portId, OvsPrioritySelf);
	if (err != 0) return err;

	// Incoming uplink for inter-client communication
	sprintRootUpIntf(intfBuf, clientId);
	err = ovsAddPort(rootSwitch, RootBridgeName, intfBuf, &portId);
	if (err != 0) return err;
	err = ovsAddIpFlow(rootSwitch, RootBridgeName, edgePort, subnet, NULL, &clientMacs[MAC_ROOT_OTHER], &clientMacs[MAC_CLIENT_OTHER], portId, OvsPriorityIn);
	if (err != 0) return err;

	return 0;
}

int workAddEdgeRoutes(const ip4Subnet* edgeSubnet, uint32_t edgePort, const macAddr* edgeLocalMac, const macAddr* edgeRemoteMac) {
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

int workJoin(void) {
	return 0;
}

static int workDestroyNamespace(const char* name, void* userData) {
	uint32_t* deletedHosts = userData;
	if (deletedHosts != NULL) ++*deletedHosts;
	return netDeleteNamespace(name);
}

int workDestroyHosts(uint32_t* deletedHosts) {
	ovsDestroy(ovsDir);
	if (deletedHosts != NULL) *deletedHosts = 0;
	return netEnumNamespaces(&workDestroyNamespace, deletedHosts);
}
