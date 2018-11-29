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

// This is the portion of the work system that actually performs system calls.
// Each worker is meant to operate in its own process, in order to have its own
// active namespace, and to isolate administrative privileges from the main I/O
// portion of the program.

#include <stdbool.h>
#include <stdint.h>

#include "ip.h"
#include "topology.h"

// Checks to see if the current thread has the required capabilities to be a
// worker thread.
bool workerHaveCap(void);

// Drops the effective capabilities of the thread to only those necessary for a
// worker thread.
bool workerDropCap(void);

// Drops all effective capabilities of the thread.
bool workerDropAllCap(void);

// Initialize the current process as a worker process.
int workerInit(const char* nsPrefix, const char* ovsDirArg, const char* ovsSchemaArg, uint64_t softMemCap);

int workerCleanup(void);

// Actual order implementations. See work.h for documentation.
int workerGetEdgeRemoteMac(const char* intfName, ip4Addr ip, macAddr* edgeRemoteMac);
int workerGetEdgeLocalMac(const char* intfName, macAddr* edgeLocalMac);
int workerGetInterfaceMtu(const char* intfName, int* mtu);
int workerAddRoot(ip4Addr addrSelf, ip4Addr addrOther, int mtu, bool useInitNs, bool existing);
int workerAddEdgeInterface(const char* intfName);
int workerAddHost(nodeId id, ip4Addr ip, macAddr macs[], int mtu, const TopoNode* node);
int workerSetSelfLink(nodeId id, const TopoLink* link);
int workerEnsureSystemScaling(uint64_t linkCount, nodeId nodeCount, nodeId clientNodes);
int workerAddLink(nodeId sourceId, nodeId targetId, ip4Addr sourceIp, ip4Addr targetIp, macAddr macs[], int mtu, const TopoLink* link);
int workerAddInternalRoutes(nodeId id1, nodeId id2, ip4Addr ip1, ip4Addr ip2, const ip4Subnet* subnet1, const ip4Subnet* subnet2);
int workerAddClientRoutes(nodeId clientId, macAddr clientMacs[], const ip4Subnet* subnet, uint32_t edgePort, uint32_t clientPorts[]);
int workerAddEdgeRoutes(const ip4Subnet* edgeSubnet, uint32_t edgePort, const macAddr* edgeLocalMac, const macAddr* edgeRemoteMac);
int workerDestroyHosts(void);
