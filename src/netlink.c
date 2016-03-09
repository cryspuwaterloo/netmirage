#include "netlink.h"

#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>

#include <unistd.h>
#include <asm/types.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include "log.h"

#define MAX_ATTR_NEST 10

struct nlContext_s {
	int sock;
	uint32_t nextSeq;

	struct sockaddr_nl localAddr;

	struct msghdr msg;
	struct iovec iov;

	// Raw buffer used to hold the message being constructed
	union {
		void* msgBuffer;
		struct nlmsghdr* nlmsg;
	};
	size_t msgBufferCap;
	size_t msgBufferLen;

	size_t attrNestPos[MAX_ATTR_NEST];
	size_t attrDepth;
};

nlContext* nlNewContext(int* err) {
	lprintln(LogDebug, "Opening rtnetlink socket");
	nlContext* ctx = malloc(sizeof(nlContext));

	errno = 0;
	ctx->sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (ctx->sock == -1) {
		lprintf(LogError, "Failed to open netlink socket: %s\n", strerror(errno));
		goto abort;
	}

	// We allow the kernel to assign us an address in order to support multiple threads
	ctx->localAddr.nl_family = AF_NETLINK;
	ctx->localAddr.nl_groups = 0;
	ctx->localAddr.nl_pad = 0;
	ctx->localAddr.nl_pid = 0;
	errno = 0;
	if (bind(ctx->sock, (struct sockaddr*)&ctx->localAddr, sizeof(ctx->localAddr)) != 0) {
		lprintf(LogError, "Failed to bind netlink socket: %s\n", strerror(errno));
		goto abort;
	}
	socklen_t myAddrLen = sizeof(ctx->localAddr);
	if (getsockname(ctx->sock, (struct sockaddr*)&ctx->localAddr, &myAddrLen)) {
		lprintf(LogError, "Failed to retrieve kernel-assigned netlink address: %s\n", strerror(errno));
		goto abort;
	}

	ctx->nextSeq = 0;
	ctx->msgBuffer = NULL;
	ctx->msgBufferCap = 0;
	ctx->msgBufferLen = 0;

	return ctx;
abort:
	if (err) *err = errno;
	if (ctx->sock != -1) close(ctx->sock);
	free(ctx);
	return NULL;
}

int nlFreeContext(nlContext* ctx) {
	lprintln(LogDebug, "Closing rtnetlink socket");
	if (ctx->msgBuffer) free(ctx->msgBuffer);
	errno = 0;
	int res = close(ctx->sock);
	free(ctx);
	if (res == -1) {
		lprintf(LogError, "Failed to close netlink socket: %s\n", strerror(errno));
		return errno;
	}
	return 0;
}

static struct sockaddr_nl kernelAddr = { AF_NETLINK, 0, 0, 0 };

static void nlReserveSpace(nlContext* ctx, size_t amount) {
	size_t capacity = ctx->msgBufferLen + amount;
	if (ctx->msgBufferCap < capacity) {
		ctx->msgBufferCap = capacity * 2;
		ctx->msgBuffer = realloc(ctx->msgBuffer, ctx->msgBufferCap);
	}
}

static void nlCommitSpace(nlContext* ctx, size_t amount) {
	ctx->msgBufferLen += amount;
}

static void* nlBufferTail(nlContext* ctx) {
	return (char*)ctx->msgBuffer + ctx->msgBufferLen;
}

void nlInitMessage(nlContext* ctx, uint16_t msgType, uint16_t msgFlags) {
	ctx->msgBufferLen = 0;
	nlReserveSpace(ctx, NLMSG_SPACE(0));

	ctx->attrDepth = 0;

	ctx->msg.msg_name = &kernelAddr;
	ctx->msg.msg_namelen = sizeof(kernelAddr);
	ctx->msg.msg_control = NULL;
	ctx->msg.msg_controllen = 0;
	ctx->msg.msg_flags = 0;

	// nlmsg length and other lengths are set just before sending, since they
	// are unknown at this point

	ctx->nlmsg->nlmsg_type = msgType;
	ctx->nlmsg->nlmsg_flags = NLM_F_REQUEST | msgFlags;
	ctx->nlmsg->nlmsg_seq = ctx->nextSeq++;
	ctx->nlmsg->nlmsg_pid = ctx->localAddr.nl_pid;

	// We don't link iov to nlmsg yet because the buffer may be reallocated
	ctx->msg.msg_iov = &ctx->iov;
	ctx->msg.msg_iovlen = 1;

	nlCommitSpace(ctx, (char*)NLMSG_DATA(ctx->nlmsg) - (char*)ctx->nlmsg);
}

void nlBufferAppend(nlContext* ctx, const void* buffer, size_t len) {
	nlReserveSpace(ctx, len);
	memcpy(nlBufferTail(ctx), buffer, len);
	nlCommitSpace(ctx, len);
}

int nlPushAttr(nlContext* ctx, unsigned short type) {
	if (ctx->attrDepth >= MAX_ATTR_NEST) {
		lprintln(LogError, "BUG: rtnetattr exceeded allowed nesting depth!");
		return -1;
	}

	nlReserveSpace(ctx, RTA_SPACE(0));

	struct rtattr* attr = nlBufferTail(ctx);
	attr->rta_type = type;

	ctx->attrNestPos[ctx->attrDepth] = ctx->msgBufferLen;
	++ctx->attrDepth;

	nlCommitSpace(ctx, (char*)RTA_DATA(attr) - (char*)attr);

	return 0;
}

int nlPopAttr(nlContext* ctx) {
	if (ctx->attrDepth == 0) {
		lprintln(LogError, "BUG: rtnetattr was finished when the stack was empty!");
		return -1;
	}

	--ctx->attrDepth;
	struct rtattr* attr = (struct rtattr*)((char*)ctx->msgBuffer + ctx->attrNestPos[ctx->attrDepth]);

	size_t payloadLen = (char*)nlBufferTail(ctx) - (char*)RTA_DATA(attr);
	attr->rta_len = RTA_LENGTH(payloadLen);

	size_t paddingDeficit = RTA_SPACE(payloadLen) - ((char*)nlBufferTail(ctx) - (char*)attr);
	if (paddingDeficit > 0) {
		nlReserveSpace(ctx, paddingDeficit);
		nlCommitSpace(ctx, paddingDeficit);
	}
	return 0;
}

int nlSendMessage(nlContext* ctx) {
	if (ctx->attrDepth > 0) {
		lprintf(LogError, "BUG: Attempted to send netlink packet with an rtattr depth of %d!\n", ctx->attrDepth);
		return -1;
	}
	ctx->nlmsg->nlmsg_len = NLMSG_LENGTH((char*)nlBufferTail(ctx) - (char*)NLMSG_DATA(ctx->nlmsg));
	ctx->iov.iov_base = ctx->nlmsg;
	ctx->iov.iov_len = ctx->nlmsg->nlmsg_len;

	errno = 0;
	if (sendmsg(ctx->sock, &ctx->msg, 0) == -1) {
		lprintf(LogError, "sendmsg err: %s\n", strerror(errno));
	}

	return 0;
}
