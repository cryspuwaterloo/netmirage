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
#include <linux/fib_rules.h>
#include <linux/if_packet.h>
#include <linux/limits.h>
#include <linux/netlink.h>
#include <linux/pkt_sched.h>
#include <linux/rtnetlink.h>
#include <linux/sockios.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ip.h"
#include "log.h"
#include "mem.h"
#include "netlink.h"

// The implementations in this module are highly specific to Linux.
// The code has been written to be compatible with the named network namespace
// approach defined by the iproute2 tools. For more information, see:
//   http://www.linuxfoundation.org/collaborate/workgroups/networking/iproute2

#include "net.inl"

#define NET_NS_DIR        "/var/run/netns"
#define CURRENT_NS_FILE   "/proc/self/ns/net"
#define INIT_NS_FILE      "/proc/1/ns/net"
#define PSCHED_PARAM_FILE "/proc/net/psched"

#define SYSCTL_FORWARDING       "/proc/sys/net/ipv4/ip_forward"
#define SYSCTL_MARTIANS         "/proc/sys/net/ipv4/conf/all/rp_filter"
#define SYSCTL_MARTIANS_DEFAULT "/proc/sys/net/ipv4/conf/default/rp_filter"
#define SYSCTL_DISABLE_IPV6     "/proc/sys/net/ipv6/conf/all/disable_ipv6"
#define SYSCTL_ARP_GC_PREFIX    "/proc/sys/net/ipv4/neigh/default/gc_thresh"

static char namespacePrefix[PATH_MAX];
static double pschedTicksPerMs = 1.0;

#if INTERFACE_BUF_LEN != IFNAMSIZ
#error "Mismatch between internal interface name buffer length and the buffer length for this kernel."
#endif

// Ensures that the network namespace folders exist and are mounted.
static int setupNamespaceEnvironment(void) {
	errno = 0;
	if (mkdir(NET_NS_DIR, 0755) != 0 && errno != EEXIST) {
		lprintf(LogError, "Could not create the system network namespace directory '" NET_NS_DIR "': %s. Elevation may be required.\n", strerror(errno));
		return errno;
	}

	// Mount the namespace directory in the same way as iproute2. This mounting
	// scheme allows namespaces to be freed sooner in some cases.
	bool createdMount = false;
	bool madeBind = false;
	while (!createdMount) {
		errno = 0;
		if (mount("", NET_NS_DIR, "none", MS_SHARED | MS_REC, NULL) == 0) {
			lprintln(LogDebug, "Mounted system network namespace directory");
			createdMount = true;
		} else if (!madeBind && errno == EINVAL) {
			errno = 0;
			lprintln(LogDebug, "Bind mounting system network namespace directory");
			mount(NET_NS_DIR, NET_NS_DIR, "none", MS_BIND, NULL);
			madeBind = true;
		}
		if (errno != 0) {
			lprintf(LogError, "Could not mount the system network namespace directory '" NET_NS_DIR "': %s. Elevation may be required.\n", strerror(errno));
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
	FILE* fd = fopen(PSCHED_PARAM_FILE, "r");
	if (fd == NULL) {
		lprintf(LogError, "Could not open psched parameter file ('" PSCHED_PARAM_FILE "'): %s\n", strerror(errno));
		return errno;
	}
	uint32_t unused, nsPerTick;
	if (fscanf(fd, "%08x %08x ", &unused, &nsPerTick) < 2) {
		lprintln(LogError, "Failed to read psched parameter file ('" PSCHED_PARAM_FILE "')");
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
	int res = snprintf(buffer, PATH_MAX, NET_NS_DIR "/%s%s", namespacePrefix, name);
	if (res < 0) return res;
	else if (res >= PATH_MAX) return -1;
	return 0;
}

// This implementation is meant to be compatible with the "ip netns add"
// command.
netContext* netOpenNamespace(const char* name, bool create, bool excl, int* err) {
	netContext* ctx = emalloc(sizeof(netContext));
	int res = netOpenNamespaceInPlace(ctx, false, name, create, excl);
	if (res == 0) return ctx;

	free(ctx);
	if (err != NULL) *err = res;
	return NULL;
}

int netOpenNamespaceInPlace(netContext* ctx, bool reusing, const char* name, bool create, bool excl) {
	int err;

	const char* netNsPath;
	char pathBuffer[PATH_MAX];
	if (name == NULL) {
		netNsPath = INIT_NS_FILE;
	} else {
		err = getNamespacePath(pathBuffer, name);
		if (err != 0) return err;
		netNsPath = pathBuffer;
	}

	bool mustSwitch = true;
	int nsFd;
	while (true) {
		if (!excl) {
			errno = 0;
			nsFd = open(netNsPath, O_RDONLY | O_CLOEXEC, 0);
			if (nsFd != -1) break;
		}
		if (!create) {
			lprintf(LogDebug, "Namespace file '%s' does not exist and was not created\n", netNsPath);
			return 1;
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
		mustSwitch = false;

		// Bind mount the new namespace. This prevents it from closing until it
		// is explicitly unmounted; there is no need to keep a dedicated process
		// bound to it.
		errno = 0;
		if (mount(CURRENT_NS_FILE, netNsPath, "none", MS_BIND, NULL) != 0) {
			lprintf(LogError, "Failed to bind new network namespace file '%s': %s\n", netNsPath, strerror(errno));
			goto abort;
		}

		// We need to open the file descriptor again. The one from before the
		// bind mount is not a valid namespace descriptor.
		excl = false;

		lprintf(LogDebug, "Created network namespace mounted at '%s'\n", netNsPath);
	}

	// We have to switch if the namespace already existed and we just opened it.
	// Otherwise, ioctl will be bound to the wrong namespace.
	if (mustSwitch) {
		err = setns(nsFd, CLONE_NEWNET);
		if (err != 0) {
			lprintf(LogError, "Failed to switch to existing network namespace: %s\n", strerror(errno));
			goto freeDeleteAbort;
		}
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
	lprintf(LogDebug, "Opened network namespace file at '%s' with context %p%s\n", netNsPath, ctx, mustSwitch ? " (required switch)" : "");
	return 0;
freeDeleteAbort:
	nlInvalidateContext(&ctx->nl);
deleteAbort:
	if (name != NULL) netDeleteNamespace(name);
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
	DIR* d = opendir(NET_NS_DIR);
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
	lprintf(LogDebug, "Switching to network namespace context %p\n", ctx);
	int nsFd = ctx->fd;
	errno = 0;
	int res = setns(nsFd, CLONE_NEWNET);
	if (res != 0) {
		lprintf(LogError, "Failed to set active network namespace: %s\n", strerror(errno));
		return errno;
	}
	return 0;
}

typedef struct {
	netContext* netCtx;
	netIfCallback callback;
	void* userData;
} netEnumIntfContext;

static int netSendLinkCallback(const nlContext* ctx, const void* data, uint32_t len, uint16_t type, uint16_t flags, void* arg) {
	netEnumIntfContext* enumCtx = arg;
	const struct ifinfomsg* ifi = data;
	int idx = ifi->ifi_index;

	const char* intfName = NULL;
	const struct rtattr* rta = (const struct rtattr*)((const char*)data + NLMSG_ALIGN(sizeof(struct ifinfomsg)));
	for (; RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
		if (rta->rta_type == IFLA_IFNAME) {
			intfName = RTA_DATA(rta);
			break;
		}
	}
	if (intfName == NULL) {
		lprintf(LogWarning, "Interface enumeration ignored nameless interface %p:%d\n", enumCtx->netCtx, idx);
	} else {
		return enumCtx->callback(intfName, idx, enumCtx->userData);
	}
	return 0;
}

int netEnumInterfaces(netIfCallback callback, netContext* ctx, void* userData) {
	nlContext* nl = &ctx->nl;

	// Get link attributes
	netEnumIntfContext enumCtx = { .netCtx = ctx, .callback = callback, .userData = userData };
	struct ifinfomsg ifi = { .ifi_family = AF_UNSPEC, .ifi_index = 0, .ifi_type = 0, .ifi_flags = 0, .ifi_change = UINT_MAX };
	nlInitMessage(nl, RTM_GETLINK, NLM_F_ACK | NLM_F_ROOT);
	nlBufferAppend(nl, &ifi, sizeof(ifi));
	return nlSendMessage(nl, true, &netSendLinkCallback, &enumCtx);
}

typedef struct {
	bool ifi;
	int findIndex;
	union {
		struct ifinfomsg ifi;
		struct ifaddrmsg ifa;
	} msg;
	void* rtAttrs;
	size_t rtAttrSize;
} netAttrBuffer;

static int netParseLinkInfo(const nlContext* ctx, const void* data, uint32_t len, uint16_t type, uint16_t flags, void* arg) {
	netAttrBuffer* attrs = arg;
	size_t headerSize;
	int thisIndex;
	if (attrs->ifi) {
		headerSize = sizeof(struct ifinfomsg);
		thisIndex = ((const struct ifinfomsg*)data)->ifi_index;
	} else {
		headerSize = sizeof(struct ifaddrmsg);
		thisIndex = (int)((const struct ifaddrmsg*)data)->ifa_index;
	}

	// rtNetlink will return multiple results. We need to filter in userspace.
	if (thisIndex != attrs->findIndex) return 0;

	memcpy(&attrs->msg, data, headerSize);

	headerSize = NLMSG_ALIGN(headerSize);

	size_t rtAttrCap = 0;
	if (attrs->rtAttrs != NULL) flexBufferFree(&attrs->rtAttrs, &attrs->rtAttrSize, &rtAttrCap);
	flexBufferInit(&attrs->rtAttrs, &attrs->rtAttrSize, &rtAttrCap);

	for (const struct rtattr* rta = (const struct rtattr*)((const char*)data + headerSize); RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
		bool keepAttr = true;
		if (attrs->ifi) {
			switch (rta->rta_type) {
			case IFLA_ADDRESS: break;
			case IFLA_BROADCAST: break;
			default:
				keepAttr = false;
			}
		} else {
			switch (rta->rta_type) {
			case IFA_ADDRESS: break;
			case IFA_LOCAL: break;
			case IFA_BROADCAST: break;
			case IFA_ANYCAST: break;
			case IFA_CACHEINFO: break;
			default:
				keepAttr = false;
			}
		}
		if (keepAttr) {
			size_t attrLen = RTA_SPACE(RTA_PAYLOAD(rta));
			flexBufferGrow(&attrs->rtAttrs, attrs->rtAttrSize, &rtAttrCap, attrLen, 1);
			flexBufferAppend(attrs->rtAttrs, &attrs->rtAttrSize, rta, attrLen, 1);
		}
	}
	return 0;
}

int netMoveInterface(netContext* srcCtx, const char* intfName, int devIdx, netContext* dstCtx) {
	lprintf(LogDebug, "Moving interface %p:%d to context %p\n", srcCtx, devIdx, dstCtx);

	// If the interface is simply moved using RTM_NEWLINK, which is what
	// iproute2 does with "ip link set <dev> netns <ns>", then all of the
	// attached configuration is dropped. We want to preserve this information.
	// To do so, we first perform requests for the configuration data and store
	// all attached rtattrs. We then replay these rtattrs after the RTM_NEWLINK
	// call.

	nlContext* srcNl = &srcCtx->nl;
	nlContext* dstNl = &dstCtx->nl;
	int err;

	netAttrBuffer linkAttrs = { .ifi = true, .findIndex = devIdx, .rtAttrs = NULL, .rtAttrSize = 0 };
	netAttrBuffer addrAttrs = { .ifi = false, .findIndex = devIdx, .rtAttrs = NULL, .rtAttrSize = 0 };

	struct ifinfomsg ifi = { .ifi_family = AF_UNSPEC, .ifi_type = 0, .ifi_flags = 0, .ifi_change = UINT_MAX };
	struct ifaddrmsg ifa = { .ifa_family = AF_INET, .ifa_prefixlen = 0, .ifa_flags = 0, .ifa_scope = 0 };

	// Get link attributes
	ifi.ifi_index = devIdx;
	nlInitMessage(srcNl, RTM_GETLINK, NLM_F_ACK | NLM_F_ROOT);
	nlBufferAppend(srcNl, &ifi, sizeof(ifi));
	err = nlSendMessage(srcNl, true, &netParseLinkInfo, &linkAttrs);
	if (err != 0) goto cleanup;

	// Get link addresses
	ifa.ifa_index = (unsigned int)devIdx;
	nlInitMessage(srcNl, RTM_GETADDR, NLM_F_ACK | NLM_F_ROOT);
	nlBufferAppend(srcNl, &ifa, sizeof(ifa));
	err = nlSendMessage(srcNl, true, &netParseLinkInfo, &addrAttrs);
	if (err != 0) goto cleanup;

	// Move link to new namespace
	nlInitMessage(srcNl, RTM_NEWLINK, NLM_F_ACK);
	nlBufferAppend(srcNl, &linkAttrs.msg.ifi, sizeof(linkAttrs.msg.ifi));
	nlPushAttr(srcNl, IFLA_NET_NS_FD);
		nlBufferAppend(srcNl, &dstCtx->fd, sizeof(dstCtx->fd));
	nlPopAttr(srcNl);
	err = nlSendMessage(srcNl, true, NULL, NULL);
	if (err != 0) goto cleanup;

	// Get index in new namespace
	int newIdx = netGetInterfaceIndex(dstCtx, intfName, &err);
	if (newIdx == -1) goto cleanup;
	linkAttrs.msg.ifi.ifi_index = newIdx;
	addrAttrs.msg.ifa.ifa_index = (__u32)newIdx;

	// TODO Switching namespaces should not be necessary, but is. This is a bug
	// somewhere (either in our code, or the kernel).
	err = netSwitchNamespace(dstCtx);
	if (err != 0) goto cleanup;

	// Set old link attributes
	if (linkAttrs.rtAttrSize > 0) {
		nlInitMessage(dstNl, RTM_NEWLINK, NLM_F_ACK);
		nlBufferAppend(dstNl, &linkAttrs.msg.ifi, sizeof(linkAttrs.msg.ifi));
		nlBufferAppend(dstNl, linkAttrs.rtAttrs, linkAttrs.rtAttrSize);
		err = nlSendMessage(dstNl, true, NULL, NULL);
		if (err != 0) goto cleanup;
	}

	// Set old link addresses
	if (addrAttrs.rtAttrSize > 0) {
		nlInitMessage(dstNl, RTM_NEWADDR, NLM_F_ACK | NLM_F_CREATE | NLM_F_REPLACE);
		nlBufferAppend(dstNl, &addrAttrs.msg.ifa, sizeof(addrAttrs.msg.ifa));
		nlBufferAppend(dstNl, addrAttrs.rtAttrs, addrAttrs.rtAttrSize);
		err = nlSendMessage(dstNl, true, NULL, NULL);
		if (err != 0) goto cleanup;
	}

cleanup:
	if (linkAttrs.rtAttrs != NULL) free(linkAttrs.rtAttrs);
	if (addrAttrs.rtAttrs != NULL) free(addrAttrs.rtAttrs);
	return err;
}

int netCreateVethPair(const char* name1, const char* name2, netContext* ctx1, netContext* ctx2, const macAddr* addr1, const macAddr* addr2, bool sync) {
	if (PASSES_LOG_THRESHOLD(LogDebug)) {
		lprintHead(LogDebug);
		lprintDirectf(LogDebug, "Creating virtual ethernet pair (%p:'%s', %p:'%s')", ctx1, name1, ctx2, name2);
		char mac[MAC_ADDR_BUFLEN];
		if (addr1 != NULL) {
			macAddrToString(addr1, mac);
			lprintDirectf(LogDebug, ", mac1=%s", mac);
		}
		if (addr2 != NULL) {
			macAddrToString(addr2, mac);
			lprintDirectf(LogDebug, ", mac2=%s", mac);
		}
		lprintDirectf(LogDebug, "\n");
		lprintDirectFinish(LogDebug);
	}

	// The netlink socket used for this message does not matter, since we
	// explicitly provide namespace file descriptors as part of the request.
	nlContext* nl = &ctx1->nl;

	nlInitMessage(nl, RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL | (sync ? NLM_F_ACK : 0));

	struct ifinfomsg ifi = { .ifi_family = AF_UNSPEC, .ifi_type = 0, .ifi_index = 0, .ifi_flags = 0, .ifi_change = UINT_MAX };
	nlBufferAppend(nl, &ifi, sizeof(ifi));

	nlPushAttr(nl, IFLA_IFNAME);
		nlBufferAppend(nl, name1, strlen(name1)+1);
	nlPopAttr(nl);

	nlPushAttr(nl, IFLA_NET_NS_FD);
		nlBufferAppend(nl, &ctx1->fd, sizeof(ctx1->fd));
	nlPopAttr(nl);

	if (addr1 != NULL) {
		nlPushAttr(nl, IFLA_ADDRESS);
			nlBufferAppend(nl, addr1->octets, MAC_ADDR_BYTES);
		nlPopAttr(nl);
	}

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
				if (addr2 != NULL) {
					nlPushAttr(nl, IFLA_ADDRESS);
						nlBufferAppend(nl, addr2->octets, MAC_ADDR_BYTES);
					nlPopAttr(nl);
				}
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

static int sendIoCtl(netContext* ctx, const char* name, unsigned long command, void* req) {
	errno = 0;
	int res = ioctl(ctx->ioctlFd, command, req);
	if (res == -1) {
		lprintf(LogError, "Error for ioctl command %d on interface %p:'%s': %s\n", command, ctx, name, strerror(errno));
		return errno;
	}
	return 0;
}

static int sendIoCtlIfReq(netContext* ctx, const char* name, unsigned long command, void* data, struct ifreq* ifr) {
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
	int res = sendIoCtlIfReq(ctx, name, SIOCGIFINDEX, NULL, &ifr);
	if (res != 0) {
		*err = res;
		return -1;
	}

	lprintf(LogDebug, "Interface %p:'%s' has index %d\n", ctx, name, ifr.ifr_ifindex);
	return ifr.ifr_ifindex;
}

int netAddInterfaceAddrIPv4(netContext* ctx, int devIdx, ip4Addr addr, uint8_t subnetBits, ip4Addr broadcastAddr, ip4Addr anycastAddr, bool sync) {
	if (PASSES_LOG_THRESHOLD(LogDebug)) {
		char ip[IP4_ADDR_BUFLEN];
		char broadcastIp[IP4_ADDR_BUFLEN];
		char anycastIp[IP4_ADDR_BUFLEN];
		ip4AddrToString(addr, ip);
		ip4AddrToString(broadcastAddr, broadcastIp);
		ip4AddrToString(anycastAddr, anycastIp);
		lprintf(LogDebug, "Adding address to %p:%d: %s/%u, broadcast %s, anycast %s\n", ctx, devIdx, ip, subnetBits, broadcastIp, anycastIp);
	}

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
	int res = sendIoCtlIfReq(ctx, name, SIOCGIFFLAGS, NULL, &ifr);
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

	int res = sendIoCtlIfReq(ctx, name, SIOCETHTOOL, &ev, NULL);
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
		lprintDirectFinish(LogDebug);
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

int netAddStaticArp(netContext* ctx, const char* intfName, ip4Addr ip, const macAddr* mac) {
	struct arpreq arpr;

	struct sockaddr_in* pa = (struct sockaddr_in*)&arpr.arp_pa;
	pa->sin_family = AF_INET;
	pa->sin_port = 0;
	pa->sin_addr.s_addr = ip;

	arpr.arp_ha.sa_family = ARPHRD_ETHER;
	memcpy(arpr.arp_ha.sa_data, mac->octets, MAC_ADDR_BYTES);

	arpr.arp_flags = ATF_COM | ATF_PERM;

	strncpy(arpr.arp_dev, intfName, INTERFACE_BUF_LEN);

	if (PASSES_LOG_THRESHOLD(LogDebug)) {
		char ipStr[IP4_ADDR_BUFLEN];
		char macStr[MAC_ADDR_BUFLEN];
		ip4AddrToString(ip, ipStr);
		macAddrToString(mac, macStr);
		lprintf(LogDebug, "Adding static ARP entry for interface %p:'%s': %s => %s\n", ctx, intfName, ipStr, macStr);
	}

	return sendIoCtl(ctx, intfName, SIOCSARP, &arpr);
}

int netGetRemoteMacAddr(netContext* ctx, const char* intfName, ip4Addr ip, macAddr* result) {
	struct arpreq arpr = { .arp_flags = 0 };

	struct sockaddr_in* pa = (struct sockaddr_in*)&arpr.arp_pa;
	pa->sin_family = AF_INET;
	pa->sin_port = 0;
	pa->sin_addr.s_addr = ip;

	arpr.arp_ha.sa_family = AF_UNSPEC;

	strncpy(arpr.arp_dev, intfName, IFNAMSIZ);
	arpr.arp_dev[IFNAMSIZ-1] = '\0';

	int res = sendIoCtl(ctx, intfName, SIOCGARP, &arpr);
	if (res == ENODEV || res == ENXIO) return EAGAIN;
	if (res != 0) return res;

	if (arpr.arp_ha.sa_family != ARPHRD_ETHER) {
		lprintf(LogError, "ARP table entry had unexpected family %u\n", arpr.arp_ha.sa_family);
		return EAFNOSUPPORT;
	}
	memcpy(result->octets, arpr.arp_ha.sa_data, MAC_ADDR_BYTES);

	// Incomplete entries are represented as completely empty addresses
	for (size_t i = 0; i < MAC_ADDR_BYTES; ++i) {
		if (result->octets[i] != 0) return 0;
	}
	return EAGAIN;
}

int netGetLocalMacAddr(netContext* ctx, const char* name, macAddr* result) {
	struct ifreq ifr;
	initIfReq(&ifr);
	int res = sendIoCtlIfReq(ctx, name, SIOCGIFHWADDR, NULL, &ifr);
	if (res != 0) return res;

	if (ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER) {
		lprintf(LogError, "Hardware address for interface %p:'%s' has an unsupported family %u\n", ifr.ifr_hwaddr.sa_family);
		return EAFNOSUPPORT;
	}
	memcpy(result->octets, ifr.ifr_hwaddr.sa_data, MAC_ADDR_BYTES);

	if (PASSES_LOG_THRESHOLD(LogDebug)) {
		char macStr[MAC_ADDR_BUFLEN];
		macAddrToString(result, macStr);
		lprintf(LogDebug, "Interface %p:'%s' has MAC address %s\n", ctx, name, macStr);
	}
	return 0;
}

static int readSysctlFmt(const char* filePath, const char* fmt, void* value) {
	errno = 0;
	FILE* f = fopen(filePath, "r");
	if (f == NULL) return errno;

	int res = fscanf(f, fmt, value);
	fclose(f);
	return (res == 1) ? 0 : 1;
}

static int writeSysctlFmt(const char* filePath, const char* fmt, ...) {
	errno = 0;
	FILE* f = fopen(filePath, "w");
	if (f == NULL) return errno;

	va_list args;
	va_start(args, fmt);
	int res = vfprintf(f, fmt, args);
	va_end(args);

	fclose(f);
	return (res < 0) ? 1 : 0;
}

int netSetForwarding(bool enabled) {
	lprintf(LogDebug, "Turning %s IP forwarding (routing) for the active namespace\n", enabled ? "on" : "off");
	return writeSysctlFmt(SYSCTL_FORWARDING, enabled ? "1" : "0");
}

int netSetMartians(bool allow) {
	lprintf(LogDebug, "%s Martian packets in the active namespace\n", allow ? "Allowing" : "Disallowing");

	// We need to write the setting to both the "default" and "all" values
	// because the kernel uses the maximum of the values as the effective
	// setting. A consequence of this is that any interfaces that were created
	// with a higher setting will be unaffected by this call.
	const char* setting = allow ? "0" : "1";
	int err = writeSysctlFmt(SYSCTL_MARTIANS, setting);
	if (err != 0) return err;
	return writeSysctlFmt(SYSCTL_MARTIANS_DEFAULT, setting);
}

int netSetIPv6(bool enabled) {
	lprintf(LogDebug, "Turning %s IPv6 support in the active namespace\n", enabled ? "on" : "off");
	return writeSysctlFmt(SYSCTL_DISABLE_IPV6, enabled ? "0" : "1");
}

int netGetArpTableSize(int* thresh1, int* thresh2, int* thresh3) {
	int err;

	err = readSysctlFmt(SYSCTL_ARP_GC_PREFIX "1", "%d", thresh1);
	if (err != 0) return err;
	err = readSysctlFmt(SYSCTL_ARP_GC_PREFIX "2", "%d", thresh2);
	if (err != 0) return err;
	err = readSysctlFmt(SYSCTL_ARP_GC_PREFIX "3", "%d", thresh3);
	if (err != 0) return err;

	return 0;
}

int netSetArpTableSize(int thresh1, int thresh2, int thresh3) {
	int err;

	err = writeSysctlFmt(SYSCTL_ARP_GC_PREFIX "1", "%d", thresh1);
	if (err != 0) return err;
	err = writeSysctlFmt(SYSCTL_ARP_GC_PREFIX "2", "%d", thresh2);
	if (err != 0) return err;
	err = writeSysctlFmt(SYSCTL_ARP_GC_PREFIX "3", "%d", thresh3);
	if (err != 0) return err;

	return 0;
}

static bool getTableId(RoutingTable table, uint8_t* tableId) {
	switch (table) {
	case TableMain:
		*tableId = RT_TABLE_MAIN;
		break;
	case TableLocal:
		*tableId = RT_TABLE_LOCAL;
		break;
	default:
		lprintf(LogError, "Unknown routing table constant %d\n", table);
		return false;
	}
	return true;
}

static bool getScopeId(RoutingScope scope, unsigned char* scopeId) {
	switch (scope) {
	case ScopeLink:
		*scopeId = RT_SCOPE_LINK;
		break;
	case ScopeGlobal:
		*scopeId = RT_SCOPE_UNIVERSE;
		break;
	default:
		lprintf(LogError, "Unknown routing scope %d\n", scope);
		return false;
	}
	return true;
}

static bool initRtMsg(struct rtmsg* rtm, unsigned char prefixLen, unsigned char table, RoutingScope scope) {
	rtm->rtm_family = AF_INET;
	rtm->rtm_dst_len = prefixLen;
	rtm->rtm_src_len = 0;
	rtm->rtm_tos = 0;

	rtm->rtm_table = table;
	rtm->rtm_protocol = RTPROT_STATIC;
	rtm->rtm_type = RTN_UNICAST;

	rtm->rtm_flags = 0;

	return getScopeId(scope, &rtm->rtm_scope);
}

int netAddRoute(netContext* ctx, RoutingTable table, RoutingScope scope, ip4Addr dstAddr, uint8_t subnetBits, ip4Addr gatewayAddr, int dstDevIdx, bool sync) {
	uint8_t tableId;
	if (!getTableId(table, &tableId)) return 1;
	return netAddRouteToTable(ctx, tableId, scope, dstAddr, subnetBits, gatewayAddr, dstDevIdx, sync);
}

int netAddRouteToTable(netContext* ctx, uint8_t table, RoutingScope scope, ip4Addr dstAddr, uint8_t subnetBits, ip4Addr gatewayAddr, int dstDevIdx, bool sync) {
	if (PASSES_LOG_THRESHOLD(LogDebug)) {
		char dstIp[IP4_ADDR_BUFLEN];
		char gatewayIp[IP4_ADDR_BUFLEN];
		ip4AddrToString(dstAddr, dstIp);
		ip4AddrToString(gatewayAddr, gatewayIp);
		lprintf(LogDebug, "Adding route for namespace %p table %u: %s/%u => interface %d via %sgateway %s\n", ctx, table, dstIp, subnetBits, dstDevIdx, gatewayAddr == 0 ? "(disabled) " : "", gatewayIp);
	}

	struct rtmsg rtm;
	if (!initRtMsg(&rtm, subnetBits, table, scope)) return 1;

	nlContext* nl = &ctx->nl;
	nlInitMessage(nl, RTM_NEWROUTE, NLM_F_CREATE | NLM_F_EXCL | (sync ? NLM_F_ACK : 0));

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

int netAddRule(netContext* ctx, const ip4Subnet* subnet, const char* inputIntf, RoutingTable table, uint32_t priority, bool sync) {
	uint8_t tableId;
	if (!getTableId(table, &tableId)) return 1;
	return netAddRuleForTable(ctx, subnet, inputIntf, tableId, priority, sync);
}

int netAddRuleForTable(netContext* ctx, const ip4Subnet* subnet, const char* inputIntf, uint8_t table, uint32_t priority, bool sync) {
	if (PASSES_LOG_THRESHOLD(LogDebug)) {
		char subnetStr[IP4_CIDR_BUFLEN];
		ip4SubnetToString(subnet, subnetStr);
		lprintf(LogDebug, "Adding policy routing rule for %p: %s from '%s' => table %u, priority %u\n", ctx, subnetStr, (inputIntf == NULL ? "(any)" : inputIntf), table, priority);
	}

	struct rtmsg rtm;
	if (!initRtMsg(&rtm, subnet->prefixLen, table, ScopeGlobal)) return 1;

	nlContext* nl = &ctx->nl;
	nlInitMessage(nl, RTM_NEWRULE, NLM_F_CREATE | NLM_F_EXCL | (sync ? NLM_F_ACK : 0));

	nlBufferAppend(nl, &rtm, sizeof(rtm));

	nlPushAttr(nl, FRA_DST);
		nlBufferAppend(nl, &subnet->addr, sizeof(subnet->addr));
	nlPopAttr(nl);

	if (inputIntf != NULL) {
		nlPushAttr(nl, FRA_IIFNAME);
			nlBufferAppend(nl, inputIntf, strlen(inputIntf)+1);
		nlPopAttr(nl);
	}

	nlPushAttr(nl, FRA_PRIORITY);
		nlBufferAppend(nl, &priority, sizeof(priority));
	nlPopAttr(nl);

	return nlSendMessage(nl, sync, NULL, NULL);
}
