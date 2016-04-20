#pragma once

// This module provides some useful helper functions for top-level application
// logic, such as argument parsing.

#include <stdbool.h>
#include <stdio.h>

#include <argp.h>
#include <glib.h>

#define DEFAULT_SETUP_FILE "setup.cfg"

// productName is a human-readable string with the name of the software.
void appInit(const char* productName, const char* productVersion);

void appCleanup(void);

// Handler for processing argp data.
error_t appParseArg(int key, char* arg, struct argp_state* state);

// Callback function for arguments. The arguments have the same semantics as an
// argp_parser_t. If state is NULL, then the argument originated from a setup
// file rather than the command line. argNum has the value of state->arg_num, or
// the current non-option argument number for non-option arguments originating
// in setup files.
typedef error_t (*appArgParser)(int key, char* arg, struct argp_state* state, unsigned int argNum);

// Callback function for setup files. If false is returned, parsing is
// terminated with a failure.
typedef bool (*appSetupParser)(GKeyFile* file);

// Processes arguments from a combination of the command line and the setup
// file. The argp parser function must be set to appParseArg for argp and all of
// its children. Events will be passed to the specified parser. If an argument
// is read from the setup file, then parser is called with a NULL argp_state.
// If the setup file exists, then setupParser is called. The call order is not
// guaranteed; setupParser may be called before or after arguments are parsed.
// setupParser may be NULL, but parser must be specified. The argument provided
// to parser is a duplicate that is guaranteed to be valid for the lifetime of
// the program. The module will automatically handle the setup file, log file,
// and log verbosity options with the specified keys. nonOptions is either NULL
// or a NULL-terminated array of long names for the non-option arguments. These
// names are used to read the non-option arguments from the setup file. Returns
// 0 on success or an error code otherwise.
int appParseArgs(appArgParser parser, appSetupParser setupParser, struct argp* argp, const char* setupOptGroup, const char** nonOptions, int setupKey, int logFileKey, int verbosityKey, int argc, char** argv);

// Compares an argument to a list of possibilities and returns the matching
// index. Returns a negative value if unmatched.
long matchArg(const char* arg, const char* options[], struct argp_state* state);

