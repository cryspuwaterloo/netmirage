// See the GOTCHAS file for common issues and misconceptions related to this
// module, or if the code stops working in a new kernel version.

#define _GNU_SOURCE

#include "net.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <dirent.h>
#include <fcntl.h>
#include <linux/ethtool.h>
#include <linux/limits.h>
#include <linux/netlink.h>
#include <linux/pkt_sched.h>
#include <linux/rtnetlink.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "log.h"
#include "netlink.h"

// The implementations in this module are highly specific to Linux.
// The code has been written to be compatible with the named network namespace
// approach defined by the iproute2 tools. For more information, see:
//   http://www.linuxfoundation.org/collaborate/workgroups/networking/iproute2

#include "net.inl"

static const char* NetNsDir             = "/var/run/netns";
static const char* CurrentNsFile        = "/proc/self/ns/net";
static const char* InitNsFile           = "/proc/1/ns/net";
static const char* PschedParamFile      = "/proc/net/psched";
static const char* ForwardingConfigFile = "/proc/sys/net/ipv4/ip_forward";

static char namespacePrefix[PATH_MAX];
static double pschedTicksPerMs = 1.0;

// Ensures that the network namespace folders exist and are mounted.
static int setupNamespaceEnvironment(void) {
	errno = 0;
	if (mkdir(NetNsDir, 0755) != 0 && errno != EEXIST) {
		lprintf(LogError, "Could not create the system network namespace directory '%s': %s. Elevation may be required.\n", NetNsDir, strerror(errno));
		return errno;
	}

	// Mount the namespace directory in the same way as iproute2. This mounting
	// scheme allows namespaces to be freed sooner in some cases.
	bool createdMount = false;
	bool madeBind = false;
	while (!createdMount) {
		errno = 0;
		if (mount("", NetNsDir, "none", MS_SHARED | MS_REC, NULL) == 0) {
			lprintln(LogDebug, "Mounted system network namespace directory");
			createdMount = true;
		} else if (!madeBind && errno == EINVAL) {
			errno = 0;
			lprintln(LogDebug, "Bind mounting system network namespace directory");
			mount(NetNsDir, NetNsDir, "none", MS_BIND, NULL);
			madeBind = true;
		}
		if (errno != 0) {
			lprintf(LogError, "Could not mount the system network namespace directory '%s': %s. Elevation may be required.\n", NetNsDir, strerror(errno));
			return errno;
		}
	}

	return 0;
}

int netInit(const char* prefix) {
	strncpy(namespacePrefix, prefix, PATH_MAX-1);
	namespacePrefix[PATH_MAX-1] = '\0';

	for (const char* p = namespacePrefix; *p; ++p) {
		char c = *p;
		if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && c != '-' && c != '_') {
			lprintf(LogError, "The network namespace prefix may only contain Arabic numerals, Latin letters, hyphens, and underscores. Disallowed character: %c\n", c);
			return 1;
		}
	}

	// We only bother initializing this once per execution. The lifetime of the
	// process is such that if a race condition occurs, it's better to quit with
	// an error rather than attempting to remount.
	int err = setupNamespaceEnvironment();
	if (err != 0) return err;

	// Read the tick rate of the traffic control system. This is necessary
	// because some calls use ticks as a unit of time. The information is
	// exposed by the kernel in /proc/net/psched. The semantics of the data has
	// gone through several revisions over the years, and much of the original
	// meaning of parameters has been lost since Linux kernel version 2.6. We
	// use the new interpretation of the parameters, unlike tc.
	errno = 0;
	FILE* fd = fopen(PschedParamFile, "r");
	if (fd == NULL) {
		lprintf(LogError, "Could not open psched parameter file (%s): %s\n", PschedParamFile, strerror(errno));
		return errno;
	}
	uint32_t unused, nsPerTick;
	if (fscanf(fd, "%08x %08x ", &unused, &nsPerTick) < 2) {
		lprintf(LogError, "Failed to read psched parameter file (%s)\n", PschedParamFile);
	}
	fclose(fd);
	const double nsPerMs = 1000000.0;
	pschedTicksPerMs = nsPerMs / nsPerTick;

	nlInit();

	return 0;
}

void netCleanup(void) {
	nlCleanup();
}

// Computes the file path for a given namespace.
static int getNamespacePath(char* buffer, const char* name) {
	int res = snprintf(buffer, PATH_MAX, "%s/%s%s", NetNsDir, namespacePrefix, name);
	if (res < 0) return res;
	else if (res >= PATH_MAX) return -1;
	return 0;
}

// This implementation is meant to be compatible with the "ip netns add"
// command.
netContext* netOpenNamespace(const char* name, bool excl, int* err) {
	netContext* ctx = malloc(sizeof(netContext));
	int res = netOpenNamespaceInPlace(ctx, false, name, excl);
	if (res == 0) return ctx;

	free(ctx);
	if (err != NULL) *err = res;
	return NULL;
}

int netOpenNamespaceInPlace(netContext* ctx, bool reusing, const char* name, bool excl) {
	int err;

	char netNsPath[PATH_MAX];
	err = getNamespacePath(netNsPath, name);
	if (err != 0) return err;

	int nsFd;
	while (true) {
		if (!excl) {
			errno = 0;
			nsFd = open(netNsPath, O_RDONLY | O_CLOEXEC, 0);
			if (nsFd != -1) break;
		}

		nsFd = open(netNsPath, O_RDONLY | O_CLOEXEC | O_CREAT | (excl ? O_EXCL : 0), S_IRUSR | S_IRGRP | S_IROTH);
		if (nsFd == -1) {
			lprintf(LogError, "Failed to create network namespace file '%s': %s\n", netNsPath, strerror(errno));
			return errno;
		}
		close(nsFd);

		// Create a new network namespace (any forked processes will still use
		// the old one). This command implicitly switches us to the new
		// namespace.
		errno = 0;
		if (unshare(CLONE_NEWNET) != 0) {
			lprintf(LogError, "Failed to instantiate a new network namespace: %s\n", strerror(errno));
			goto abort;
		}

		// Bind mount the new namespace. This prevents it from closing until it
		// is explicitly unmounted; there is no need to keep a dedicated process
		// bound to it.
		errno = 0;
		if (mount(CurrentNsFile, netNsPath, "none", MS_BIND, NULL) != 0) {
			lprintf(LogError, "Failed to bind new network namespace file '%s': %s\n", netNsPath, strerror(errno));
			goto abort;
		}

		// We need to open the file descriptor again. The one from before the
		// bind mount is not a valid namespace descriptor.
		excl = false;
	}

	errno = nlNewContextInPlace(&ctx->nl);
	if (errno != 0) goto deleteAbort;

	// We need a socket descriptor specifically for ioctl in order to bind the
	// calls to the correct net namespace. ioctl does not accept namespace bind
	// file descriptors like the one stored in nsFd.
	int ioctlFd = socket(AF_PACKET, SOCK_RAW, 0);
	if (ioctlFd == -1) {
		lprintf(LogError, "Failed to open ioctl socket: %s\n", strerror(errno));
		goto freeDeleteAbort;
	}

	ctx->fd = nsFd;
	ctx->ioctlFd = ioctlFd;
	lprintf(LogDebug, "Opened network namespace file at '%s' with context %p\n", netNsPath, ctx);
	return 0;
freeDeleteAbort:
	nlInvalidateContext(&ctx->nl);
deleteAbort:
	netDeleteNamespace(name);
	return errno;
abort:
	unlink(netNsPath);
	return errno;
}

void netCloseNamespace(netContext* ctx, bool inPlace) {
	netInvalidateContext(ctx);
	if (!inPlace) free(ctx);
}

void netInvalidateContext(netContext* ctx) {
	lprintf(LogDebug, "Releasing network context %p\n", ctx);
	close(ctx->fd);
	close(ctx->ioctlFd);
	nlInvalidateContext(&ctx->nl);
}

int netDeleteNamespace(const char* name) {
	char netNsPath[PATH_MAX];
	int res = getNamespacePath(netNsPath, name);
	if (res != 0) return res;

	lprintf(LogDebug, "Deleting network namespace file at '%s'\n", netNsPath);

	// Lazy unmount preserves the namespace if it is still being used by others
	errno = 0;
	if (umount2(netNsPath, MNT_DETACH) != 0) {
		lprintf(LogWarning, "Failed to unmount network namespace file '%s': %s\n", netNsPath, strerror(errno));
	}
	errno = 0;
	if (unlink(netNsPath) != 0) {
		lprintf(LogError, "Failed to delete network namespace file '%s': %s\n", netNsPath, strerror(errno));
		return errno;
	}

	return 0;
}

int netEnumNamespaces(netNsCallback callback, void* userData) {
	errno = 0;
	DIR* d = opendir(NetNsDir);
	if (d == NULL) return errno;

	size_t compareLen = strlen(namespacePrefix);
	struct dirent* ent;
	while ((ent = readdir(d)) != NULL) {
		if (strncmp(ent->d_name, namespacePrefix, compareLen) == 0) {
			const char* name = ent->d_name + compareLen;

			// Only report files
			bool isFile;
#ifdef _DIRENT_HAVE_D_TYPE
			isFile = (ent->d_type == DT_REG);
#else
			struct stat s;
			char netNsPath[PATH_MAX];
			errno = getNamespacePath(netNsPath, name);
			if (errno != 0) break;
			errno = lstat(netNsPath, &s);
			if (errno != 0) break;
			isFile = ((s.st_mode & S_IFMT) == S_IFREG);
#endif
			if (isFile) {
				errno = callback(name, userData);
				if (errno != 0) break;
			}
		}
	}
	int err = errno; // Must capture errors from readdir and loop interior

	closedir(d);
	return err;
}

int netSwitchNamespace(netContext* ctx) {
	errno = 0;
	int nsFd;
	if (ctx != NULL) {
		lprintf(LogDebug, "Switching to network namespace context %p\n", ctx);
		nsFd = ctx->fd;
	} else {
		lprintln(LogDebug, "Switching to default network namespace");
		nsFd = open(InitNsFile, O_RDONLY | O_CLOEXEC);
		if (nsFd == -1) {
			lprintf(LogError, "Failed to open init network namespace file: %s\n", strerror(errno));
			return errno;
		}
	}
	int res = setns(nsFd, CLONE_NEWNET);
	if (res != 0) {
		lprintf(LogError, "Failed to set active network namespace: %s\n", strerror(errno));
		return errno;
	}
	if (ctx == NULL) close(nsFd);
	return 0;
}

int netCreateVethPair(const char* name1, const char* name2, netContext* ctx1, netContext* ctx2, bool sync) {
	lprintf(LogDebug, "Creating virtual ethernet pair (%p:'%s', %p:'%s')\n", ctx1, name1, ctx2, name2);

	// The netlink socket used for this message does not matter, since we
	// explicitly provide namespace file descriptors as part of the request.
	nlContext* nl = &ctx1->nl;

	nlInitMessage(nl, RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL | (sync ? NLM_F_ACK : 0));

	struct ifinfomsg ifi = { .ifi_family = AF_UNSPEC, .ifi_change = UINT_MAX, .ifi_type = 0, .ifi_index = 0, .ifi_flags = 0 };
	nlBufferAppend(nl, &ifi, sizeof(ifi));

	nlPushAttr(nl, IFLA_IFNAME);
		nlBufferAppend(nl, name1, strlen(name1)+1);
	nlPopAttr(nl);

	nlPushAttr(nl, IFLA_NET_NS_FD);
		nlBufferAppend(nl, &ctx1->fd, sizeof(ctx1->fd));
	nlPopAttr(nl);

	nlPushAttr(nl, IFLA_LINKINFO);
		nlPushAttr(nl, IFLA_INFO_KIND);
			nlBufferAppend(nl, "veth", 4);
		nlPopAttr(nl);
		nlPushAttr(nl, IFLA_INFO_DATA);
			nlPushAttr(nl, 1); // VETH_INFO_PEER
				nlBufferAppend(nl, &ifi, sizeof(ifi));
				nlPushAttr(nl, IFLA_IFNAME);
					nlBufferAppend(nl, name2, strlen(name2)+1);
				nlPopAttr(nl);
				nlPushAttr(nl, IFLA_NET_NS_FD);
					nlBufferAppend(nl, &ctx2->fd, sizeof(ctx2->fd));
				nlPopAttr(nl);
			nlPopAttr(nl);
		nlPopAttr(nl);
	nlPopAttr(nl);

	return nlSendMessage(nl, sync, NULL, NULL);
}

static void initIfReq(struct ifreq* ifr) {
	// The kernel ignores unnecessary fields, so this is only useful for debug
	// builds that are being profiled for pointers to unallocated data
#ifdef DEBUG
	memset(ifr, 0xCE, sizeof(struct ifreq));
#endif
}

static int sendIoCtl(netContext* ctx, const char* name, unsigned long command, struct ifreq* ifr) {
	errno = 0;
	int res = ioctl(ctx->ioctlFd, command, ifr);
	if (res == -1) {
		lprintf(LogError, "Error for ioctl command %d on interface %p:'%s': %s\n", command, ctx, name, strerror(errno));
		return errno;
	}
	return 0;
}

static int setSendIoCtl(netContext* ctx, const char* name, unsigned long command, void* data, struct ifreq* ifr) {
	struct ifreq ifrLocal;
	if (ifr == NULL) {
		ifr = &ifrLocal;
		initIfReq(&ifrLocal);
	}

	strncpy(ifr->ifr_name, name, IFNAMSIZ);
	ifr->ifr_name[IFNAMSIZ-1] = '\0';
	ifr->ifr_data = data;

	return sendIoCtl(ctx, name, command, ifr);
}

int netGetInterfaceIndex(netContext* ctx, const char* name, int* err) {
	struct ifreq ifr;
	initIfReq(&ifr);
	int res = setSendIoCtl(ctx, name, SIOCGIFINDEX, NULL, &ifr);
	if (res != 0) return res;

	lprintf(LogDebug, "Interface %p:'%s' has index %d\n", ctx, name, ifr.ifr_ifindex);
	return ifr.ifr_ifindex;
}

int netAddInterfaceAddrIPv4(netContext* ctx, int devIdx, uint32_t addr, uint8_t subnetBits, uint32_t broadcastAddr, uint32_t anycastAddr, bool sync) {
	lprintf(LogDebug, "Adding address to %p:%d: %08x/%u, broadcast %08x, anycast %08x\n", ctx, devIdx, addr, subnetBits, broadcastAddr, anycastAddr);

	// Using netlink for this task takes about 70% of the time that ioctl
	// requires to set the address, subnet, and broadcast address.

	if (subnetBits > 32) subnetBits = 32;

	nlContext* nl = &ctx->nl;
	nlInitMessage(nl, RTM_NEWADDR, NLM_F_CREATE | NLM_F_REPLACE | (sync ? NLM_F_ACK : 0));

	struct ifaddrmsg ifa = { .ifa_family = AF_INET, .ifa_prefixlen = subnetBits, .ifa_index = (unsigned int)devIdx, .ifa_flags = 0, .ifa_scope = 0 };
	nlBufferAppend(nl, &ifa, sizeof(ifa));

	if (addr > 0) {
		nlPushAttr(nl, IFA_LOCAL);
			nlBufferAppend(nl, &addr, sizeof(addr));
		nlPopAttr(nl);
		nlPushAttr(nl, IFA_ADDRESS);
			nlBufferAppend(nl, &addr, sizeof(addr));
		nlPopAttr(nl);
	}
	if (broadcastAddr > 0) {
		nlPushAttr(nl, IFA_BROADCAST);
			nlBufferAppend(nl, &addr, sizeof(addr));
		nlPopAttr(nl);
	}
	if (anycastAddr > 0) {
		nlPushAttr(nl, IFA_ANYCAST);
			nlBufferAppend(nl, &addr, sizeof(addr));
		nlPopAttr(nl);
	}

	return nlSendMessage(nl, sync, NULL, NULL);
}

int netDelInterfaceAddrIPv4(netContext* ctx, int devIdx, bool sync) {
	lprintf(LogDebug, "Deleting address from %p:%d\n", ctx, devIdx);

	nlContext* nl = &ctx->nl;
	nlInitMessage(nl, RTM_DELADDR, sync ? NLM_F_ACK : 0);

	struct ifaddrmsg ifa = {.ifa_family = AF_INET, .ifa_index = (unsigned int)devIdx, .ifa_prefixlen = 0, .ifa_flags = 0, .ifa_scope = 0 };
	nlBufferAppend(nl, &ifa, sizeof(ifa));

	return nlSendMessage(nl, true, NULL, NULL);
}

int netSetInterfaceUp(netContext* ctx, const char* name, bool up) {
	lprintf(LogDebug, "Bringing %s interface %p:'%s'\n", up ? "up" : "down", ctx, name);

	struct ifreq ifr;
	initIfReq(&ifr);
	int res = setSendIoCtl(ctx, name, SIOCGIFFLAGS, NULL, &ifr);
	if (res != 0) return res;

	if (up) ifr.ifr_flags |= IFF_UP;
	else ifr.ifr_flags &= ~IFF_UP;

	res = sendIoCtl(ctx, name, SIOCSIFFLAGS, &ifr);
	if (res != 0) return res;

	return 0;
}

int netSetInterfaceGro(netContext* ctx, const char* name, bool enabled) {
	lprintf(LogDebug, "Turning %s generic receive offload for interface %p:'%s'\n", enabled ? "on" : "off", ctx, name);

	struct ethtool_value ev;
	ev.cmd = ETHTOOL_SGRO;
	ev.data = enabled ? 1 : 0;

	int res = setSendIoCtl(ctx, name, SIOCETHTOOL, &ev, NULL);
	if (res != 0) return res;

	return 0;
}

int netSetEgressShaping(netContext* ctx, int devIdx, double delayMs, double jitterMs, double lossRate, double rateMbit, uint32_t queueLen, bool sync) {
	// Default queue length declared in tc (q_netem.c). Not in headers.
	const __u32 defaultQueueLen = 1000;

	// Sanitize
	if (lossRate < 0.0) lossRate = 0.0;
	else if (lossRate > 1.0) lossRate = 1.0;
	if (queueLen == 0) queueLen = defaultQueueLen;

	if (PASSES_LOG_THRESHOLD(LogDebug)) {
		lprintHead(LogDebug);
		lprintDirectf(LogDebug, "Setting egress shaping for interface %p:%d: delay %.0lfms, jitter %.0lfms, loss %.2lf", ctx, devIdx, delayMs, jitterMs, lossRate * 100.0);
		if (rateMbit != 0.0) {
			lprintDirectf(LogDebug, ", rate %.3lfMbit/s", rateMbit);
		}
		lprintDirectf(LogDebug, ", queue len %lu\n", queueLen);
	}

	// There are many ways to construct both latency and rate limiting with the
	// Linux Traffic Control utility. Historically, the standard approach was to
	// use HTB as the root qdisc and attach a leaf HTB class with a netem qdisc.
	// The HTB class would handle the rate limiting, and netem the latency. A
	// more recent approach was to use netem as the root qdisc and attach a TBF
	// qdisc as a direct child. However, since Linux kernel version 3.3, netem
	// has supported rate limiting on its own. Using netem directly for both
	// rate limiting and latency has both performance and accuracy advantages.

	nlContext* nl = &ctx->nl;
	nlInitMessage(nl, RTM_NEWQDISC, NLM_F_CREATE | NLM_F_REPLACE | (sync ? NLM_F_ACK : 0));

	struct tcmsg tcm = { .tcm_family = AF_UNSPEC, .tcm_ifindex = devIdx, .tcm_handle = 0x00010000, .tcm_parent = TC_H_ROOT, .tcm_info = 0 };
	nlBufferAppend(nl, &tcm, sizeof(tcm));

	nlPushAttr(nl, TCA_KIND);
		nlBufferAppend(nl, "netem", 6);
	nlPopAttr(nl);

	nlPushAttr(nl, TCA_OPTIONS);
		struct tc_netem_qopt opt = { .gap = 0, .duplicate = 0 };
		opt.latency = (__u32)llrint(delayMs * pschedTicksPerMs);
		opt.jitter = (__u32)llrint(jitterMs * pschedTicksPerMs);
		opt.limit = (queueLen > 0 ? queueLen : defaultQueueLen);
		opt.loss = (__u32)llrint(lossRate * UINT32_MAX);
		nlBufferAppend(nl, &opt, sizeof(opt));

		if (rateMbit > 0.0) {
			nlPushAttr(nl, TCA_NETEM_RATE);
				struct tc_netem_rate rate = { .packet_overhead = 0, .cell_size = 0, .cell_overhead = 0 };
				// Convert the rate from Mbit/s to byte/s
				rate.rate = (__u32)llrint(1000.0 * 1000.0 / 8.0 * rateMbit);
				nlBufferAppend(nl, &rate, sizeof(rate));
			nlPopAttr(nl);
		}
	nlPopAttr(nl);

	return nlSendMessage(nl, sync, NULL, NULL);
}

int netSetForwarding(bool enabled) {
	lprintf(LogDebug, "Turning %s IP forwarding (routing) for the active namespace\n", enabled ? "on" : "off");

	errno = 0;
	int fd = open(ForwardingConfigFile, O_WRONLY);
	if (fd == -1) return errno;

	ssize_t res;
	do {
		errno = 0;
		res = write(fd, enabled ? "1" : "0", 1);
		if (res == -1) goto abort;
	} while (res < 1);

	errno = 0;
abort:
	close(fd);
	return errno;
}

int netAddRoute(netContext* ctx, uint32_t dstAddr, uint8_t subnetBits, uint32_t gatewayAddr, int dstDevIdx, bool sync) {
	lprintf(LogDebug, "Adding route for namespace %p: %08x/%u ==> interface %d via %sgateway %08X\n", ctx, dstAddr, subnetBits, dstDevIdx, gatewayAddr == 0 ? "(disabled) " : "", gatewayAddr);

	nlContext* nl = &ctx->nl;
	nlInitMessage(nl, RTM_NEWROUTE, NLM_F_CREATE | NLM_F_EXCL | (sync ? NLM_F_ACK : 0));

	struct rtmsg rtm = { .rtm_src_len = 0, .rtm_tos = 0, .rtm_flags = 0 };
	rtm.rtm_family = AF_INET;
	rtm.rtm_dst_len = subnetBits;
	rtm.rtm_table = RT_TABLE_MAIN;
	rtm.rtm_protocol = RTPROT_STATIC;
	rtm.rtm_scope = RT_SCOPE_UNIVERSE;
	rtm.rtm_type = RTN_UNICAST;
	nlBufferAppend(nl, &rtm, sizeof(rtm));

	nlPushAttr(nl, RTA_DST);
		nlBufferAppend(nl, &dstAddr, sizeof(dstAddr));
	nlPopAttr(nl);

	if (gatewayAddr != 0) {
		nlPushAttr(nl, RTA_GATEWAY);
			nlBufferAppend(nl, &gatewayAddr, sizeof(gatewayAddr));
		nlPopAttr(nl);
	}

	nlPushAttr(nl, RTA_OIF);
		nlBufferAppend(nl, &dstDevIdx, sizeof(dstDevIdx));
	nlPopAttr(nl);

	return nlSendMessage(nl, sync, NULL, NULL);
}
