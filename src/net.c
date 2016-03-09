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
// The code has been written to be compatible with the named network namespace approach
// defined by the iproute2 tools. For more information, see:
//   http://www.linuxfoundation.org/collaborate/workgroups/networking/iproute2

static const char* NetNsDir = "/var/run/netns";

static char namespacePrefix[PATH_MAX];
static char currentNsPath[PATH_MAX];

void setNamespacePrefix(const char* prefix) {
	strncpy(namespacePrefix, prefix, PATH_MAX-1);
	namespacePrefix[PATH_MAX-1] = '\0';
}

// Ensures that the network namespace folders exist and are mounted.
static int setupNamespaceEnvironment() {
	// We only bother initializing this once per execution. The lifetime of the sneac
	// process is such that if a race condition occurs, it's better to quit with an error
	// rather than attempting to remount.
	static bool initialized = false;
	if (initialized) return 0;

	errno = 0;
	if (mkdir(NetNsDir, 0755) != 0 && errno != EEXIST) {
		lprintf(LogError, "Could not create the system network namespace directory '%s': %s. Elevation may be required.\n", NetNsDir, strerror(errno));
		return errno;
	}

	// Mount the namespace directory in the same way as iproute2. This mounting scheme
	// allows namespaces to be freed sooner in some cases.
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

static void setCurrentNamespacePath(const char* path) {
	strncpy(currentNsPath, path, PATH_MAX-1);
	currentNsPath[PATH_MAX-1] = '\0';
}

// This implementation is meant to be compatible with the "ip netns add" command.
int createNetNamespace(const char* name) {
	int res = setupNamespaceEnvironment();
	if (res != 0) return res;

	char netNsPath[PATH_MAX];
	res = getNamespacePath(netNsPath, name);
	if (res != 0) return res;

	lprintf(LogDebug, "Creating network namespace file at '%s'\n", netNsPath);
	errno = 0;
	int nsFile = open(netNsPath, O_RDONLY | O_CREAT, 0);
	if (nsFile == -1) {
		if (errno == EEXIST) return 0;
		lprintf(LogError, "Failed to create network namespace file '%s': %s\n", netNsPath, strerror(errno));
		return errno;
	}
	close(nsFile);

	// Create a new network namespace (any forked processes will still use the old one).
	// This command implicitly switches us to the new namespace.
	errno = 0;
	if (unshare(CLONE_NEWNET) != 0) {
		lprintf(LogError, "Failed to instantiate a new network namespace: %s\n", strerror(errno));
	} else {
		// Bind mount the new namespace. This prevents it from closing until it is
		// explicitly unmounted; there is no need to keep a dedicated process bound to it.
		errno = 0;
		if (mount("/proc/self/ns/net", netNsPath, "none", MS_BIND, NULL) != 0) {
			lprintf(LogError, "Failed to bind new network namespace file '%s': %s\n", netNsPath, strerror(errno));
		}
	}

	if (errno != 0) {
		unlink(netNsPath);
		return errno;
	}

	setCurrentNamespacePath(netNsPath);
	return 0;
}

int deleteNetNamespace(const char* name) {
	char netNsPath[PATH_MAX];
	int res = getNamespacePath(netNsPath, name);
	if (res != 0) return res;

	lprintf(LogDebug, "Deleting network namespace file at '%s'\n", netNsPath);

	// If we are deleting our current namespace, switch to the default first
	if (strncmp(currentNsPath, netNsPath, PATH_MAX) == 0) {
		res = switchNetNamespace(NULL);
		if (res != 0) return res;
	}

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

int switchNetNamespace(const char* name) {
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
	int nsFile = open(netNsPath, O_RDONLY | O_CLOEXEC);
	if (nsFile == -1) {
		lprintf(LogError, "Failed to open network namespace file '%s': %s\n", netNsPath, strerror(errno));
		return errno;
	}
	errno = 0;
	int res = setns(nsFile, CLONE_NEWNET);
	close(nsFile);
	if (res != 0) {
		lprintf(LogError, "Failed to set active network namespace: %s\n", strerror(errno));
		return errno;
	}

	setCurrentNamespacePath(netNsPath);
	return 0;
}

// TODO
int createVethPair(nlContext* ctx, const char* name1, const char* name2) {
	nlInitMessage(ctx, RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK);

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

	setupNamespaceEnvironment();
	createNetNamespace("red");
	switchNetNamespace(NULL);

	int blueFd = open("/var/run/netns/sneac-blue", O_RDONLY | O_CLOEXEC);
	int redFd = open("/var/run/netns/sneac-red", O_RDONLY | O_CLOEXEC);

	nlPushAttr(ctx, IFLA_NET_NS_FD);
		nlBufferAppend(ctx, &blueFd, sizeof(blueFd));
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
					nlBufferAppend(ctx, &redFd, sizeof(redFd));
				nlPopAttr(ctx);
			nlPopAttr(ctx);
		nlPopAttr(ctx);
	nlPopAttr(ctx);

	nlSendMessage(ctx);

	close(blueFd);
	close(redFd);

	return 0;
}
