#define _GNU_SOURCE

#include "app.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <argp.h>
#include <glib.h>

#include "log.h"
#include "mem.h"

// Buffer for string data. Each argument is duplicated individually, and the
// flexible buffer maintains a list of pointers to free. This way, applications
// can use the argument pointers immediately without worrying about freeing them
// or having them move.
static char** argBuf;
static size_t argBufLen;
static size_t argBufCap;

// Application configuration
static const char* appProductName;
static const char* appProductVersion;
static const char* appSetupOptGroup;
static int appSetupKey;
static int appLogFileKey;
static int appVerbosityKey;

// Configurable argp processing hook. This allows us to easily reconfigure a
// global parser for all argp option blocks.
static argp_parser_t argParser;

// Callbacks provided by the user
static appArgParser appCurrentArgParser;
static appSetupParser appCurrentSetupParser;

// Name of the setup file
static const char* argSetupFile;

static void argpVersion(FILE* stream, struct argp_state* state) {
	fprintf(stream, "%s %s\n", appProductName, appProductVersion);
}

void appInit(const char* productName, const char* productVersion) {
	argp_program_version_hook = &argpVersion;

	// If any errors appear during startup, we send them to stderr. However, our
	// convention is to print directly to stderr without logging decoration for
	// configuration errors.
	logSetStream(stderr);
	logSetThreshold(LogWarning);

	flexBufferInit((void**)&argBuf, &argBufLen, &argBufCap);

	appProductName = productName;
	appProductVersion = productVersion;
}

void appCleanup(void) {
	for (size_t i = 0; i < argBufLen; ++i) {
		free(((char**)argBuf)[i]);
	}
	flexBufferFree((void**)&argBuf, &argBufLen, &argBufCap);

	logCleanup();
}

error_t appParseArg(int key, char* arg, struct argp_state* state) {
	return argParser(key, arg, state);
}

// Argument parsing hook for finding setup file configuration
static error_t findSetupFile(int key, char* arg, struct argp_state* state) {
	if (key == appSetupKey) {
		argSetupFile = arg;
	}
	// We just pretend that we handle everything. The second pass will catch
	// unknown arguments.
	return 0;
}

static char* duplicateArg(char* arg) {
	char* argCopy = strdup(arg);
	flexBufferGrow((void**)&argBuf, argBufLen, &argBufCap, 1, sizeof(char*));
	flexBufferAppend(argBuf, &argBufLen, &argCopy, 1, sizeof(char*));
	return argCopy;
}

// Parsing hook for argp that duplicates strings and relays events to the user's
// callback function.
static error_t redirectDupArg(int key, char* arg, struct argp_state* state) {
	if (key == appSetupKey) return 0; // Already handled in first pass

	char* argCopy = NULL;
	if (arg != NULL) {
		argCopy = duplicateArg(arg);

		// We can handle some flags ourself, without resorting to telling the caller
		if (key == appLogFileKey) {
			if (!logSetFile(argCopy)) {
				fprintf(stderr, "Could not open log file '%s' for writing.\n", argCopy);
				return EINVAL;
			}
			return 0;
		} else if (key == appVerbosityKey) {
			long index = matchArg(argCopy, LogLevelStrings);
			if (index < 0) {
				fprintf(stderr, "Unknown logging level '%s'\n", argCopy);
				return EINVAL;
			}
			logSetThreshold(index);
			return 0;
		}
	}

	return appCurrentArgParser(key, argCopy, state, (state == NULL ? 0 : state->arg_num));
}

// Reads argp configuration settings from the setup key-value file. Returns true
// on success or false on failure.
static bool parseSetupAppOptions(GKeyFile* f, const struct argp* argp, const char** nonOptions) {
	if (argp->options != NULL) {
		for (const struct argp_option* option = argp->options; option->name != NULL || option->key != 0; ++option) {
			if (option->name == NULL) continue;

			gchar* val = g_key_file_get_string(f, appSetupOptGroup, option->name, NULL);
			if (val == NULL) continue;
			error_t err = redirectDupArg(option->key, val, NULL);
			g_free(val);
			if (err != 0 && err != ARGP_ERR_UNKNOWN) {
				fprintf(stderr, "In setup file: the configuration for application option \"%s\" was invalid: %s\n", option->name, strerror(err));
				return false;
			}
		}
	}
	if (argp->children != NULL) {
		for (const struct argp_child* child = argp->children; child->argp != NULL; ++child) {
			if (!parseSetupAppOptions(f, child->argp, NULL)) return false;
		}
	}
	if (nonOptions != NULL) {
		unsigned int argNum = 0;
		for (const char** nonOption = nonOptions; *nonOption != NULL; ++nonOption, ++argNum) {
			const char* longName = *nonOption;
			gchar* val = g_key_file_get_string(f, appSetupOptGroup, longName, NULL);
			if (val == NULL) continue;

			char* argCopy = duplicateArg((char*)val);
			g_free(val);
			error_t err = appCurrentArgParser(ARGP_KEY_ARG, argCopy, NULL, argNum);
			if (err != 0) {
				fprintf(stderr, "In setup file: the configuration for application non-option argument \"%s\" was invalid: %s\n", longName, strerror(err));
				return false;
			}
		}
	}
	return true;
}

// Tries to parse the options in a setup file specified in args.setupFile.
// Updates the settings in args. Returns true on success or false on failure.
static bool parseSetupFile(const struct argp* argp, bool mustExist, const char** nonOptions) {
	GError* gerr = NULL;
	bool res = false;

	// glib leaks global memory (according to the "still reachable but unfreed"
	// definition) by design. There is nothing that we can do to fix this.
	GKeyFile* f = g_key_file_new();

	gchar* filename = g_filename_from_utf8(argSetupFile, -1, NULL, NULL, &gerr);
	if (filename == NULL) {
		fprintf(stderr, "Could not convert the setup filename ('%s') from UTF-8 to the glib filename encoding: %s\n", argSetupFile, gerr->message);
		goto cleanup;
	}

	if (g_key_file_load_from_file(f, filename, G_KEY_FILE_NONE, &gerr) == FALSE) {
		if (mustExist) fprintf(stderr, "Failed to load setup file '%s': %s\n", argSetupFile, gerr->message);
		goto cleanup;
	}

	if (appCurrentSetupParser != NULL) {
		if (!appCurrentSetupParser(f)) {
			res = 1;
			goto cleanup;
		}
	}

	res = parseSetupAppOptions(f, argp, nonOptions);

cleanup:
	if (gerr != NULL) g_error_free(gerr);
	if (filename != NULL) g_free(filename);
	g_key_file_free(f);
	return res;
}

int appParseArgs(appArgParser parser, appSetupParser setupParser, struct argp* argp, const char* setupOptGroup, const char** nonOptions, int setupKey, int logFileKey, int verbosityKey, int argc, char** argv) {
	appCurrentArgParser = parser;
	appCurrentSetupParser = setupParser;
	appSetupOptGroup = setupOptGroup;
	appSetupKey = setupKey;
	appLogFileKey = logFileKey;
	appVerbosityKey = verbosityKey;

	// In our first argument pass, find out if a setup file was specified
	argParser = &findSetupFile;
	if (argp_parse(argp, argc, argv, 0, NULL, NULL) != 0) {
		argp_help(argp, stderr, ARGP_HELP_STD_USAGE, argv[0]);
		return 1;
	}
	bool explicitSetupFile = (argSetupFile != NULL);
	if (!explicitSetupFile) argSetupFile = DEFAULT_SETUP_FILE;

	// Setup file settings have higher priority than defaults. If we are using
	// the default setup file, we don't care if it doesn't exist.
	argParser = &redirectDupArg;
	bool setupSuccess = parseSetupFile(argp, explicitSetupFile, nonOptions);
	if (explicitSetupFile && !setupSuccess) return 1;

	// Now parse the arguments again (explicit arguments have higher priority
	// than anything in the setup file)
	if (argp_parse(argp, argc, argv, 0, NULL, NULL) != 0) {
		argp_help(argp, stderr, ARGP_HELP_STD_USAGE, argv[0]);
		return 1;
	}

	return 0;
}

long matchArg(const char* arg, const char* options[]) {
	if (arg[0]) {
		// First try converting arg to an index
		char* endptr;
		long userIndex = strtol(arg, &endptr, 10);
		if (endptr[0]) userIndex = -1; // String is not an index

		// Scan the options until we find a match
		long index = -1;
		const char* option;
		while ((option = options[++index]) != NULL) {
			if (index == userIndex || strcasecmp(arg, option) == 0) {
				return index;
			}
		}
	}
	return -1;
}
