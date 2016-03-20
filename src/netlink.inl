// This file exposes the internal netlink context structure. It is exposed so
// that it can be embedded for performance reasons.

// WARNING: Nothing except netlink.c should access the data members!

#include <stdint.h>

#include <unistd.h>
#include <asm/types.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#define MAX_ATTR_NEST 10

struct nlContext_s {
	int sock;
	uint32_t nextSeq;

	struct sockaddr_nl localAddr;

	struct msghdr msg;
	struct iovec iov;

	size_t attrNestPos[MAX_ATTR_NEST];
	size_t attrDepth;
};