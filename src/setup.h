#pragma once

// This module contains functions that are responsible for setting up virtual
// network topologies. The module is meant to allow for multiple functions that
// read from different types of sources (e.g., different file formats). The
// functions are not thread-safe.

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "ip.h"

typedef struct {
	ip4Addr ip;            // The real IP address of the edge node
	char* intf;            // The interface connected to the edge node

	bool macSpecified;
	macAddr mac;           // The real MAC address of the edge node

	bool vsubnetSpecified;
	ip4Subnet vsubnet;     // The subnet for virtual clients in the edge node
} edgeNodeParams;

typedef struct {
	const char* nsPrefix; // Prefix for network namespaces

	// srcFile is the path to a file containing the network topology in the
	// appropriate format. If it is NULL, then stdin is used instead.
	const char* srcFile;

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

// Initializes the setup system with the given global parameters. Returns 0 on
// success or an error code otherwise.
int setupInit(const setupParams* params);

// Releases all setup-related resources. The program must not call any other
// setup calls after this.
int setupCleanup(void);

// Sets up a virtual network from a GraphML topology. Returns 0 on success or an
// error code otherwise.
int setupGraphML(const setupGraphMLParams* gmlParams);

// Destroys a previous network. Returns 0 on success or an error code otherwise.
int destroyNetwork(void);
