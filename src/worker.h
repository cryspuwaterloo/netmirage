#pragma once

// This is the portion of the work system that actually performs system calls.
// Each worker is meant to operate in its own process, in order to have its own
// active namespace, and to isolate administrative privileges from the main I/O
// portion of the program.

#include <stdint.h>

#include "ip.h"
#include "topology.h"

// Initialize the current process as a worker process.
int workerInit(const char* nsPrefix, const char* ovsDirArg, const char* ovsSchemaArg, uint64_t softMemCap);

int workerCleanup(void);

// Actual order implementations. See work.h for documentation.
int workerGetEdgeRemoteMac(const char* intfName, ip4Addr ip, macAddr* edgeRemoteMac);
int workerGetEdgeLocalMac(const char* intfName, macAddr* edgeLocalMac);
int workerAddRoot(ip4Addr addrSelf, ip4Addr addrOther, bool existing);
int workerAddEdgeInterface(const char* intfName, uint32_t* portId);
int workerAddHost(nodeId id, ip4Addr ip, macAddr macs[], const TopoNode* node);
int workerSetSelfLink(nodeId id, const TopoLink* link);
int workerEnsureSystemScaling(uint64_t linkCount, nodeId nodeCount, nodeId clientNodes);
int workerAddLink(nodeId sourceId, nodeId targetId, ip4Addr sourceIp, ip4Addr targetIp, macAddr macs[], const TopoLink* link);
int workerAddInternalRoutes(nodeId id1, nodeId id2, ip4Addr ip1, ip4Addr ip2, const ip4Subnet* subnet1, const ip4Subnet* subnet2);
int workerAddClientRoutes(nodeId clientId, macAddr clientMacs[], const ip4Subnet* subnet, uint32_t edgePort);
int workerAddEdgeRoutes(const ip4Subnet* edgeSubnet, uint32_t edgePort, const macAddr* edgeLocalMac, const macAddr* edgeRemoteMac);
int workerDestroyHosts(void);
