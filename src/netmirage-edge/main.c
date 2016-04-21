#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include <argp.h>
#include <strings.h>

#include "app.h"
#include "ip.h"
#include "log.h"
#include "mem.h"
#include "net.h"
#include "version.h"

static struct {
	char* intfName;
	ip4Addr coreIp;
	ip4Subnet myNet;
	uint32_t clients;
	uint64_t applications;
	bool maxApplications;
	uint32_t priorityIncoming;
	uint32_t priorityOutgoing;
	uint32_t priorityOther;
	uint8_t outgoingTableId;

	// Flex buffer for subnets of other edge nodes
	union {
		ip4Subnet* edgeNets;
		void* netBuf;
	};
	size_t edgeNetCount;
	size_t netBufCap;

	bool remove;
} args;

static uint8_t seenNonOptions;
static bool netInitialized = false;
static netContext* net = NULL;
static int intfIdx;
static ip4Iter** clientIters = NULL;
static size_t currentClient;

// Arbitrary constants, but chosen to allow the user room to modify them
#define RULE_PRIORITY_DEFAULT_LOCAL 0
#define RULE_PRIORITY_INCOMING 5
#define RULE_PRIORITY_OUTGOING 10
#define RULE_PRIORITY_OTHER 15
#define OUTGOING_TABLE_ID 128

#define KEY_FILE_GROUP "edge"

static error_t parseArg(int key, char* arg, struct argp_state* state, unsigned int argNum) {
	switch (key) {
	case 'n': {
		ip4Subnet edgeNet;
		if (!ip4GetSubnet(arg, &edgeNet)) return 1;
		flexBufferGrow(&args.netBuf, args.edgeNetCount, &args.netBufCap, 1, sizeof(ip4Subnet));
		flexBufferAppend(args.netBuf, &args.edgeNetCount, &edgeNet, 1, sizeof(ip4Subnet));
		break;
	}
	case 'c': {
		sscanf(arg, "%" SCNu32, &args.clients);
		break;
	}

	case 'r':
		args.remove = true;
		break;

	case 'i':
		sscanf(arg, "%" SCNu32, &args.priorityIncoming);
		break;
	case 'o':
		sscanf(arg, "%" SCNu32, &args.priorityOutgoing);
		break;
	case 'h':
		sscanf(arg, "%" SCNu32, &args.priorityOther);
		break;
	case 't':
		sscanf(arg, "%" SCNu8, &args.outgoingTableId);
		break;

	case ARGP_KEY_ARG:
		switch (argNum) {
		case 0: // IFACE
			args.intfName = arg;
			break;
		case 1: // COREIP
			if (!ip4GetAddr(arg, &args.coreIp)) return 1;
			break;
		case 2: // VSUBNET
			if (!ip4GetSubnet(arg, &args.myNet)) return 1;
			break;
		case 3: // APPLICATIONS
			if (strcasecmp(arg, "max") == 0) {
				args.maxApplications = true;
			} else {
				args.maxApplications = false;
				sscanf(arg, "%" SCNu64, &args.applications);
			}
			break;
		default: return ARGP_ERR_UNKNOWN;
		}
		++seenNonOptions;
		break;
	default: return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static int initOperations(void) {
	lprintln(LogDebug, "Performing setup operations");

	uint64_t availAddrs = ip4SubnetSize(&args.myNet, true);
	if (args.maxApplications) {
		args.applications = availAddrs;
	}
	if (args.applications > availAddrs) {
		lprintf(LogError, "Requested %" SCNu64 " addresses, but only %" SCNu64 " are available in this subnet.\n", args.applications, availAddrs);
		return 1;
	}

	if (args.priorityIncoming >= args.priorityOutgoing || args.priorityIncoming >= args.priorityOther || args.priorityOutgoing >= args.priorityOther) {
		lprintf(LogError, "Invalid routing rule priorities. The priorities must satisfy (incoming < outgoing < other). The given priorities were (%" SCNu32 ", %" SCNu32 ", %" SCNu32 ")\n", args.priorityIncoming, args.priorityOutgoing, args.priorityOther);
		return 1;
	}

	int err = 0;

	err = netInit(""); // Prefix is irrelevant; we don't create namespaces
	if (err != 0) {
		lprintln(LogError, "Initializing the namespace system failed. You may need to run the program as root.");
		return err;
	}
	netInitialized = true;

	net = netOpenNamespace(NULL, false, false, &err);
	if (err != 0) return err;

	intfIdx = netGetInterfaceIndex(net, args.intfName, &err);
	if (err != 0) return err;

	if (args.clients < 1) args.clients = 1;

	return 0;
}

static int initAddressIterators(void) {
	ip4FragIter* fragIt = ip4FragmentSubnet(&args.myNet, args.clients);
	if (fragIt == NULL) {
		if (PASSES_LOG_THRESHOLD(LogError)) {
			char subnetStr[IP4_CIDR_BUFLEN];
			ip4SubnetToString(&args.myNet, subnetStr);
			lprintf(LogError, "The subnet %s cannot be divided into %" SCNu32 " parts. Ensure that the client count and subnet match the parameters used to set up the network in the core node.\n", subnetStr, args.clients);
		}
		return 1;
	}
	clientIters = eacalloc(args.clients, sizeof(ip4Iter*), 0);
	for (uint32_t i = 0; i < args.clients; ++i) {
		if (!ip4FragIterNext(fragIt)) {
			lprintln(LogError, "BUG: Failed to advance fragment iterator");
			ip4FreeFragIter(fragIt);
			return 1;
		}
		ip4Subnet clientNet;
		ip4FragIterSubnet(fragIt, &clientNet);
		clientIters[i] = ip4NewIter(&clientNet, false, NULL);
	}
	ip4FreeFragIter(fragIt);
	currentClient = 0;

	// We need to skip the first address if the range has reserved addresses.
	// The last one (also reserved) will never be visited because we already
	// constrain the number of applications to the number of non-reserved
	// addresses.
	if (ip4SubnetHasReserved(&args.myNet)) {
		ip4IterNext(clientIters[0]);
	}
	return 0;
}

static void freeAddressIterators(void) {
	if (clientIters != NULL) {
		for (uint32_t i = 0; i < args.clients; ++i) {
			if (clientIters[i] != NULL) ip4FreeIter(clientIters[i]);
		}
		free(clientIters);
	}
	clientIters = NULL;
}

static ip4Addr nextAppAddr(void) {
	if (!ip4IterNext(clientIters[currentClient])) {
		lprintf(LogError, "BUG: Ran out of address space for client %" SCNu32 "\n", currentClient);
		return 0;
	}
	ip4Addr res = ip4IterAddr(clientIters[currentClient]);
	currentClient = (currentClient + 1) % args.clients;
	return res;
}

static int applyConfiguration(void) {
	if (PASSES_LOG_THRESHOLD(LogInfo)) {
		char myNetStr[IP4_CIDR_BUFLEN];
		char coreIpStr[IP4_ADDR_BUFLEN];
		ip4SubnetToString(&args.myNet, myNetStr);
		ip4AddrToString(args.coreIp, coreIpStr);
		lprintf(LogInfo, "%s configuration for %" SCNu64 " applications in subnet %s routed to core node %s behind interface %s\n",
				(args.remove ? "Removing" : "Adding"),
				args.applications, myNetStr, coreIpStr, args.intfName);
	}

	int err = 0;

	freeAddressIterators();
	err = initAddressIterators();
	if (err != 0) return err;

	lprintf(LogDebug, "Configuring %" SCNu64 " application addresses\n", args.applications);
	if (args.remove) {
		err = netModifyInterfaceAddrIPv4(net, true, intfIdx, nextAppAddr(), args.myNet.prefixLen, 0, 0, true);
	} else {
		for (uint64_t i = 0; i < args.applications; ++i) {
			ip4Addr appIp = nextAppAddr();

			if (PASSES_LOG_THRESHOLD(LogDebug)) {
				char appIpStr[IP4_ADDR_BUFLEN];
				ip4AddrToString(appIp, appIpStr);
				lprintf(LogDebug, "%s application address %s\n", (args.remove ? "Removing" : "Adding"), appIpStr);
			}

			err = netModifyInterfaceAddrIPv4(net, false, intfIdx, appIp, args.myNet.prefixLen, 0, 0, true);
			if (err != 0) break;
		}
	}
	if (err != 0 && !args.remove) return err;

	// If we just added addreses, a default route gets created in the main table
	// directing packets out the interface. We don't want this rule.
	if (!args.remove) {
		lprintln(LogDebug, "Removing default routes for subnet");
		err = netModifyRoute(net, true, netGetTableId(TableMain), ScopeLink, CreatorKernel, ip4SubnetStart(&args.myNet), args.myNet.prefixLen, 0, intfIdx, true);
		if (err != 0) return err;
	}

	lprintln(LogDebug, "Reconfiguring routing");

	// First we configure a custom table with rules to send packets to the core
	err = netModifyRoute(net, args.remove, args.outgoingTableId, ScopeGlobal, CreatorAdmin, args.myNet.addr, args.myNet.prefixLen, args.coreIp, intfIdx, true);
	if (err != 0 && !args.remove) return err;

	for (size_t i = 0; i < args.edgeNetCount; ++i) {
		err = netModifyRoute(net, args.remove, args.outgoingTableId, ScopeGlobal, CreatorAdmin, args.edgeNets[i].addr, args.edgeNets[i].prefixLen, args.coreIp, intfIdx, true);
		if (err != 0 && !args.remove) return err;
	}

	// We need to decrease the default priority of the local routing table. We
	// need to maintain the local routes in the local table in order for
	// applications to successfully bind to addresses (Linux does not consult
	// the rules table for this purpose). At the same time, we also need to have
	// a higher-priority rule that routes packets externally if they originate
	// from the local machine. See GOTCHAS for more details. In order to avoid
	// interruption of service, we must always add a rule for the table before
	// removing the existing one.
	uint32_t localToPriority = (args.remove ? RULE_PRIORITY_DEFAULT_LOCAL : args.priorityOther);
	uint32_t localFromPriority = (args.remove ? args.priorityOther : RULE_PRIORITY_DEFAULT_LOCAL);
	bool toRuleExists = false;
	err = netRuleExists(net, localToPriority, &toRuleExists);
	if (err != 0) return err;
	if (!args.remove && toRuleExists) {
		lprintf(LogError, "A routing rule with priority %" SCNu32 " (other priority) already exists\n", localToPriority);
		return 1;
	}
	if (!toRuleExists) {
		err = netModifyRule(net, false, NULL, NULL, netGetTableId(TableLocal), CreatorAdmin, localToPriority, true);
		if (err != 0) return err; // Abort on removal (could lose network!)
	}
	// Failure to remove the default rule is okay, since it might have already
	// been moved or deleted.
	netModifyRule(net, true, NULL, NULL, netGetTableId(TableLocal), CreatorAny, localFromPriority, true);

	// Finally, add/remove the custom rules for incoming and outgoing packets
	if (!args.remove) {
		// Don't add duplicate rules
		bool ruleExists = false;
		err = netRuleExists(net, args.priorityIncoming, &ruleExists);
		if (err != 0) return err;
		if (ruleExists) {
			lprintf(LogError, "A routing rule with priority %" SCNu32 " (incoming priority) already exists\n", args.priorityIncoming);
			return 1;
		}
		err = netRuleExists(net, args.priorityOutgoing, &ruleExists);
		if (err != 0) return err;
		if (ruleExists) {
			lprintf(LogError, "A routing rule with priority %" SCNu32 " (outgoing priority) already exists\n", args.priorityOutgoing);
			return 1;
		}
	}
	err = netModifyRule(net, args.remove, &args.myNet, args.intfName, netGetTableId(TableLocal), CreatorAdmin, args.priorityIncoming, true);
	if (err != 0 && !args.remove) return err;
	err = netModifyRule(net, args.remove, &args.myNet, NULL, args.outgoingTableId, CreatorAdmin, args.priorityOutgoing, true);
	if (err != 0 && !args.remove) return err;

	return 0;
}

static void cleanupOperations(void) {
	freeAddressIterators();
	if (net != NULL) netCloseNamespace(net, false);
	if (netInitialized) netCleanup();
}

int main(int argc, char** argv) {
	appInit("NetMirage Edge", getVersion());

	// Command-line switch definitions
	struct argp_option generalOptions[] = {
			{ "other-edge", 'n', "CIDR", 0, "Specifies a subnet that belongs to the NetMirage virtual address space. Any traffic to this subnet will be routed through the core node.", 0 },
			{ "clients",    'c', "COUNT", 0, "Specifies the number of client nodes in the core topology associated with this edge node. If this is NOT given, then IP addresses will be allocated sequentially from the subnet. If it IS given, then addresses will be sampled from each client node subnet in a round-robin pattern.", 0 },

			{ "remove",     'r', NULL, OPTION_ARG_OPTIONAL, "If specified, the program will attempt to remove a previously created configuration. No new routes will be configured. Note that the program must be called with the exact same network configuration that was used to create the previous setup.", 1 },

			{ "verbosity",  'v', "{debug,info,warning,error}", 0, "Verbosity of log output (default: warning).", 2 },
			{ "log-file",   'l', "FILE",                       0, "Log output to FILE instead of stderr. Note: configuration errors may still be written to stderr.", 2 },

			{ "rule-in",    'i', "PRIORITY", 0, "Optional routing rule priority for incoming packets.", 3 },
			{ "rule-out",   'o', "PRIORITY", 0, "Optional routing rule priority for outgoing packets.", 3 },
			{ "rule-other", 'h', "PRIORITY", 0, "Optional routing rule priority for default local routing table lookups.", 3 },
			{ "table-id",   't', "ID", 0, "Optional identifier for the routing table used by outgoing packets.", 3 },

			{ "setup-file", 's', "FILE", 0, "Specifies a file that contains default configuration settings. This file is a key-value file (similar to an .ini file). Values should be added to the \"edge\" group. This group may contain any of the long names for command arguments. Note that any file paths specified in the setup file are relative to the current working directory (not the file location). Any arguments passed on the command line override the defaults and those set in the setup file. The non-option arguments can be specified using the \"iface\", \"core-ip\", \"vsubnet\", and \"clients\" keys. By default, the program attempts to read setup information from " DEFAULT_SETUP_FILE ".", 4 },

			{ NULL },
	};
	struct argp argp = { generalOptions, &appParseArg, "IFACE COREIP VSUBNET APPLICATIONS", "Configures a NetMirage edge node.\vIFACE specifies the network interface connected to the NetMirage core node, and COREIP is the IP address of the core node.\n\nVSUBNET is the subnet, in CIDR notation, assigned to this edge node by the core (obtained from the output of the netmirage-core command). This subnet is automatically considered part of the virtual range, so there is no need to explicitly specify it using a --vsubnet argument.\n\nAPPLICATIONS is the number of IP addresses that should be allocated for applications. To allocate all available addresses in the subnet, specify \"max\" for APPLICATIONS." };
	const char* nonOptions[] = { "iface", "core-ip", "vsubnet", "applications", NULL };

	// Defaults
	args.remove = false;
	args.priorityIncoming = RULE_PRIORITY_INCOMING;
	args.priorityOutgoing = RULE_PRIORITY_OUTGOING;
	args.priorityOther = RULE_PRIORITY_OTHER;
	args.outgoingTableId = OUTGOING_TABLE_ID;
	flexBufferInit(&args.netBuf, &args.edgeNetCount, &args.netBufCap);

	int err = 0;

	err = appParseArgs(&parseArg, NULL, &argp, KEY_FILE_GROUP, nonOptions, 's', 'l', 'v', argc, argv);
	if (err != 0) goto cleanup;

	if (seenNonOptions != 4) {
		argp_help(&argp, stderr, ARGP_HELP_STD_USAGE, argv[0]);
		goto cleanup;
	}

	lprintf(LogInfo, "Starting NetMirage Edge %s\n", getVersion());

	err = initOperations();
	if (err != 0) goto cleanup;

	err = applyConfiguration();
	if (err != 0) {
		lprintf(LogError, "A fatal error occurred: code %d\n", err);
		if (!args.remove) {
			lprintln(LogWarning, "Attempting to undo partially configured setup");
			args.remove = true;
			applyConfiguration();
		}
	} else {
		lprintln(LogInfo, "All operations completed successfully");
	}

cleanup:
	cleanupOperations();

	flexBufferFree(&args.netBuf, &args.edgeNetCount, &args.netBufCap);

	appCleanup();
	return err;
}
