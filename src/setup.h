#pragma once

// This module contains functions that are responsible for setting up virtual
// network topologies. The module is meant to allow for multiple functions that
// read from different types of sources (e.g., different file formats). The
// functions are not thread-safe.

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	const char* nsPrefix; // Prefix for network namespaces

	// srcFile is the path to a file containing the network topology in the
	// appropriate format. If it is NULL, then stdin is used instead.
	const char* srcFile;

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
int setupCleanup();

// Sets up a virtual network from a GraphML topology. Returns 0 on success or an
// error code otherwise.
int setupGraphML(const setupGraphMLParams* gmlParams);

// Destroys a previous network. Returns 0 on success or an error code otherwise.
int destroyNetwork();
