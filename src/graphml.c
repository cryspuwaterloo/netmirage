#include "graphml.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <libxml/parser.h>

#include "log.h"

// Squash internal libxml errors
static void quashError(void* ctx, const char* msg, ...) {}

void initGraphParser() {
	xmlGenericErrorFunc err = &quashError;
	initGenericErrorDefaultFunc(&err);
}

// Parser state for GraphML files
typedef struct {
	// Internal parser state

	// Data necessary for calling back to the client
	void* userData;
	xmlSAXHandler Callbacks;
} GraphParserState;

// Generates libxml SAX2 callbacks for given GraphML callbacks, and the corresponding parser state.
static GraphParserState* genGraphParserState(NewNodeFunc newNode, NewLinkFunc newLink, void* userData) {
	GraphParserState* state = malloc(sizeof(GraphParserState));
	memset(&state->Callbacks, 0, sizeof(xmlSAXHandler));
	return state;
}

// Convenience function to clean up after parsing concludes
static int parserCleanup(int parseResult, GraphParserState* state) {
	free(state);
	return parseResult;
}

int parseGraph(FILE* input, NewNodeFunc newNode, NewLinkFunc newLink, void* userData) {
#ifdef LIBXML_PUSH_ENABLED
	GraphParserState* state = genGraphParserState(newNode, newLink, userData);
	xmlParserCtxtPtr xmlContext = NULL;
	int xmlError = 0;

	// Read the input in chunks
	const size_t chunkSize = 1024 * 8;
	size_t read;
	char* buffer = malloc(chunkSize);
	do {
		read = fread(buffer, 1, chunkSize, input);
		if (read > 0) {
			if (!xmlContext) {
				// Initialize the context using the first chunk to detect the encoding
				xmlContext = xmlCreatePushParserCtxt(&state->Callbacks, state, buffer, read, NULL);
				if (!xmlContext) {
					xmlError = -1;
				} else {
					xmlSetGenericErrorFunc(xmlContext, &quashError);
				}
			} else {
				xmlError = xmlParseChunk(xmlContext, buffer, read, 0);
			}
		}
	} while (read == chunkSize && !xmlError);

	// If there were no problems, terminate the reading
	if (xmlContext && !xmlError) {
		xmlError = xmlParseChunk(xmlContext, NULL, 0, 1);
	}

	// Cleanup
	free(buffer);
	int readError = parserCleanup(ferror(input), state);

	// Handle errors
	int error = readError ? readError : xmlError;
	if (error) {
		lprintf(LogError, "Failed to parse the GraphML file (error: %d). The document may be malformed.", error);
	}
	return error;
#else
	lprintln(LogError, "Reading GraphML content in this mode is not supported because libxml was compiled without push parser support");
	return -1;
#endif
}

int parseGraphFile(const char* filename, NewNodeFunc newNode, NewLinkFunc newLink, void* userData) {
	GraphParserState* state = genGraphParserState(newNode, newLink, userData);
	return parserCleanup(xmlSAXUserParseFile(&state->Callbacks, state, filename), state);
}

int parseGraphMemory(char* buffer, int size, NewNodeFunc newNode, NewLinkFunc newLink, void* userData) {
	GraphParserState* state = genGraphParserState(newNode, newLink, userData);
	return parserCleanup(xmlSAXUserParseMemory(&state->Callbacks, state, buffer, size), state);
}
