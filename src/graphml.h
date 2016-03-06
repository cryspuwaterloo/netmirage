#pragma once

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

// Initializes the module. Should be called before any of the parse* functions
void initGraphParser();

// Node identifiers are generated automatically; the source GraphML file uses strings.
// We use 32 bits, which is large enough to simulate a fully populated IPv4 space.
typedef uint32_t id_t;

// A node represents a client or AS in a network graph
typedef struct {
	id_t Id;
	bool Client;
} GraphNode;

// A link represents a network connection between nodes
typedef struct {
	id_t SourceId;
	id_t TargetId;
} GraphLink;

// Callback for when nodes are read from the GraphML file
typedef void (*NewNodeFunc)(const GraphNode* node, void* userData);

// Callback for when links are read from the GraphML file
typedef void (*NewLinkFunc)(const GraphLink* link, void* userData);

// Parses a GraphML file from a stream. Returns 0 for success.
int parseGraph(FILE* input, NewNodeFunc newNode, NewLinkFunc newLink, void* userData);

// Parses a GraphML file stored on the disk. Returns 0 for success.
int parseGraphFile(const char* filename, NewNodeFunc newNode, NewLinkFunc newLink, void* userData);

// Parses a GraphML file stored in memory. Returns 0 for success.
int parseGraphMemory(char* buffer, int size, NewNodeFunc newNode, NewLinkFunc newLink, void* userData);
