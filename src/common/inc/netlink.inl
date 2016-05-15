/*******************************************************************************
 * Copyright © 2016 Nik Unger, Ian Goldberg, Qatar University, and the Qatar
 * Foundation for Education, Science and Community Development.
 *
 * This file is part of NetMirage.
 *
 * NetMirage is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * NetMirage is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with NetMirage. If not, see <http://www.gnu.org/licenses/>.
 *******************************************************************************/

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

struct nlContext {
	int sock;
	uint32_t nextSeq;

	struct sockaddr_nl localAddr;

	struct msghdr msg;
	struct iovec iov;

	size_t attrNestPos[MAX_ATTR_NEST];
	size_t attrDepth;
};