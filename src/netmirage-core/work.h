/*******************************************************************************
 * Copyright © 2018 Nik Unger, Ian Goldberg, Qatar University, and the Qatar
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
#pragma once

// This module provides a mechanism for actually performing the logical setup of
// virtual networks. It serves as a bridge between the "setup" module and the
// "net" module. All of the functions here are meant to be called from a single
// thread whose primary purpose is performing I/O operations. The calls are all
// asynchronous. If an error occurs, it is returned by subsequent calls before
// they perform their function. The join operation, which waits for all work to
// finish, will return any queued errors. Some calls automatically perform the
// join operation before or after executing; these cases are marked. A NULL
// return value indicates that no error was queued. If an error occurs while one
// is already queued, one of the errors will be dropped, so the caller is
// expected to cease all work operations after encountering an error.

#include <stdbool.h>
#include <stdint.h>

#include "ip.h"
#include "log.h"
#include "topology.h"

#define NEEDED_MACS_LINK 2
#define NEEDED_MACS_CLIENT (2 * NEEDED_MACS_LINK)
#define NEEDED_PORTS_CLIENT 2

// Initializes the work subsystem. Free resources with workCleanup.
// workConfigure must be called before sending any work commands.
int workInit(void);

// Sends configuration values to the initialized work subsystem.
int workConfigure(LogLevel logThreshold, bool logColorize, const char* nsPrefix, const char* ovsDir, const char* ovsSchema, uint64_t softMemCap);

// Frees all resources associated with the work subsystem. This function
// automatically joins before cleaning up.
int workCleanup(void);

// Determines the MAC address of an edge node connected to a physical interface.
// The semantics are the same as netGetMacAddr. Since this function returns a
// response, it automatically joins.
int workGetEdgeRemoteMac(const char* intfName, ip4Addr ip, macAddr* edgeRemoteMac);

// Determines the MAC address of a physical interface connected to an edge node.
// Assumes that the interface has already been moved into the root namespace.
// Since this function returns a response, it automatically joins.
int workGetEdgeLocalMac(const char* intfName, macAddr* edgeLocalMac);

// Determines the MTU of a physical interface. Assumes that the interface is in
// the default namespace. Since this function returns a response, it
// automatically joins.
int workGetInterfaceMtu(const char* intfName, int* mtu);

// Creates a network namespace called the "root", which provides connectivity to
// the external world.
int workAddRoot(ip4Addr addrSelf, ip4Addr addrOther, int mtu, bool useInitNs);

// Adds an external interface to the root namespace. This removes it from the
// init namespace, so it will appear to vanish from a simple "ifconfig" listing.
// The interface is added to the switch, thereby connecting it to to virtual
// network. Port numbers are assigned on a first-come-first-served basis, so the
// caller should ensure that workJoin is called between workAddEdgeInterface
// calls.
int workAddEdgeInterface(const char* intfName);

// Creates a new virtual host in its own network namespace. If the node is a
// client, then it is connected to the root. If the node is a client, then macs
// should contain NeededMacsClient unique addresses.
int workAddHost(nodeId id, ip4Addr ip, macAddr macs[], int mtu, const TopoNode* node);

// Applies traffic shaping parameters to a client node's "self" link.
int workSetSelfLink(nodeId id, const TopoLink* link);

// Sets system parameters to ensure that the kernel allocates enough resources
// for the network. This should be called before adding any links or routes.
int workEnsureSystemScaling(uint64_t linkCount, nodeId nodeCount, nodeId clientNodes);

// Adds a virtual connection between two hosts. macs should contain
// NeededMacsLink unique addresses.
int workAddLink(nodeId sourceId, nodeId targetId, ip4Addr sourceIp, ip4Addr targetIp, macAddr macs[], int mtu, const TopoLink* link);

// Adds static routing paths for internal links. Node 1 will route packets for
// subnet2 through node 2. The reverse path is also set up.
int workAddInternalRoutes(nodeId id1, nodeId id2, ip4Addr ip1, ip4Addr ip2, const ip4Subnet* subnet1, const ip4Subnet* subnet2);

// Adds static routing paths between a client node and the root. The subnet is
// the range that the client node is responsible for. This also adds the
// associated flow rules to the switch in the root namespace. clientMacs should
// have the same value as the call to workAddHost. edgePort is the port
// identifier for the associated edge node interface, as assigned during the
// workAddEdgeInterface call. nextOvsPort should be the next available port in
// the switch. This call will add NEEDED_PORTS_CLIENT ports to the switch. The
// caller should join between calls in order to prevent port number races.
int workAddClientRoutes(nodeId clientId, macAddr clientMacs[], const ip4Subnet* subnet, uint32_t edgePort, uint32_t nextOvsPort);

// Adds egression routes for an edge node to the switch in the root namespace.
// edgeLocalMac should be the MAC address associated with the edge interface,
// and edgeRemoteMac should be the MAC address of the remote edge node, as
// returned by workGetEdgeMac and workGetEdgeLocalMac.
int workAddEdgeRoutes(const ip4Subnet* edgeSubnet, uint32_t edgePort, const macAddr* edgeLocalMac, const macAddr* edgeRemoteMac);

// Destroys all hosts created with the network prefix. If an Open vSwitch
// instance is running for a root namespace, it is shut down and deleted. If
// deletedHosts is not NULL, the number of deleted hosts is stored. If an error
// was encountered, the value of deletedHosts is undefined.
int workDestroyHosts(void);

// Waits until all submitted work has been completed. If resetError is true,
// then all queued errors are ignored, and the error state of the subsystem is
// reset.
int workJoin(bool resetError);
