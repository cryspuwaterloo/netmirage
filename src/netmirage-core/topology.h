#pragma once

// Common definitions of logical network topology structures

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t nodeId;
#define MAX_NODE_ID     (UINT32_MAX-1)
#define INVALID_NODE_ID (UINT32_MAX)
// Maximum number of chars required for a decimal representation, with
// terminator:
#define MAX_NODE_ID_BUFLEN (11)

// A node represents a client or AS in a network graph
typedef struct {
	bool client;
	double packetLoss;
	double bandwidthUp;
	double bandwidthDown;
} TopoNode;

// A link represents a network connection between nodes
typedef struct {
	double latency;
	double packetLoss;
	double jitter;
	uint32_t queueLen;
} TopoLink;
