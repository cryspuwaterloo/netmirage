#pragma once

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

// Node and link identifiers are NUL-terminated opaque char sequences.
// Client code should not display them to the user directly.

// A node represents a client or AS in a network graph
typedef struct {
	const char* Id;

	bool Client;
	double PacketLoss;
	double BandwidthUp;
	double BandwidthDown;
} GraphNode;

// A link represents a network connection between nodes
typedef struct {
	const char* SourceId;
	const char* TargetId;

	double Latency;
	double PacketLoss;
	double Jitter;
	uint32_t QueueLen;
} GraphLink;

// Callback for when nodes are read from the GraphML file.
// Node is valid only for the duration of the call.
typedef void (*NewNodeFunc)(const GraphNode* node, void* userData);

// Callback for when links are read from the GraphML file.
// Link is valid only for the duration of the call.
typedef void (*NewLinkFunc)(const GraphLink* link, void* userData);

// Parses a GraphML file from a stream. Returns 0 for success.
int parseGraph(FILE* input, NewNodeFunc newNode, NewLinkFunc newLink, void* userData, const char* clientType);

// Parses a GraphML file stored on the disk. Returns 0 for success.
int parseGraphFile(const char* filename, NewNodeFunc newNode, NewLinkFunc newLink, void* userData, const char* clientType);

// Parses a GraphML file stored in memory. Returns 0 for success.
int parseGraphMemory(char* buffer, int size, NewNodeFunc newNode, NewLinkFunc newLink, void* userData, const char* clientType);
