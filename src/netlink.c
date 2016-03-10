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

	// Raw buffer used to hold the message being constructed or received
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

	// We let the kernel assign us an address to support multiple threads
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

static void nlResetSpace(nlContext* ctx, size_t initialCapacity) {
	ctx->msgBufferLen = 0;
	nlReserveSpace(ctx, initialCapacity);
}

static void* nlBufferTail(nlContext* ctx) {
	return (char*)ctx->msgBuffer + ctx->msgBufferLen;
}

void nlInitMessage(nlContext* ctx, uint16_t msgType, uint16_t msgFlags) {
	nlResetSpace(ctx, NLMSG_SPACE(0));

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

int nlSendMessage(nlContext* ctx, bool waitResponse, nlResponseHandler handler, void* arg) {
	if (ctx->attrDepth > 0) {
		lprintf(LogError, "BUG: Attempted to send netlink packet with an rtattr depth of %d!\n", ctx->attrDepth);
		return -1;
	}
	ctx->nlmsg->nlmsg_len = NLMSG_LENGTH((char*)nlBufferTail(ctx) - (char*)NLMSG_DATA(ctx->nlmsg));
	ctx->iov.iov_base = ctx->nlmsg;
	ctx->iov.iov_len = ctx->nlmsg->nlmsg_len;

	// TODO
	lprintf(LogDebug, "Sending:\n");
	for (size_t i = 0; i < ctx->msgBufferLen; ++i) {
		lprintDirectf(LogDebug, "%02X", (unsigned char)((char*)ctx->msgBuffer)[i]);
		if (i % 4 == 3) lprintDirectf(LogDebug, " ");
		if (i % 16 == 15) lprintDirectf(LogDebug, "\n");
	}
	lprintDirectf(LogDebug,"\n");

	while (true) {
		lprintf(LogDebug, "Sending netlink message %lu\n", ctx->nlmsg->nlmsg_seq);
		errno = 0;
		if (sendmsg(ctx->sock, &ctx->msg, 0) == -1) {
			if (errno == EAGAIN || errno == EINTR) continue;
			lprintf(LogError, "Error when sending netlink request to the kernel: %s\n", strerror(errno));
			return errno;
		} else break;
	}

	if (!waitResponse) return 0;

	// Cache sent information to prevent losing it when reusing the buffer
	__u32 seq = ctx->nlmsg->nlmsg_seq;

	// We reuse the send buffers for receiving to avoid extra allocations
	nlResetSpace(ctx, 4096);
	ctx->iov.iov_base = ctx->msgBuffer;
	ctx->iov.iov_len = ctx->msgBufferCap;
	struct sockaddr_nl fromAddr = { AF_NETLINK, 0, 0, 0 };
	ctx->msg.msg_name = &fromAddr;

	bool foundResponse = false;
	while (!foundResponse) {
		errno = 0;
		int res = recvmsg(ctx->sock, &ctx->msg, 0);
		if (res < 0) {
			if (res == ENOBUFS) {
				lprintln(LogWarning, "Kernel ran out of memory when sending netlink responses. View of state may be desynchronized, resulting in potential stalls!");
				continue;
			}
			if (res == EAGAIN || errno == EINTR) continue;
			lprintf(LogError, "Netlink socket read error: %s\n", strerror(errno));
			return errno;
		}
		if (res == 0) {
			lprintln(LogError, "Netlink socket was closed by the kernel");
			return -1;
		}
		if (ctx->msg.msg_namelen != sizeof(fromAddr)) {
			lprintln(LogError, "Netlink response used wrong address protocol");
			return -1;
		}

		// TODO
		lprintf(LogDebug, "Received:\n");
		for (size_t i = 0; i < res; ++i) {
			lprintDirectf(LogDebug, "%02X", (unsigned char)((char*)ctx->msgBuffer)[i]);
			if (i % 4 == 3) lprintDirectf(LogDebug, " ");
			if (i % 16 == 15) lprintDirectf(LogDebug, "\n");
		}
		lprintDirectf(LogDebug,"\n");

		for (struct nlmsghdr* nlm = ctx->msgBuffer; NLMSG_OK(nlm, res); nlm = NLMSG_NEXT(nlm, res)) {
			if (nlm->nlmsg_type == NLMSG_DONE) break;
			if (nlm->nlmsg_seq != seq) {
				// We ignore responses to previous messages, since we were not
				// interested in the errors when the calls were made
				continue;
			}
			foundResponse = true;
			if (nlm->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr* nlerr = NLMSG_DATA(nlm);
				if (nlerr->error != 0) {
					// Errors reported by the kernel are negative
					lprintf(LogError, "Netlink-reported error: %s\n", strerror(-nlerr->error));
					return -nlerr->error;
				}
			}
			if (handler != NULL) {
				int userError = handler(ctx, NLMSG_DATA(nlm), NLMSG_PAYLOAD(nlm, 0), nlm->nlmsg_type, nlm->nlmsg_flags, arg);
				if (userError != 0) return userError;
			}
		}
	}
	lprintf(LogDebug, "Kernel acknowledged netlink message %d\n", seq);
	return 0;
}
