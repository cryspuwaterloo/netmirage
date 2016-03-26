#include "work.h"

#include <errno.h>
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
#include "topology.h"

// TODO: preliminary implementation of this module is single-threaded

const size_t NeededMacsClient = 4;
const size_t NeededMacsLink = 2;

static netCache* nc;

// We keep these outside of the cache because they are used frequently:
static netContext* defaultNet;
static netContext* rootNet;
static ip4Addr rootIp;

// Converts a node identifier into a namespace name. buffer should be large
// enough to hold the namespace prefix, the identifier in decimal
// representation, and the NUL terminator.
static void idToNsName(nodeId id, char* buffer) {
	sprintf(buffer, "%u", id);
}

static const char* RootName = "root";

// Must be at most (InterfaceBufLen-MAX_NODE_ID_BUFLEN) characters (4)
static const char* SelfLinkPrefix = "self";
static const char* RootLinkPrefix = "root";
static const char* NodeLinkPrefix = "node";

int workInit(const char* nsPrefix, uint64_t softMemCap) {
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

int workGetEdgeMac(const char* intfName, ip4Addr ip, macAddr* result) {
	int res = netSwitchNamespace(defaultNet);
	if (res != 0) return res;

	char ipStr[IP4_ADDR_BUFLEN];
	ipStr[0] = '\0';
	for (int attempt = 0; attempt < 5; ++attempt) {
		res = netGetRemoteMacAddr(defaultNet, intfName, ip, result);
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

	return 0;
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
		sprintf(intfBuf, "%s-%u", SelfLinkPrefix, id);

		int sourceIntfIdx, targetIntfIdx;

		err = buildVethPair(net, rootNet, SelfLinkPrefix, intfBuf, ip, rootIp, &macs[0], &macs[1], &sourceIntfIdx, &targetIntfIdx);
		if (err != 0) return err;
		// We don't apply shaping to the self link until we read a reflexive
		// edge from the input file (handled in workAddLink). However, we add
		// the link immediately so that traffic can flow between clients in the
		// same edge node even if no reflexive edge is present in the topology.

		sprintf(intfBuf, "%s-%u", NodeLinkPrefix, id);

		err = buildVethPair(net, rootNet, RootLinkPrefix, intfBuf, ip, rootIp, &macs[2], &macs[3], &sourceIntfIdx, &targetIntfIdx);
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

int workAddLink(nodeId sourceId, nodeId targetId, ip4Addr sourceIp, ip4Addr targetIp, macAddr macs[], const TopoLink* link) {
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

	char sourceIntf[InterfaceBufLen];
	char targetIntf[InterfaceBufLen];
	sprintf(sourceIntf, "%s-%u", NodeLinkPrefix, targetId);
	sprintf(targetIntf, "%s-%u", NodeLinkPrefix, sourceId);

	int sourceIntfIdx, targetIntfIdx;

	err = buildVethPair(sourceNet, targetNet, sourceIntf, targetIntf, sourceIp, targetIp, &macs[0], &macs[1], &sourceIntfIdx, &targetIntfIdx);
	if (err != 0) return err;

	err = netSetEgressShaping(sourceNet, sourceIntfIdx, link->latency, link->jitter, link->packetLoss, 0.0, link->queueLen, true);
	if (err != 0) return err;
	err = netSetEgressShaping(targetNet, targetIntfIdx, link->latency, link->jitter, link->packetLoss, 0.0, link->queueLen, true);
	if (err != 0) return err;

	err = netAddRoute(sourceNet, targetIp, 32, 0, sourceIntfIdx, true);
	if (err != 0) return err;
	err = netAddRoute(targetNet, sourceIp, 32, 0, targetIntfIdx, true);
	if (err != 0) return err;

	return 0;
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
	if (deletedHosts != NULL) *deletedHosts = 0;
	return netEnumNamespaces(&workDestroyNamespace, deletedHosts);
}
