#define _GNU_SOURCE

#include "net.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <fcntl.h>
#include <linux/limits.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"
#include "netlink.h"

// The implementations in this module are highly specific to Linux.
// The code has been written to be compatible with the named network namespace
// approach defined by the iproute2 tools. For more information, see:
//   http://www.linuxfoundation.org/collaborate/workgroups/networking/iproute2

struct netContext_s {
	int fd;
	nlContext* nl;
};

static const char* NetNsDir = "/var/run/netns";

static char namespacePrefix[PATH_MAX];

void setNamespacePrefix(const char* prefix) {
	strncpy(namespacePrefix, prefix, PATH_MAX-1);
	namespacePrefix[PATH_MAX-1] = '\0';
}

// Ensures that the network namespace folders exist and are mounted.
static int setupNamespaceEnvironment() {
	// We only bother initializing this once per execution. The lifetime of the
	// process is such that if a race condition occurs, it's better to quit with
	// an error rather than attempting to remount.
	static bool initialized = false;
	if (initialized) return 0;

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
			lprintf(LogDebug, "Created system network namespace directory\n");
			createdMount = true;
		} else if (!madeBind && errno == EINVAL) {
			errno = 0;
			lprintf(LogDebug, "Bind mounting system network namespace directory\n");
			mount(NetNsDir, NetNsDir, "none", MS_BIND, NULL);
			madeBind = true;
		}
		if (errno != 0) {
			lprintf(LogError, "Could not mount the system network namespace directory '%s': %s. Elevation may be required.\n", NetNsDir, strerror(errno));
			return errno;
		}
	}

	initialized = true;
	return 0;
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
netContext* netOpenNamespace(const char* name, int* err, bool excl) {
	int localErr;
	if (err == NULL) err = &localErr;

	*err = setupNamespaceEnvironment();
	if (*err != 0) return NULL;

	char netNsPath[PATH_MAX];
	*err = getNamespacePath(netNsPath, name);
	if (*err != 0) return NULL;

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
			*err = errno;
			return NULL;
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
		if (mount("/proc/self/ns/net", netNsPath, "none", MS_BIND, NULL) != 0) {
			lprintf(LogError, "Failed to bind new network namespace file '%s': %s\n", netNsPath, strerror(errno));
			goto abort;
		}

		// We need to open the file descriptor again. The one from before the
		// bind mount is not a valid namespace descriptor.
		excl = false;
	}

	nlContext* nl = nlNewContext(err);
	if (nl == NULL) {
		netDeleteNamespace(name);
		return NULL;
	}

	netContext* ctx = malloc(sizeof(netContext));
	ctx->fd = nsFd;
	ctx->nl = nl;
	lprintf(LogDebug, "Opened network namespace file at '%s' with context %p\n", netNsPath, ctx);
	return ctx;
abort:
	unlink(netNsPath);
	*err = errno;
	return NULL;
}

void netFreeContext(netContext* ctx) {
	close(ctx->fd);
	nlFreeContext(ctx->nl);
	free(ctx);
}

int netDeleteNamespace(const char* name) {
	char netNsPath[PATH_MAX];
	int res = getNamespacePath(netNsPath, name);
	if (res != 0) return res;

	lprintf(LogDebug, "Deleting network namespace file at '%s'\n", netNsPath);

	// Lazy unmount preserves the namespace if it is still being used by others
	errno = 0;
	if (umount2(netNsPath, MNT_DETACH) != 0) {
		lprintf(LogError, "Failed to unmount network namespace file '%s': %s. Elevation may be required.\n", netNsPath, strerror(errno));
		return errno;
	}
	errno = 0;
	if (unlink(netNsPath) != 0) {
		lprintf(LogError, "Failed to delete network namespace file '%s': %s\n", netNsPath, strerror(errno));
		return errno;
	}

	return 0;
}

int netSwitchContext(netContext* ctx) {
	errno = 0;
	int nsFd;
	if (ctx != NULL) {
		lprintf(LogDebug, "Switching to network namespace context %p\n", ctx);
		nsFd = ctx->fd;
	} else {
		lprintf(LogDebug, "Switching to default network namespace\n");
		nsFd = open("/proc/1/ns/net", O_RDONLY | O_CLOEXEC);
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
	nlContext* nl = ctx1->nl;

	nlInitMessage(nl, RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL | (sync ? NLM_F_ACK : 0));

	struct ifinfomsg ifi = { .ifi_family = AF_UNSPEC, .ifi_change = ~0, .ifi_type = 0, .ifi_index = 0, .ifi_flags = 0 };
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

typedef struct {
	const char* name;
	int index;
} getInterfaceState;

static int recordInterfaceIndex(const nlContext* ctx, const void* data, uint32_t len, uint16_t type, uint16_t flags, void* arg) {
	getInterfaceState* state = arg;

	const struct ifinfomsg* ifi = data;
	if (type != RTM_NEWLINK) {
		lprintf(LogError, "Unknown interface info response type %d in index response\n", ifi->ifi_type);
		return -1;
	}

	int64_t remLen = len - sizeof(*ifi);
	if (remLen < 0) {
		lprintf(LogError, "Interface info response was truncated (%u bytes)\n", len);
		return -1;
	}

	for (const struct rtattr* rta = (const struct rtattr*)((char*)data + NLMSG_LENGTH(sizeof(*ifi)) - NLMSG_LENGTH(0)); RTA_OK(rta, remLen); rta = RTA_NEXT(rta, remLen)) {
		if (rta->rta_type == IFLA_IFNAME) {
			size_t nameLen = strlen(state->name)+1;
			if (nameLen > rta->rta_len || strncmp(state->name, RTA_DATA(rta), nameLen) != 0) {
				lprintf(LogError, "Interface info response had the wrong identifier ('%s', expected '%s')\n", RTA_DATA(rta), state->name);
				return -1;
			}
			state->index = ifi->ifi_index;
			return 0;
		}
	}
	lprintln(LogError, "Interface info response did not include an interface name.");
	return -1;
}

int netGetInterfaceIndex(netContext* ctx, const char* name, int* err) {
	nlContext* nl = ctx->nl;
	nlInitMessage(nl, RTM_GETLINK, 0);

	struct ifinfomsg ifi = { .ifi_family = AF_UNSPEC, .ifi_change = ~0, .ifi_type = 0, .ifi_index = 0, .ifi_flags = 0 };
	nlBufferAppend(nl, &ifi, sizeof(ifi));

	nlPushAttr(nl, IFLA_IFNAME);
		nlBufferAppend(nl, name, strlen(name)+1);
	nlPopAttr(nl);

	getInterfaceState state = {name, 0};
	int res = nlSendMessage(nl, true, &recordInterfaceIndex, &state);
	if (res != 0) {
		lprintf(LogError, "Error while retrieving interface identifier for '%s': code %d\n", name, res);
		if (err != NULL) *err = res;
		return -1;
	}

	lprintf(LogDebug, "Interface %p:'%s' has index %d\n", ctx, name, state.index);
	return state.index;
}

int netAddInterfaceAddrIPv4(netContext* ctx, int devIdx, uint32_t addr, uint8_t subnetBits, uint32_t broadcastAddr, uint32_t anycastAddr, bool sync) {
	if (subnetBits > 32) subnetBits = 32;

	nlContext* nl = ctx->nl;
	nlInitMessage(nl, RTM_NEWADDR, NLM_F_CREATE | NLM_F_REPLACE | (sync ? NLM_F_ACK : 0));

	struct ifaddrmsg ifa = { .ifa_family = AF_INET, .ifa_prefixlen = subnetBits, .ifa_index = devIdx, .ifa_flags = 0, .ifa_scope = 0 };
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

	return nlSendMessage(nl, true, NULL, NULL);
}

int netDelInterfaceAddrIPv4(netContext* ctx, int devIdx, bool sync) {
	nlContext* nl = ctx->nl;
	nlInitMessage(nl, RTM_DELADDR, sync ? NLM_F_ACK : 0);

	struct ifaddrmsg ifa = {.ifa_family = AF_INET, .ifa_index = devIdx, .ifa_prefixlen = 0, .ifa_flags = 0, .ifa_scope = 0 };
	nlBufferAppend(nl, &ifa, sizeof(ifa));

	return nlSendMessage(nl, true, NULL, NULL);
}
