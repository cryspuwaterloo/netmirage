// This file exposes the internal net namespace context structure. It is exposed
// so that it can be embedded for performance reasons.

// WARNING: Nothing except net.c should access the data members!

// Embed nlContext for performance
#include "netlink.inl"

struct netContext_s {
	int fd;
	int ioctlFd;
	nlContext nl;
};