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