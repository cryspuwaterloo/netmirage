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
#include "graphml.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libxml/parser.h>

#include "log.h"
#include "mem.h"
#include "topology.h"

typedef struct {
	xmlChar* data;
	size_t cap;
} xmlCharBuffer;

const size_t DefaultXmlBufferLen = 255; // Arbitrary default; very generous
                                        // for GraphML identifiers

static void initXmlCharBuffer(xmlCharBuffer* buffer) {
	flexBufferInit((void**)&buffer->data, NULL, &buffer->cap);
}

static void freeXmlCharBuffer(xmlCharBuffer* buffer) {
	flexBufferFree((void**)&buffer->data, NULL, &buffer->cap);
}

// Copy a libxml string quickly, reallocating the target buffer if needed.
// We do this to prevent allocating memory for every element in the graph.
static void copyXmlStr(xmlCharBuffer* dst, const xmlChar* src) {
	// xmlChars represent bytes of UTF-8 code points. UTF-8 never contains a NUL
	// except for the final terminator, so we can treat these like C-strings for
	// the purpose of copying.
	size_t srcLen = strlen((const char*)src);
	eaddSize(srcLen, 1, &srcLen);
	flexBufferGrow((void**)&dst->data, 0, &dst->cap, srcLen, 1);
	flexBufferAppend(dst->data, NULL, src, srcLen, 1);
}

typedef enum {
	GpUnknown,	// Inside an unknown element
	GpInitial,	// Looking for initial graphml element
	GpTopLevel,	// Waiting for keys or the graph
	GpGraph,	// Inside the graph element
	GpNode,		// Inside a node element
	GpEdge,		// Inside an edge element
	GpData,		// Inside a data element
} GraphParserMode;

// Parser state for GraphML files
typedef struct {
	GraphParserMode mode;

	// Value used in nodes' type attribute to indicate that they are a client
	const char* clientType;

	// Keys for shortest path detection
	const char* weightKey;
	const char* weightKeyUp;
	const char* weightKeyDown;

	// GpUnknown state
	unsigned int unknownDepth;		// The depth since the first unknown element
	GraphParserMode unknownMode;	// The previous mode

	// Graph parameters
	bool defaultUndirected;	// True if edges are undirected by default

	// Attribute identifiers
	struct {
		xmlChar* typeId;
		xmlChar* packetLossId;
		xmlChar* bandwidthUpId;
		xmlChar* bandwidthDownId;
	} nodeAttribs;
	struct {
		xmlChar* weightId; // Shortest path key. May be the same as another key.
		xmlChar* latencyId;
		xmlChar* packetLossId;
		xmlChar* jitterId;
		xmlChar* queueLenId;
		xmlChar* weightUpId;
		xmlChar* latencyUpId;
		xmlChar* packetLossUpId;
		xmlChar* jitterUpId;
		xmlChar* queueLenUpId;
		xmlChar* weightDownId;
		xmlChar* latencyDownId;
		xmlChar* packetLossDownId;
		xmlChar* jitterDownId;
		xmlChar* queueLenDownId;
	} edgeAttribs;

	// Attribute values
	xmlCharBuffer dataKey;		// Key for the data being parsed
	xmlChar* dataValue;			// Buffer for the data value
	size_t dataValueLen;		// Size of the data value buffer contents
	size_t dataValueCap;		// Capacity of the data value buffer
	GraphParserMode dataMode;	// Mode to return to after parsing the data

	// Node and link objects used to pass to the callers
	GmlNode node;
	xmlCharBuffer nodeId;
	GmlLink link;
	xmlCharBuffer linkSourceId;
	xmlCharBuffer linkTargetId;

	// Error handling state
	bool partialError;	// True if libxml sent an error without a newline
	bool dead;			// True if a fatal error has been encountered

	// Data necessary for calling back to the client
	void* userData;
	NewNodeFunc newNodeFunc;
	NewLinkFunc newLinkFunc;
	int userError;
} GraphParserState;

// Prepend internal libxml errors
static void showXmlError(void* ctx, const char* msg, ...) {
	va_list args;
	va_start(args, msg);

	// Render the message into memory so that we can examine its contents
	char* errBuffer;
	if (newVsprintf(&errBuffer, msg, args) == -1) {
		lprintln(LogError, "Could not display libxml error");
	} else {
		// Redirect the error to our logging system
		GraphParserState* state = (GraphParserState*)ctx;
		if (!state->partialError) {
			lprintHead(LogError);
			lprintDirectf(LogError, "libxml error while parsing GraphML file: ");
		}
		lprintDirectf(LogError, "%s", errBuffer);

		// The error is considered partial if it is non-empty and doesn't end
		// with a newline
		char* p;
		for (p = errBuffer; *p; ++p);
		state->partialError = (errBuffer[0] != '\0' && p[-1] != '\n');
		if (!state->partialError) {
			lprintDirectFinish(LogError);
		}

		free(errBuffer);
	}

	va_end(args);
}

// Display a parsing error and terminate the parsing
static void graphFatalError(GraphParserState* state, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	lprintHead(LogError);
	lprintDirectf(LogError, "GraphML parse error: ");
	lvprintDirectf(LogError, fmt, args);
	lprintDirectFinish(LogError);
	va_end(args);
	state->dead = true;
}

static void initGraphParserState(GraphParserState* state, NewNodeFunc newNode, NewLinkFunc newLink, void* userData, const char* clientType, const char* weightKey) {
	state->clientType = clientType;
	state->weightKey = weightKey;
	size_t keylen = strlen(weightKey);
	char* weightKeyUp = emalloc(keylen+3);
	strcpy(weightKeyUp, weightKey);
	state->weightKeyUp = strcat(weightKeyUp, "up");
	char* weightKeyDown = emalloc(keylen+5);
	strcpy(weightKeyDown, weightKey);
	state->weightKeyDown = strcat(weightKeyDown, "down");
	state->newNodeFunc = newNode;
	state->newLinkFunc = newLink;
	state->userData = userData;
	state->mode = GpInitial;
	state->defaultUndirected = false;
	initXmlCharBuffer(&state->dataKey);
	flexBufferInit((void**)&state->dataValue, &state->dataValueLen, &state->dataValueCap);
	flexBufferGrow((void**)&state->dataValue, state->dataValueLen, &state->dataValueCap, DefaultXmlBufferLen, 1);
	initXmlCharBuffer(&state->nodeId);
	initXmlCharBuffer(&state->linkSourceId);
	initXmlCharBuffer(&state->linkTargetId);
	state->partialError = false;
	state->dead = false;
	state->userError = 0;
	memset(&state->nodeAttribs, 0, sizeof(state->nodeAttribs));
	memset(&state->edgeAttribs, 0, sizeof(state->edgeAttribs));
	xmlSetGenericErrorFunc(state, &showXmlError);
}

static void cleanupGraphParserState(GraphParserState* state) {
	freeXmlCharBuffer(&state->dataKey);
	flexBufferFree((void**)&state->dataValue, &state->dataValueLen, &state->dataValueCap);
	freeXmlCharBuffer(&state->nodeId);
	freeXmlCharBuffer(&state->linkSourceId);
	freeXmlCharBuffer(&state->linkTargetId);

	// Free attribute identifiers. Written this way to avoid another place with
	// explicit names.
	#define FREE_ATTRIBS(objType) do{ \
		for (size_t i = 0; i < sizeof(state->objType##Attribs); i += sizeof(xmlChar*)) { \
			xmlChar** p = (xmlChar**)(((char*)&state->objType##Attribs) + i); \
			if (*p) free(*p); \
		} }while(0)
	FREE_ATTRIBS(node);
	FREE_ATTRIBS(edge);
}

static void graphStartElement(void* ctx, const xmlChar* name, const xmlChar** atts) {
	GraphParserState* state = (GraphParserState*)ctx;
	if (state->dead) return;
	bool unknown = false;

	switch (state->mode) {
	case GpUnknown:
		++state->unknownDepth;
		break;

	case GpInitial:
		if (!xmlStrEqual(name, (const xmlChar*)"graphml")) {
			graphFatalError(state, "The topology file is not a GraphML file.\n");
			break;
		}
		for (const xmlChar** att = atts; *att; att += 2) {
			if (xmlStrEqual(att[0], (const xmlChar*)"xmlns")) {
				if (!xmlStrEqual(att[1], (const xmlChar*)"http://graphml.graphdrawing.org/xmlns")) {
					graphFatalError(state, "The topology file used an unknown GraphML namespace.\n");
				}
				break;
			}
		}
		state->mode = GpTopLevel;
		break;

	case GpTopLevel:
		if (xmlStrEqual(name, (const xmlChar*)"key")) {
			const xmlChar* keyName = NULL;
			const xmlChar* id = NULL;
			const xmlChar* type = NULL;
			const xmlChar* keyFor = NULL;
			for (const xmlChar** att = atts; *att; att += 2) {
				if (xmlStrEqual(att[0], (const xmlChar*)"attr.name")) keyName = att[1];
				else if (xmlStrEqual(att[0], (const xmlChar*)"id")) id = att[1];
				else if (xmlStrEqual(att[0], (const xmlChar*)"attr.type")) type = att[1];
				else if (xmlStrEqual(att[0], (const xmlChar*)"for")) keyFor = att[1];
			}

			if (keyName && id && type && keyFor) {
				// Convenience macro used to record attribute identifiers.
				// We store it explicitly rather than using a hash map for
				// performance reasons.
				#define CHECK_SET_ATTR(key, acceptInt, acceptFloat, acceptStr, objType, attr) \
					if (xmlStrEqual(keyName, (const xmlChar*)key)) { \
						bool correctType = false; \
						if (xmlStrEqual(type, (const xmlChar*)"int") || xmlStrEqual(type, (const xmlChar*)"long")) correctType = (acceptInt); \
						else if (xmlStrEqual(type, (const xmlChar*)"float") || xmlStrEqual(type, (const xmlChar*)"double")) correctType = (acceptFloat); \
						else if (xmlStrEqual(type, (const xmlChar*)"string")) correctType = (acceptStr); \
						if (!correctType) { \
							graphFatalError(state, "The key '%s' in the topology file had unexpected type '%s'.\n", key, type); \
						} else state->objType##Attribs.attr##Id = xmlStrdup(id); \
					}

				// Record the attribute if it is one of the known ones
				if (xmlStrEqual(keyFor, (const xmlChar*)"node")) {
					CHECK_SET_ATTR("type", false, false, true, node, type)
					else CHECK_SET_ATTR("packetloss", true, true, false, node, packetLoss)
					else CHECK_SET_ATTR("bandwidthup", true, true, false, node, bandwidthUp)
					else CHECK_SET_ATTR("bandwidthdown", true, true, false, node, bandwidthDown)
				} else if (xmlStrEqual(keyFor, (const xmlChar*)"edge")) {
					CHECK_SET_ATTR(state->weightKey, false, true, false, edge, weight)
					else CHECK_SET_ATTR(state->weightKeyUp, false, true, false, edge, weightUp)
					else CHECK_SET_ATTR(state->weightKeyDown, false, true, false, edge, weightDown)
					CHECK_SET_ATTR("latency", true, true, false, edge, latency)
					else CHECK_SET_ATTR("packetloss", true, true, false, edge, packetLoss)
					else CHECK_SET_ATTR("jitter", true, true, false, edge, jitter)
					else CHECK_SET_ATTR("queue_len", true, false, false, edge, queueLen)
					else CHECK_SET_ATTR("latencyup", true, true, false, edge, latencyUp)
					else CHECK_SET_ATTR("packetlossup", true, true, false, edge, packetLossUp)
					else CHECK_SET_ATTR("jitterup", true, true, false, edge, jitterUp)
					else CHECK_SET_ATTR("queue_lenup", true, false, false, edge, queueLenUp)
					else CHECK_SET_ATTR("latencydown", true, true, false, edge, latencyDown)
					else CHECK_SET_ATTR("packetlossdown", true, true, false, edge, packetLossDown)
					else CHECK_SET_ATTR("jitterdown", true, true, false, edge, jitterDown)
					else CHECK_SET_ATTR("queue_lendown", true, false, false, edge, queueLenDown)
				}
			}
			unknown = true;
		} else if (xmlStrEqual(name, (const xmlChar*)"graph")) {
			if (state->edgeAttribs.weightId == NULL && (state->edgeAttribs.weightUpId == NULL || state->edgeAttribs.weightDownId == NULL))
				graphFatalError(state, "The topology file did not include an edge parameter '%s' for route calculations. Specify --weight to use a different attribute.\n", state->weightKey);
			for (const xmlChar** att = atts; *att; att += 2) {
				if (xmlStrEqual(att[0], (const xmlChar*)"edgedefault")) {
					state->defaultUndirected = xmlStrEqual(att[1], (const xmlChar*)"undirected");
					break;
				}
			}
			state->mode = GpGraph;
		} else unknown = true;
		break;

	case GpGraph:
		if (xmlStrEqual(name, (const xmlChar*)"node")) {
			const xmlChar* id = NULL;
			for (const xmlChar** att = atts; *att; att += 2) {
				if (xmlStrEqual(att[0], (const xmlChar*)"id")) {
					id = att[1];
				}
			}
			if (!id) graphFatalError(state, "Topology contained a node without an identifier.\n");
			else {
				copyXmlStr(&state->nodeId, id);
				state->node.name = (const char*)state->nodeId.data;
				state->node.t.client = (state->clientType != NULL ? false : true);
				state->node.t.packetLoss = 0.0;
				state->node.t.bandwidthUp = 0;
				state->node.t.bandwidthDown = 0;
				state->mode = GpNode;
			}
		} else if (xmlStrEqual(name, (const xmlChar*)"edge")) {
			bool undirected = state->defaultUndirected;
			const xmlChar* source = NULL;
			const xmlChar* target = NULL;
			for (const xmlChar** att = atts; *att; att += 2) {
				if (xmlStrEqual(att[0], (const xmlChar*)"directed")) {
					if (xmlStrEqual(att[1], (const xmlChar*)"false")) {
						undirected = true;
					} else {
						undirected = false;
					}
				} else if (xmlStrEqual(att[0], (const xmlChar*)"source")) {
					source = att[1];
				} else if (xmlStrEqual(att[0], (const xmlChar*)"target")) {
					target = att[1];
				}
			}
			if (!source) graphFatalError(state, "Topology contained an edge that did not specify a source node.\n");
			else if (!target) graphFatalError(state, "Topology contained an edge that did not specify a target node.\n");
			else if (!undirected) graphFatalError(state, "Topology contained a directed edge from '%s' to '%s'. Only undirected edges are supported.\n", source, target);
			else {
				copyXmlStr(&state->linkSourceId, source);
				copyXmlStr(&state->linkTargetId, target);
				state->link.sourceName = (const char*)state->linkSourceId.data;
				state->link.targetName = (const char*)state->linkTargetId.data;
				state->link.weightUp = INFINITY;
				state->link.weightDown = INFINITY;
				state->link.t.latencyUp = 0.0;
				state->link.t.packetLossUp = 0.0;
				state->link.t.jitterUp = 0.0;
				state->link.t.latencyDown = 0.0;
				state->link.t.packetLossDown = 0.0;
				state->link.t.jitterDown = 0.0;
				state->link.t.queueLenUp = 0;
				state->link.t.queueLenDown = 0;
				state->mode = GpEdge;
			}
		} else unknown = true;
		break;

	case GpNode:
	case GpEdge:
		if (xmlStrEqual(name, (const xmlChar*)"data")) {
			bool foundKey = false;
			for (const xmlChar** att = atts; *att; att += 2) {
				if (xmlStrEqual(att[0], (const xmlChar*)"key")) {
					foundKey = true;
					copyXmlStr(&state->dataKey, att[1]);
					break;
				}
			}
			if (!foundKey) graphFatalError(state, "Topology contains a data attributed with no key.\n");
			state->dataValueLen = 0;
			state->dataMode = state->mode;
			state->mode = GpData;
		} else unknown = true;
		break;

	case GpData:
		unknown = true;
		break;

	default: graphFatalError(state, "BUG: Unknown GraphML parser state %d for startElement!\n", state->mode);
	}

	if (unknown) {
		state->unknownMode = state->mode;
		state->mode = GpUnknown;
		state->unknownDepth = 0;
	}
}

static void graphEndElement(void* ctx, const xmlChar* name) {
	GraphParserState* state = (GraphParserState*)ctx;
	if (state->dead) return;

	switch (state->mode) {
	case GpUnknown:
		if (state->unknownDepth == 0) {
			state->mode = state->unknownMode;
		} else {
			--state->unknownDepth;
		}
		break;

	case GpData: {
		// Add NUL terminator if characters were read
		const xmlChar* value = (const xmlChar*)"";
		if (state->dataValue) {
			state->dataValue[state->dataValueLen] = 0;
			value = state->dataValue;
		}

		switch (state->dataMode) {
		case GpNode:
			if (state->clientType != NULL && state->nodeAttribs.typeId && xmlStrEqual(state->dataKey.data, state->nodeAttribs.typeId)) {
				state->node.t.client = (xmlStrEqual(value, (const xmlChar*)state->clientType));
			} else if (state->nodeAttribs.packetLossId && xmlStrEqual(state->dataKey.data, state->nodeAttribs.packetLossId)) {
				state->node.t.packetLoss = strtod((const char*)value, NULL);
			} else if (state->nodeAttribs.bandwidthUpId && xmlStrEqual(state->dataKey.data, state->nodeAttribs.bandwidthUpId)) {
				state->node.t.bandwidthUp = strtod((const char*)value, NULL);
			} else if (state->nodeAttribs.bandwidthDownId && xmlStrEqual(state->dataKey.data, state->nodeAttribs.bandwidthDownId)) {
				state->node.t.bandwidthDown = strtod((const char*)value, NULL);
			}
			break;

		case GpEdge:
			if (state->edgeAttribs.weightId && xmlStrEqual(state->dataKey.data, state->edgeAttribs.weightId)) {
				state->link.weightUp = state->link.weightDown = strtof((const char*)value, NULL);
                                lprintf(LogDebug, "weight set to %f\n", state->link.weightUp);
			} else if (state->edgeAttribs.weightUpId && xmlStrEqual(state->dataKey.data, state->edgeAttribs.weightUpId)) {
				state->link.weightUp = strtof((const char*)value, NULL);
                                lprintf(LogDebug, "weightUp set to %f\n", state->link.weightUp);
			} else if (state->edgeAttribs.weightDownId && xmlStrEqual(state->dataKey.data, state->edgeAttribs.weightDownId)) {
				state->link.weightDown = strtof((const char*)value, NULL);
                                lprintf(LogDebug, "weightDown set to %f\n", state->link.weightDown);
			}
			if (state->edgeAttribs.latencyId && xmlStrEqual(state->dataKey.data, state->edgeAttribs.latencyId)) {
				state->link.t.latencyUp = state->link.t.latencyDown = strtod((const char*)value, NULL);
			} else if (state->edgeAttribs.packetLossId && xmlStrEqual(state->dataKey.data, state->edgeAttribs.packetLossId)) {
				state->link.t.packetLossUp = state->link.t.packetLossDown = strtod((const char*)value, NULL);
			} else if (state->edgeAttribs.jitterId && xmlStrEqual(state->dataKey.data, state->edgeAttribs.jitterId)) {
				state->link.t.jitterUp = state->link.t.jitterDown = strtod((const char*)value, NULL);
			} else if (state->edgeAttribs.queueLenId && xmlStrEqual(state->dataKey.data, state->edgeAttribs.queueLenId)) {
				state->link.t.queueLenUp = state->link.t.queueLenDown = (uint32_t)strtoul((const char*)value, NULL, 10);
			} else if (state->edgeAttribs.latencyUpId && xmlStrEqual(state->dataKey.data, state->edgeAttribs.latencyUpId)) {
				state->link.t.latencyUp = strtod((const char*)value, NULL);
                                lprintf(LogDebug, "latencyUp set to %f\n", state->link.t.latencyUp);
			} else if (state->edgeAttribs.packetLossUpId && xmlStrEqual(state->dataKey.data, state->edgeAttribs.packetLossUpId)) {
				state->link.t.packetLossUp = strtod((const char*)value, NULL);
			} else if (state->edgeAttribs.jitterUpId && xmlStrEqual(state->dataKey.data, state->edgeAttribs.jitterUpId)) {
				state->link.t.jitterUp = strtod((const char*)value, NULL);
			} else if (state->edgeAttribs.queueLenUpId && xmlStrEqual(state->dataKey.data, state->edgeAttribs.queueLenUpId)) {
				state->link.t.queueLenUp = (uint32_t)strtoul((const char*)value, NULL, 10);
			} else if (state->edgeAttribs.latencyDownId && xmlStrEqual(state->dataKey.data, state->edgeAttribs.latencyDownId)) {
				state->link.t.latencyDown = strtod((const char*)value, NULL);
                                lprintf(LogDebug, "latencyDown set to %f\n", state->link.t.latencyDown);
			} else if (state->edgeAttribs.packetLossDownId && xmlStrEqual(state->dataKey.data, state->edgeAttribs.packetLossDownId)) {
				state->link.t.packetLossDown = strtod((const char*)value, NULL);
			} else if (state->edgeAttribs.jitterDownId && xmlStrEqual(state->dataKey.data, state->edgeAttribs.jitterDownId)) {
				state->link.t.jitterDown = strtod((const char*)value, NULL);
			} else if (state->edgeAttribs.queueLenDownId && xmlStrEqual(state->dataKey.data, state->edgeAttribs.queueLenDownId)) {
				state->link.t.queueLenDown = (uint32_t)strtoul((const char*)value, NULL, 10);
			}
			break;

		default: graphFatalError(state, "BUG: Unknown GraphML data state %d when parsing key!\n", state->dataMode);
		}

		state->mode = state->dataMode;
		break;
	}

	case GpNode:
		state->userError = state->newNodeFunc(&state->node, state->userData);
		state->mode = GpGraph;
		break;

	case GpEdge:
		state->userError = state->newLinkFunc(&state->link, state->userData);
		state->mode = GpGraph;
		break;

	case GpGraph:
		state->mode = GpTopLevel;
		break;

	case GpTopLevel:
		state->mode = GpUnknown;
		state->unknownDepth = 0;
		state->unknownMode = GpUnknown;
		break;

	default: graphFatalError(state, "BUG: Unknown GraphML parser state %d for endElement!\n", state->mode);
	}

	if (state->userError != 0) {
		graphFatalError(state, "Terminating GraphML parsing due to client error code %d\n", state->userError);
	}
}

static void graphCharacters(void* ctx, const xmlChar* ch, int len) {
	GraphParserState* state = (GraphParserState*)ctx;
	if (state->dead) return;

	if (state->mode == GpData) {
		// Append the new UTF-8 characters to the data buffer
		flexBufferGrow((void**)&state->dataValue, state->dataValueLen, &state->dataValueCap, (size_t)len, 1);
		flexBufferAppend(state->dataValue, &state->dataValueLen, ch, (size_t)len, 1);
	}
}

// Callbacks for parsing GraphML files with libxml SAX interface
static xmlSAXHandler graphHandlers = {
	startElement: &graphStartElement,
	endElement: &graphEndElement,
	characters: &graphCharacters,

	warning: &showXmlError,
	error: &showXmlError,
	fatalError: &showXmlError,
};

int gmlParse(FILE* input, NewNodeFunc newNode, NewLinkFunc newLink, void* userData, const char* clientType, const char* weightKey) {
#ifdef LIBXML_PUSH_ENABLED
	GraphParserState state;
	initGraphParserState(&state, newNode, newLink, userData, clientType, weightKey);
	xmlParserCtxtPtr xmlContext = NULL;
	int err = 0;

	// Read the input in chunks
	const size_t chunkSize = 1024 * 8; // Must fit in an int
	size_t read;
	char* buffer = emalloc(chunkSize);
	do {
		read = fread(buffer, 1, chunkSize, input);
		if (read > 0) {
			if (!xmlContext) {
				// Initialize the context using the first chunk to detect the
				// encoding
				xmlContext = xmlCreatePushParserCtxt(&graphHandlers, &state, buffer, (int)read, NULL);
				if (!xmlContext) {
					err = -1;
				} else {
					xmlSetGenericErrorFunc(xmlContext, &showXmlError);
				}
			} else {
				err = xmlParseChunk(xmlContext, buffer, (int)read, 0);
			}
		}
	} while (read == chunkSize && !err);

	// If there were no problems, terminate the reading
	if (xmlContext && !err) {
		err = xmlParseChunk(xmlContext, NULL, 0, 1);
	}
	free(buffer);
	cleanupGraphParserState(&state);

	// Handle errors
	int readError = ferror(input);
	int error = readError ? readError : err;
	if (error) {
		lprintf(LogError, "Failed to parse the GraphML file (error: %d). The document may be malformed.", error);
	}
	return error;
#else
	lprintln(LogError, "Reading GraphML content in this mode is not supported because libxml was compiled without push parser support");
	return -1;
#endif
}

// Determines the proper return value given a set of parsing errors
static int reportErrors(const GraphParserState* state, int libXmlResult) {
	if (state->userError != 0) return state->userError;
	if (libXmlResult != 0) return libXmlResult;
	if (state->dead) return 1;
	return 0;
}

int gmlParseFile(const char* filename, NewNodeFunc newNode, NewLinkFunc newLink, void* userData, const char* clientType, const char* weightKey) {
	GraphParserState state;
	initGraphParserState(&state, newNode, newLink, userData, clientType, weightKey);
	int result = xmlSAXUserParseFile(&graphHandlers, &state, filename);
	cleanupGraphParserState(&state);
	return reportErrors(&state, result);
}

int gmlParseMemory(char* buffer, int size, NewNodeFunc newNode, NewLinkFunc newLink, void* userData, const char* clientType, const char* weightKey) {
	GraphParserState state;
	initGraphParserState(&state, newNode, newLink, userData, clientType, weightKey);
	int result = xmlSAXUserParseMemory(&graphHandlers, &state, buffer, size);
	cleanupGraphParserState(&state);
	return reportErrors(&state, result);
}
