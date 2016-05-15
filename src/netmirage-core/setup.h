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
#pragma once

// This module contains functions that are responsible for setting up virtual
// network topologies. The module is meant to allow for multiple functions that
// read from different types of sources (e.g., different file formats). The
// functions are not thread-safe.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ip.h"

typedef struct {
	ip4Addr ip;            // The real IP address of the edge node
	char* intf;            // The interface connected to the edge node

	bool macSpecified;
	macAddr mac;           // The real MAC address of the edge node

	bool vsubnetSpecified;
	ip4Subnet vsubnet;     // The subnet for virtual clients in the edge node

	char* remoteDev;       // The interface on the edge node
	uint32_t remoteApps;   // The number of remote applications to configure
} edgeNodeParams;

typedef struct {
	const char* nsPrefix;  // Prefix for network namespaces
	const char* ovsDir;    // Directory for Open vSwitch files
	const char* ovsSchema; // Path to Open vSwitch's OVSDB schema

	bool destroyFirst;

	// srcFile is the path to a file containing the network topology in the
	// appropriate format. If it is NULL, then stdin is used instead.
	const char* srcFile;

	ip4Addr routingIp;

	// edgeFile is a path to a file to which edge settings will be written, or
	// NULL to indicate stdout.
	const char* edgeFile;
	bool quiet; // If true, edge information is not written

	bool rootIsInitNs; // If true, the "root" namespace is the init namespace

	edgeNodeParams* edgeNodes;
	size_t edgeNodeCount;
	struct {
		bool intfSpecified;
		const char* intf;
		ip4Subnet globalVSubnet;
	} edgeNodeDefaults;

	uint64_t softMemCap; // (Very) approximate memory use
} setupParams;

typedef struct {
	// Divisor to convert bandwidth rates in the GraphML file into Mbit/s.
	float bandwidthDivisor;

	bool twoPass; // True if the file contains node elements after edge elements

	const char* weightKey; // Data key used for static routing computation
	const char* clientType; // Value for "type" identifying client nodes
} setupGraphMLParams;

// Initializes the setup system. setupConfigure must be called before any
// source-specific setup functions. Returns 0 on success or an error code
// otherwise.
int setupInit(void);

// Configures the setup system with the given global parameters. Returns 0 on
// success or an error code otherwise.
int setupConfigure(const setupParams* params);

// Releases all setup-related resources. The program must not call any other
// setup calls after this.
int setupCleanup(void);

// Sets up a virtual network from a GraphML topology. Returns 0 on success or an
// error code otherwise.
int setupGraphML(const setupGraphMLParams* gmlParams);

// Destroys a previous network. Returns 0 on success or an error code otherwise.
int destroyNetwork(void);
