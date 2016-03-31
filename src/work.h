#pragma once

// This module provides a mechanism for actually performing the logical setup of
// virtual networks. It serves as a bridge between the "setup" module and the
// "net" module. All of the functions here are meant to be called from a single
// thread whose primary purpose is performing I/O operations. The calls are all
// asynchronous. If an error occurs, it is returned by subsequent calls before
// they perform their function. The join operation, which waits for all work to
// finish, will return any queued errors. A value of 0 indicates no error.

#include <stdint.h>
#include <stdlib.h>

#include "ip.h"
#include "topology.h"

#define NEEDED_MACS_LINK 2
#define NEEDED_MACS_CLIENT (2 * NEEDED_MACS_LINK)

// Initializes the work subsystem. Free resources with workJoin. nsPrefix,
// ovsDir, and ovsSchema are expected to be valid until workCleanup is called.
int workInit(const char* nsPrefix, const char* ovsDir, const char* ovsSchema, uint64_t softMemCap);

// Frees all resources associated with the work subsystem.
int workCleanup(void);

// Determines the MAC address of an edge node connected to a physical interface.
// The semantics are the same as netGetMacAddr.
int workGetEdgeRemoteMac(const char* intfName, ip4Addr ip, macAddr* edgeRemoteMac);

// Determines the MAC address of a physical interface connected to an edge node.
// Assumes that the interface has already been moved into the root namespace.
int workGetEdgeLocalMac(const char* intfName, macAddr* edgeLocalMac);

// Creates a network namespace called the "root", which provides connectivity to
// the external world.
int workAddRoot(ip4Addr addrSelf, ip4Addr addrOther);

// Adds an external interface to the root namespace. This removes it from the
// init namespace, so it will appear to vanish from a simple "ifconfig" listing.
// The interface is added to the switch, thereby connecting it to to virtual
// network. The port number of the interface in the bridge is returned in
// portId, which is needed to set up client routes. Returns 0 on success or an
// error code otherwise.
int workAddEdgeInterface(const char* intfName, uint32_t* portId);

// Creates a new virtual host in its own network namespace. If the node is a
// client, then it is connected to the root. If the node is a client, then macs
// should contain NeededMacsClient unique addresses.
int workAddHost(nodeId id, ip4Addr ip, macAddr macs[], const TopoNode* node);

// Applies traffic shaping parameters to a client node's "self" link.
int workSetSelfLink(nodeId id, const TopoLink* link);

// Sets system parameters to ensure that the kernel allocates enough resources
// for the network. This should be called before adding any links or routes.
int workEnsureSystemScaling(uint64_t linkCount, nodeId nodeCount, nodeId clientNodes);

// Adds a virtual connection between two hosts. macs should contain
// NeededMacsLink unique addresses.
int workAddLink(nodeId sourceId, nodeId targetId, ip4Addr sourceIp, ip4Addr targetIp, macAddr macs[], const TopoLink* link);

// Adds static routing paths for internal links. Node 1 will route packets for
// subnet2 through node 2. The reverse path is also set up.
int workAddInternalRoutes(nodeId id1, nodeId id2, ip4Addr ip1, ip4Addr ip2, const ip4Subnet* subnet1, const ip4Subnet* subnet2);

// Adds static routing paths between a client node and the root. The subnet is
// the range that the client node is responsible for. This also adds the
// associated flow rules to the switch in the root namespace. clientMacs should
// have the same value as the call to workAddHost. edgePort is the port
// identifier for the associated edge node interface, as returned by
// workAddEdgeInterface.
int workAddClientRoutes(nodeId clientId, macAddr clientMacs[], const ip4Subnet* subnet, uint32_t edgePort);

// Adds egression routes for an edge node to the switch in the root namespace.
// edgeLocalMac should be the MAC address associated with the edge interface,
// and edgeRemoteMav should be the MAC address of the remote edge node, as
// returned by workGetEdgeMac and workGetEdgeLocalMac.
int workAddEdgeRoutes(const ip4Subnet* edgeSubnet, uint32_t edgePort, const macAddr* edgeLocalMac, const macAddr* edgeRemoteMac);

// Destroys all hosts created with the network prefix. If an Open vSwitch
// instance is running for a root namespace, it is shut down and deleted. If
// deletedHosts is not NULL, the number of deleted hosts is stored. If an error
// was encountered, the value of deletedHosts is undefined.
int workDestroyHosts(uint32_t* deletedHosts);

// Waits until all submitted work has been completed and releases all resources.
int workJoin(void);
