#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <argp.h>
#include <glib.h>
#include <libxml/parser.h>

#include "app.h"
#include "ip.h"
#include "log.h"
#include "mem.h"
#include "setup.h"
#include "version.h"

// TODO: normalize naming conventions for "client", "root", etc.
// TODO: follow POSIX reserved identifier conventions
// TODO: increase precision of _GNU_SOURCE
// TODO: minimize includes
// TODO: more specific error codes than 1
// TODO: normalize (return result, out err) vs (return err, out result)
// TODO: normalize res and err
// TODO: use inttypes.h for scanf

// Argument data recovered by argp
static struct {
	size_t edgeNodeCap; // Buffer length is stored in the setupParams
	bool loadedEdgesFromSetup;

	// Actual parameters for setup procedure
	setupParams params;
	setupGraphMLParams gmlParams;
} args;

// Divisors for GraphML bandwidths
static const float ShadowDivisor = 125.f;    // KiB/s
static const float ModelNetDivisor = 1000.f; // Kb/s

#define DEFAULT_CLIENTS_SUBNET "10.0.0.0/8"

#define DEFAULT_OVS_DIR    "/tmp/netmirage"

// Adds an edge node based on strings, which may be NULL
static bool addEdgeNode(const char* ipStr, const char* intfStr, const char* macStr, const char* vsubnetStr) {
	edgeNodeParams params;
	if (!ip4GetAddr(ipStr, &params.ip)) return false;

	if (intfStr != NULL && *intfStr == '\0') return false;
	if (intfStr == NULL) {
		params.intf = NULL;
	} else {
		params.intf = eamalloc(strlen(intfStr), 1, 1); // Extra byte for NUL
		strcpy(params.intf, intfStr);
	}

	params.macSpecified = (macStr != NULL);
	if (params.macSpecified) {
		if (!macGetAddr(macStr, &params.mac)) return false;
	}

	params.vsubnetSpecified = (vsubnetStr != NULL);
	if (params.vsubnetSpecified) {
		if (!ip4GetSubnet(vsubnetStr, &params.vsubnet)) return false;
	}
	flexBufferGrow((void**)&args.params.edgeNodes, args.params.edgeNodeCount, &args.edgeNodeCap, 1, sizeof(edgeNodeParams));
	flexBufferAppend(args.params.edgeNodes, &args.params.edgeNodeCount, &params, 1, sizeof(edgeNodeParams));
	return true;
}

static error_t parseArg(int key, char* arg, struct argp_state* state) {
	switch (key) {
	case 'd': args.params.destroyFirst = true; break;
	case 'f': args.params.srcFile = arg; break;
	case 'r': args.params.ovsDir = arg; break;
	case 'a': args.params.ovsSchema = arg; break;

	case 'i': {
		args.params.edgeNodeDefaults.intfSpecified = true;
		args.params.edgeNodeDefaults.intf = arg;
		break;
	}
	case 'n': {
		if (!ip4GetSubnet(arg, &args.params.edgeNodeDefaults.globalVSubnet)) {
			fprintf(stderr, "Invalid global virtual client subnet specified: '%s'\n", arg);
			return EINVAL;
		}
		break;
	}
	case 'e': {
		// We ignore edge configuration in the setup file's [emulator] group
		if (state == NULL) break;

		// If we just found an explicit edge node for the first time after
		// loading configuration from the setup file, erase the old edges
		if (args.loadedEdgesFromSetup) {
			args.params.edgeNodeCount = 0;
			args.loadedEdgesFromSetup = false;
		}

		char* ip = arg;
		char* intf = NULL;
		char* mac = NULL;
		char* vsubnet = NULL;

		char* optionSep = arg;
		while (true) {
			optionSep = strchr(optionSep, ',');
			if (optionSep == NULL) break;

			*optionSep = '\0';
			++optionSep;

			// If the '=' was found in a subsequent option, that's ok because
			// none of the valid options include ',' in their name; we will err
			char* keyValSep = strchr(optionSep, '=');
			if (keyValSep == NULL) {
				fprintf(stderr, "Invalid format for edge node argument '%s'\n", arg);
				return EINVAL;
			}
			size_t cmpLen = (size_t)(keyValSep - optionSep);
			if (cmpLen == 0) {
				fprintf(stderr, "Empty option name in edge node argument '%s'\n", arg);
				return EINVAL;
			}

			if (strncmp(optionSep, "iface", cmpLen) == 0) {
				intf = keyValSep+1;
			} else if (strncmp(optionSep, "mac", cmpLen) == 0) {
				mac = keyValSep+1;
			} else if (strncmp(optionSep, "vsubnet", cmpLen) == 0) {
				vsubnet = keyValSep+1;
			} else {
				*keyValSep = '\0';
				fprintf(stderr, "Unknown option '%s' in edge node argument '%s'\n", optionSep, arg);
				return EINVAL;
			}
		}

		if (!addEdgeNode(ip, intf, mac, vsubnet)) {
			fprintf(stderr, "Edge node argument '%s' was invalid\n", arg);
			return EINVAL;
		}
		break;
	}

	case 'p': args.params.nsPrefix = arg; break;

	case 'm': args.params.softMemCap = (size_t)(1024.0 * 1024.0 * strtod(arg, NULL)); break;

	case 'u': {
		const char* options[] = {"shadow", "modelnet", "KiB", "Kb", NULL};
		float divisors[] = {ShadowDivisor, ModelNetDivisor, ShadowDivisor, ModelNetDivisor};
		long index = matchArg(arg, options, state);
		if (index < 0) {
			fprintf(stderr, "Unknown bandwidth units '%s'\n", arg);
			return EINVAL;
		}
		args.gmlParams.bandwidthDivisor = divisors[index];
		break;
	}
	case 'w': args.gmlParams.weightKey = arg; break;
	case 'c': args.gmlParams.clientType = arg; break;
	case 't': args.gmlParams.twoPass = true; break;

	default: return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

// Configures edge nodes using information in the setup file
static bool readSetupEdges(GKeyFile* file) {
	gchar** groups = g_key_file_get_groups(file, NULL);
	for (gchar** g = groups; *g != NULL; ++g) {
		gchar* group = *g;
		// We allow "node" for backwards compatibility, but do not advertise it
		if (strncmp(group, "edge", 4) == 0 || strncmp(group, "node", 4) == 0) {
			char* ip = g_key_file_get_string(file, group, "ip", NULL);
			char* intf = g_key_file_get_string(file, group, "iface", NULL);
			char* mac = g_key_file_get_string(file, group, "mac", NULL);
			char* vsubnet = g_key_file_get_string(file, group, "vsubnet", NULL);
			bool added = addEdgeNode(ip, intf, mac, vsubnet);

			g_free(ip);
			g_free(intf);
			g_free(mac);
			g_free(vsubnet);

			if (!added) {
				fprintf(stderr, "In setup file: invalid configuration for edge node '%s'\n", group);
				return false;
			}
			args.loadedEdgesFromSetup = true;
		}
	}
	g_strfreev(groups);
	return true;
}

int main(int argc, char** argv) {
	appInit("NetMirage Core", getVersion());

	// Launch worker processes so that we can drop our privileges as quickly as
	// possible (note that we have not handled any user input at this point)
	if (setupInit() != 0) {
		lprintln(LogError, "Failed to start worker processes. Elevation may be required.");
		logCleanup();
		return 1;
	}

	// Initialize libxml and ensure that the shared object is correct version
	LIBXML_TEST_VERSION

	// Command-line switch definitions
	struct argp_option generalOptions[] = {
			{ "destroy",      'd', NULL,   OPTION_ARG_OPTIONAL, "If specified, any previous virtual network created by the program will be destroyed. If -f is not specified, the program terminates after deleting the network.", 0 },
			{ "file",         'f', "FILE", 0,                   "The GraphML file containing the network topology. If omitted, the topology is read from stdin.", 0 },
			{ "setup-file",   's', "FILE", 0,                   "The file containing setup information about edge nodes and emulator interfaces. This file is a key-value file (similar to an .ini file). Every group whose name begins with \"edge\" or \"node\" denotes the configuration for an edge node. The keys and values permitted in an edge node group are the same as those in an --edge-node argument. There may also be an \"emulator\" group. This group may contain any of the long names for command arguments. Note that any file paths specified in the setup file are relative to the current working directory (not the file location). Any arguments passed on the command line override the defaults and those set in the setup file. By default, the program attempts to read setup information from " DEFAULT_SETUP_FILE ".", 0 },

			{ "iface",        'i', "DEVNAME",                                     0, "Default interface connected to the edge nodes. Individual edge nodes can override this setting in the setup file or as part of the --edge-nodes argument.", 1 },
			{ "vsubnet",      'n', "CIDR",                                        0, "The global subnet to which all virtual clients belong. By default, each edge node is given a fragment of this global subnet in which to spawn clients. Subnets for edge nodes can also be manually assigned rather than drawing them from this larger space. The default value is " DEFAULT_CLIENTS_SUBNET ".", 1 },
			{ "edge-node",    'e', "IP[,iface=DEVNAME][,mac=MAC][,vsubnet=CIDR]", 0, "Adds an edge node to the configuration. The presence of an --edge-node argument causes all edge node configuration in the setup file to be ignored. The node's IPv4 address must be specified. If the optional \"iface\" portion is specified, it lists the interface connected to the edge node (if omitted, --iface is used). \"mac\" specifies the MAC address of the node (if omitted, it is found using ARP). \"vsubnet\" specifies the subnet, in CIDR notation, for clients in the edge node (if omitted, a subnet is assigned automatically from the --vsubnet range).", 1 },

			{ "verbosity",    'v', "{debug,info,warning,error}", 0, "Verbosity of log output (default: warning).", 2 },
			{ "log-file",     'l', "FILE",                       0, "Log output to FILE instead of stderr. Note: configuration errors will still be written to stderr.", 2 },

			{ "netns-prefix", 'p', "PREFIX", 0, "Prefix string for network namespace files, which are visible to \"ip netns\" (default: \"nm-\").", 3 },
			{ "ovs-dir",      'r', "DIR",    0, "Directory for storing temporary Open vSwitch files, such as the flow database and management sockets (default: \"" DEFAULT_OVS_DIR "\").", 3 },
			{ "ovs-schema",   'a', "FILE",   0, "Path to the OVSDB schema definition for Open vSwitch (default: \"/usr/share/openvswitch/vswitch.ovsschema\").", 3 },

			{ "mem",          'm', "MiB",    0, "Approximate maximum memory use, specified in MiB. The program may use more than this amount if needed.", 4 },

			// File-specific options get priorities [50 - 99]

			{ NULL },
	};
	struct argp_option gmlOptions[] = {
			{ "units",        'u', "{shadow,modelnet,KiB,Kb}", 0,                   "Specifies the bandwidth units used in the input file. Shadow uses KiB/s (the default), whereas ModelNet uses Kbit/s." },
			{ "weight",       'w', "KEY",                      0,                   "Edge parameter to use for computing shortest paths for static routes. Must be a key used in the GraphML file (default: \"latency\")." },
			{ "client-node",  'c', "TYPE",                     0,                   "Type of client nodes. Nodes in the GraphML file whose \"type\" attribute matches this value will be clients. If omitted, all nodes are clients." },
			{ "two-pass",     't', NULL,                       OPTION_ARG_OPTIONAL, "This option must be specified if the GraphML file does not place all <node> tags before all <edge> tags. This option doubles the data retrieved from disk." },
			{ NULL },
	};
	struct argp_option defaultDoc[] = { { "\n These options provide program documentation:", 0, NULL, OPTION_DOC | OPTION_NO_USAGE }, { NULL } };

	struct argp gmlArgp = { gmlOptions, &appParseArg };
	struct argp defaultDocArgp = { defaultDoc };

	struct argp_child children[] = {
			{ &gmlArgp, 0, "These options apply specifically to GraphML files:\n", 50 },
			{ &defaultDocArgp, 0, NULL, 100 },
			{ NULL },
	};
	struct argp argp = { generalOptions, &appParseArg, NULL, "Sets up virtual networking infrastructure for a NetMirage core node.", children };

	// Default arguments
	args.params.nsPrefix = "nm-";
	args.params.ovsDir = DEFAULT_OVS_DIR;
	args.params.softMemCap = 2L * 1024L * 1024L * 1024L;
	args.params.destroyFirst = false;
	ip4GetSubnet(DEFAULT_CLIENTS_SUBNET, &args.params.edgeNodeDefaults.globalVSubnet);
	args.gmlParams.bandwidthDivisor = ShadowDivisor;
	args.gmlParams.weightKey = "latency";
	args.gmlParams.twoPass = false;

	int err = 0;

	err = appParseArgs(&parseArg, &readSetupEdges, &argp, "emulator", 's', 'l', 'v', argc, argv);
	if (err != 0) goto cleanup;

	lprintf(LogInfo, "Starting NetMirage Core %s\n", getVersion());

	err = setupConfigure(&args.params);
	if (err != 0) goto cleanup;

	if (err == 0 && (!args.params.destroyFirst || args.params.srcFile != NULL)) {
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

cleanup:
	setupCleanup();
	if (args.params.edgeNodes != NULL) {
		for (size_t i = 0; i < args.params.edgeNodeCount; ++i) {
			edgeNodeParams* edge = &args.params.edgeNodes[i];
			if (edge->intf != NULL) free(edge->intf);
		}
	}
	flexBufferFree((void**)&args.params.edgeNodes, &args.params.edgeNodeCount, &args.edgeNodeCap);
	xmlCleanupParser();
	appCleanup();

	return err;
}
