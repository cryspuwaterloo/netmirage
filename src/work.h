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

extern const size_t NeededMacsClient;
extern const size_t NeededMacsLink;

// Initializes the work subsystem. Free resources with workJoin. nsPrefix is
// expected to be valid until workCleanup is called.
int workInit(const char* nsPrefix, uint64_t softMemCap);

// Frees all resources associated with the work subsystem.
int workCleanup(void);

// Determines the MAC address of an edge node connected to a physical interface.
// The semantics are the same as netGetMacAddr.
int workGetEdgeMac(const char* intfName, ip4Addr ip, macAddr* result);

// Creates a network namespace called the "root", which provides connectivity to
// the external world.
int workAddRoot(ip4Addr addr);

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
// the range that the client node is responsible for.
int workAddClientRoutes(nodeId clientId, const ip4Subnet* subnet);

// Destroys all hosts created with the network prefix. If deletedHosts is not
// NULL, the number of deleted hosts is stored. If an error was encountered, the
// value of deletedHosts is undefined.
int workDestroyHosts(uint32_t* deletedHosts);

// Waits until all submitted work has been completed and releases all resources.
int workJoin(void);
