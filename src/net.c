#define _GNU_SOURCE

#include "net.h"

#include <errno.h>
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
int openNetNamespace(const char* name, int* err, bool excl) {
	*err = setupNamespaceEnvironment();
	if (*err != 0) return -1;

	char netNsPath[PATH_MAX];
	*err = getNamespacePath(netNsPath, name);
	if (*err != 0) return -1;

	lprintf(LogDebug, "Opening network namespace file at '%s'\n", netNsPath);
	int nsFd;

	while (true) {
		if (!excl) {
			errno = 0;
			nsFd = open(netNsPath, O_RDONLY | O_CLOEXEC, 0);
			if (nsFd != -1) return nsFd;
		}

		nsFd = open(netNsPath, O_RDONLY | O_CLOEXEC | O_CREAT | (excl ? O_EXCL : 0), S_IRUSR | S_IRGRP | S_IROTH);
		if (nsFd == -1) {
			lprintf(LogError, "Failed to create network namespace file '%s': %s\n", netNsPath, strerror(errno));
			*err = errno;
			return -1;
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

	return nsFd;
abort:
	unlink(netNsPath);
	return errno;
}

int deleteNetNamespace(const char* name) {
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

int switchNetNamespace(int nsFd) {
	errno = 0;
	int res = setns(nsFd, CLONE_NEWNET);
	close(nsFd);
	if (res != 0) {
		lprintf(LogError, "Failed to set active network namespace: %s\n", strerror(errno));
		return errno;
	}
	return 0;
}

int switchNetNamespaceName(const char* name) {
	char netNsPath[PATH_MAX];
	if (name == NULL || name[0] == '\0') {
		lprintf(LogDebug, "Switching to default network namespace\n");
		strncpy(netNsPath, "/proc/1/ns/net", PATH_MAX-1);
	} else {
		int res = getNamespacePath(netNsPath, name);
		if (res != 0) return res;
		lprintf(LogDebug, "Switching to network namespace file at '%s'\n", netNsPath);
	}

	errno = 0;
	int nsFd = open(netNsPath, O_RDONLY | O_CLOEXEC);
	if (nsFd == -1) {
		lprintf(LogError, "Failed to open network namespace file '%s': %s\n", netNsPath, strerror(errno));
		return errno;
	}
	int res = switchNetNamespace(nsFd);
	close(nsFd);
	return res;
}

int createVethPair(nlContext* ctx, const char* name1, const char* name2, int ns1, int ns2, bool sync) {
	nlInitMessage(ctx, RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL | (sync ? NLM_F_ACK : 0));

	struct ifinfomsg ifi;
	ifi.ifi_family = AF_UNSPEC;
	ifi.ifi_type = 0;
	ifi.ifi_index = 0;
	ifi.ifi_flags = 0;
	ifi.ifi_change = ~0;

	nlBufferAppend(ctx, &ifi, sizeof(ifi));

	nlPushAttr(ctx, IFLA_IFNAME);
		nlBufferAppend(ctx, name1, strlen(name1)+1);
	nlPopAttr(ctx);

	nlPushAttr(ctx, IFLA_NET_NS_FD);
		nlBufferAppend(ctx, &ns1, sizeof(ns1));
	nlPopAttr(ctx);

	nlPushAttr(ctx, IFLA_LINKINFO);
		nlPushAttr(ctx, IFLA_INFO_KIND);
			nlBufferAppend(ctx, "veth", 4);
		nlPopAttr(ctx);
		nlPushAttr(ctx, IFLA_INFO_DATA);
			nlPushAttr(ctx, 1); // VETH_INFO_PEER
				nlBufferAppend(ctx, &ifi, sizeof(ifi));
				nlPushAttr(ctx, IFLA_IFNAME);
					nlBufferAppend(ctx, name2, strlen(name2)+1);
				nlPopAttr(ctx);
				nlPushAttr(ctx, IFLA_NET_NS_FD);
					nlBufferAppend(ctx, &ns2, sizeof(ns2));
				nlPopAttr(ctx);
			nlPopAttr(ctx);
		nlPopAttr(ctx);
	nlPopAttr(ctx);

	return nlSendMessage(ctx);
}
