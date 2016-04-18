#include <argp.h>
#include <string.h>

#include "app.h"
#include "log.h"
#include "mem.h"
#include "version.h"

static error_t parseArg(int key, char* arg, struct argp_state* state) {
	return ARGP_ERR_UNKNOWN;
}

int main(int argc, char** argv) {
	appInit("NetMirage Edge", getVersion());

	// Command-line switch definitions
	struct argp_option generalOptions[] = {
			{ "verbosity",    'v', "{debug,info,warning,error}", 0, "Verbosity of log output (default: warning).", 0 },
			{ "log-file",     'l', "FILE",                       0, "Log output to FILE instead of stderr. Note: configuration errors will still be written to stderr.", 0 },

			{ "vsubnet",      'n', "CIDR", 0, "Specifies a subnet that belongs to the NetMirage virtual address space. Any traffic to this subnet will be routed through the core node.", 1 },

			{ "setup-file",   's', "FILE", 0, "Specifies a file that contains default configuration settings. This file is a key-value file (similar to an .ini file). Values should be added to the \"edge\" group. This group may contain any of the long names for command arguments. Note that any file paths specified in the setup file are relative to the current working directory (not the file location). Any arguments passed on the command line override the defaults and those set in the setup file. By default, the program attempts to read setup information from " DEFAULT_SETUP_FILE ".", 2 },

			{ NULL },
	};
	struct argp argp = { generalOptions, &appParseArg, "IFACE COREIP VSUBNET CLIENTS", "Configures a NetMirage edge node.\vIFACE specifies the network interface connected to the NetMirage core node, and COREIP is the IP address of the core node.\n\nVSUBNET is the subnet, in CIDR notation, assigned to this edge node by the core (obtained from the output of the netmirage-core command). This subnet is automatically considered part of the virtual range, so there is no need to explicitly specify it using a --vsubnet argument.\n\nCLIENTS is the number of IP addresses that should be allocated for applications. To allocate all available addresses in the subnet, specify \"max\" for CLIENTS." };

	int err = 0;

	err = appParseArgs(&parseArg, NULL, &argp, "edge", 's', 'l', 'v', argc, argv);
	if (err != 0) goto cleanup;

	lprintf(LogInfo, "Starting NetMirage Edge %s\n", getVersion());

	if (err != 0) {
		lprintf(LogError, "A fatal error occurred: code %d\n", err);
	} else {
		lprintln(LogInfo, "All operations completed successfully");
	}

cleanup:
	logCleanup();
	appCleanup();
	return err;
}
