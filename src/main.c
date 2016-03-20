#include <linux/version.h>
#ifndef __linux__
#error "This program can only be compiled on Linux, since it uses Linux-specific networking features."
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0)
#error "This program must be compiled with Linux kernel version 3.3 or later."
#endif

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <argp.h>
#include <strings.h>
#include <libxml/parser.h>

#include "log.h"
#include "version.h"
#include "setup.h"

// Argp version and help configuration
static void argpVersion(FILE* stream, struct argp_state* state) {
	fprintf(stream, "%s\n", getVersionString());
}
void (*argp_program_version_hook)(FILE*, struct argp_state*) = &argpVersion;

// Compares an argument to a list of possibilities and returns the matching
// index. Errors if unmatched.
static long matchArg(const char* arg, const char* options[], struct argp_state* state) {
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
	argp_usage(state);
	return -1; // Unreachable
}

// Argument data recovered by argp
static struct {
	bool cleanup;
	LogLevel verbosity;
	const char* logFile;

	// Actual parameters for setup procedure
	setupParams params;
	setupGraphMLParams gmlParams;
} args;

// Divisors for GraphML bandwidths
static const float ShadowDivisor = 125.f;    // KiB/s
static const float ModelNetDivisor = 1000.f; // Kb/s

// Argument parsing hook for argp
static error_t parseArg(int key, char* arg, struct argp_state* state) {
	switch (key) {
	case 'd': args.cleanup = true; break;
	case 'f': args.params.srcFile = arg; break;

	case 'v': {
		args.verbosity = matchArg(arg, LogLevelStrings, state);
		break;
	}
	case 'l': args.logFile = arg; break;

	case 'p': args.params.nsPrefix = arg; break;

	case 'm': args.params.softMemCap = (size_t)(1024.0 * 1024.0 * strtod(arg, NULL)); break;

	case 'u': {
		const char* options[] = {"shadow", "modelnet", "KiB", "Kb", NULL};
		float divisors[] = {ShadowDivisor, ModelNetDivisor, ShadowDivisor, ModelNetDivisor};
		long index = matchArg(arg, options, state);
		args.gmlParams.bandwidthDivisor = divisors[index];
		break;
	}
	case 'w': args.gmlParams.weightKey = arg; break;
	case 'c': args.gmlParams.clientType = arg; break;
	case 's': args.gmlParams.twoPass = true; break;

	default: return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

int main(int argc, char** argv) {
	// Initialize libxml and ensure that the shared object is correct version
	LIBXML_TEST_VERSION

	// Command-line switch definitions
	struct argp_option generalOptions[] = {
			{ "destroy",      'd', NULL,                         OPTION_ARG_OPTIONAL, "If specified, any previous virtual network created by the program will be destroyed. If -f is not specified, the program terminates after deleting the network.", 0 },
			{ "file",         'f', "FILE",                       0,                   "The GraphML file containing the network topology. If omitted, the topology is read from stdin.", 0 },

			{ "verbosity",    'v', "{debug,info,warning,error}", 0,                   "Verbosity of log output.", 1 },
			{ "log-file",     'l', "FILE",                       0,                   "Log output to FILE instead of stdout.", 1 },

			{ "netns-prefix", 'p', "PREFIX",                     0,                   "Prefix string for network namespace files, which are visible to \"ip netns\" (default: \"sneac-\").", 2 },

			{ "mem",          'm', "MiB",                        0,                   "Approximate maximum memory use, specified in MiB. The program may use more than this amount if needed.", 3 },

			// File-specific options get priorities [50 - 99]

			{ NULL },
	};
	struct argp_option gmlOptions[] = {
			{ "units",        'u', "{shadow,modelnet,KiB,Kb}", 0,                   "Specifies the bandwidth units used in the input file. Shadow uses KiB/s (the default), whereas ModelNet uses Kbit/s." },
			{ "weight",       'w', "KEY",                      0,                   "Edge parameter to use for computing shortest paths for static routes. Must be a key used in the GraphML file (default: \"latency\")." },
			{ "client-node",  'c', "TYPE",                     0,                   "Type of client nodes. Nodes in the GraphML file whose \"type\" attribute matches this value will be clients. If omitted, all nodes are clients." },
			{ "scrambled",    's', NULL,                       OPTION_ARG_OPTIONAL, "This option must be specified if the GraphML file does not place all <node> tags before all <edge> tags. This option doubles the data retrieved from disk." },
			{ NULL },
	};
	struct argp_option defaultDoc[] = { { "\n These options provide program documentation:", 0, NULL, OPTION_DOC | OPTION_NO_USAGE }, { NULL } };

	struct argp gmlArgp = { gmlOptions, &parseArg };
	struct argp defaultDocArgp = { defaultDoc };

	struct argp_child children[] = {
			{ &gmlArgp, 0, "These options apply specifically to GraphML files:\n", 50 },
			{ &defaultDocArgp, 0, NULL, 100 },
			{ NULL },
	};
	struct argp argp = { generalOptions, &parseArg, NULL, "Sets up virtual networking infrastructure for a SNEAC core node.", children };

	// Defaults
	args.cleanup = false;
	args.verbosity = LogError;
	args.params.nsPrefix = "sneac-";
	args.params.softMemCap = 2L * 1024L * 1024L * 1024L;
	args.gmlParams.bandwidthDivisor = ShadowDivisor;
	args.gmlParams.weightKey = "latency";
	args.gmlParams.twoPass = false;

	// Parse arguments
	argp_parse(&argp, argc, argv, 0, NULL, NULL);
	int err = 0;

	// Set up logging
	if (args.logFile == NULL) {
		logSetStream(stderr);
	} else {
		if (!logSetFile(args.logFile)) {
			fprintf(stderr, "Could not open custom log file '%s' for writing.\n", args.logFile);
			err = 1;
		}
	}
	logSetThreshold(args.verbosity);

	if (err != 0) goto cleanup;

	lprintln(LogInfo, "Starting SNEAC: The Large-Scale Network Emulator");
	err = setupInit(&args.params);
	if (err != 0) goto cleanup;

	if (args.cleanup) {
		err = destroyNetwork();
	}

	if (err == 0 && (!args.cleanup || args.params.srcFile != NULL)) {
		lprintln(LogInfo, "Beginning network construction");
		err = setupGraphML(&args.gmlParams);
	}

	if (err != 0) {
		lprintf(LogError, "A fatal error occurred: code %d\n", err);
		lprintln(LogWarning, "Attempting to destroy partially-constructed network");
		destroyNetwork();
	} else {
		lprintln(LogInfo, "All operations completed successfully");
	}

	setupCleanup();
cleanup:
	xmlCleanupParser();
	logCleanup();

	return err;
}
