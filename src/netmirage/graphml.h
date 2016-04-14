#pragma once

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "topology.h"

typedef struct {
	// An opaque string token identifying the node in the file's namespace. Its
	// encoding is undefined, and thus should not be shown to the user.
	const char* name;

	TopoNode t;
} GmlNode;

typedef struct {
	// Opaque string tokens denoting the start and end of the link
	const char* sourceName;
	const char* targetName;

	float weight;
	TopoLink t;
} GmlLink;

// Callback for when nodes are read from the GraphML file. Node is valid only
// for the duration of the call. Non-zero return values terminate parsing.
typedef int (*NewNodeFunc)(const GmlNode* node, void* userData);

// Callback for when links are read from the GraphML file. Link is valid only
// for the duration of the call. Non-zero return values terminate parsing.
typedef int (*NewLinkFunc)(const GmlLink* link, void* userData);

// Parses a GraphML file from a stream. Returns 0 for success.
int gmlParse(FILE* input, NewNodeFunc newNode, NewLinkFunc newLink, void* userData, const char* clientType, const char* weightKey);

// Parses a GraphML file stored on the disk. Returns 0 for success.
int gmlParseFile(const char* filename, NewNodeFunc newNode, NewLinkFunc newLink, void* userData, const char* clientType, const char* weightKey);

// Parses a GraphML file stored in memory. Returns 0 for success.
int gmlParseMemory(char* buffer, int size, NewNodeFunc newNode, NewLinkFunc newLink, void* userData, const char* clientType, const char* weightKey);
