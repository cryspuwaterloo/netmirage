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
int workAddRoot(void);

// Creates a new virtual host in its own network namespace. If the node is a
// client, then it is connected to the root.
int workAddHost(nodeId id, const TopoNode* node);

// Adds a virtual connection between two hosts.
int workAddLink(nodeId sourceId, nodeId targetId, const TopoLink* link);

// Destroys all hosts created with the network prefix. If deletedHosts is not
// NULL, the number of deleted hosts is stored. If an error was encountered, the
// value of deletedHosts is undefined.
int workDestroyHosts(uint32_t* deletedHosts);

// Waits until all submitted work has been completed and releases all resources.
int workJoin(void);
