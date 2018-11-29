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
#define _GNU_SOURCE // Needed for Linux-specific functionality

#include "ovs.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ip.h"
#include "log.h"
#include "mem.h"
#include "net.h"

struct ovsContext {
	netContext* net;
	const char* directory;
	char* dbSocketConnArg;
	char* actionBuffer; // Flexible buffer
	const char* compatArgs;
	size_t actionBufferCap;
};

static int switchContext(ovsContext* ctx) {
	return netSwitchNamespace(ctx->net);
}

#define OVS_DEFAULT_SCHEMA_PATH "/usr/share/openvswitch/vswitch.ovsschema"
#define OVS_CMD_MAX_ARGS 20
#define OVSDB_CTL_FILE "ovsdb-server.ctl"
#define OVS_CTL_FILE "ovs-vswitchd.ctl"
#define LKM_LIST_FILE "/proc/modules"
#define LKM_OVS_NAME "openvswitch"

// Forks and executes an OVS command with the given arguments. The first
// argument is automatically set to be the command. Must call switchContext
// before this function. If output is non-NULL, output, outputLen, and
// outputCap are a flexBuffer that will contain the merger of stdout and stderr
// for the subprocess. If dir is not NULL, the process runs in the given working
// directory. Returns 0 on success or an error code otherwise.
static int ovsCommandVArg(char** output, size_t* outputLen, size_t* outputCap, const char* dir, va_list args) {
	char* argv[OVS_CMD_MAX_ARGS+2];
	char* command = va_arg(args, char*); // Safe cast (see POSIX standard)

	lprintHead(LogDebug);
	argv[0] = command;
	lprintDirectf(LogDebug, "Running Open vSwitch command: %s", command);
	size_t argCount = 1;
	for (size_t i = 1; i < OVS_CMD_MAX_ARGS+1; ++i) {
		char* nextArg = va_arg(args, char*); // Also a safe cast
		if (nextArg == NULL) break;

		// We ignore empty arguments (simplifies some other logic)
		if (nextArg[0] != '\0') {
			argv[argCount++] = nextArg;
			lprintDirectf(LogDebug, " \"%s\"", nextArg);
		}
	}
	va_end(args);
	argv[argCount] = NULL;
	lprintDirectf(LogDebug, "\n");
	lprintDirectFinish(LogDebug);

	int pipefd[2];
	errno = 0;
	if (pipe(pipefd) != 0) {
		lprintf(LogError, "Failed to create pipe for Open vSwitch command: %s\n", strerror(errno));
		return errno;
	}

	errno = 0;
	pid_t pid = fork();
	if (pid == -1) {
		lprintf(LogError, "Failed to fork to execute Open vSwitch command: %s\n", strerror(errno));
		return errno;
	}
	if (pid == 0) { // Child
		if (dir != NULL) {
			errno = 0;
			if (chdir(dir) != 0) exit(errno);
		}

		close(pipefd[0]);
		close(STDIN_FILENO);
		errno = 0;
		if (dup2(pipefd[1], STDOUT_FILENO) == -1) exit(errno);
		errno = 0;
		if (dup2(pipefd[1], STDERR_FILENO) == -1) exit(errno);

		close(pipefd[1]);

		char* envp[] = { NULL, NULL };
		if (dir != NULL) {
			newSprintf(&envp[0], "OVS_RUNDIR=%s", dir);
		}

		errno = 0;
		execvpe(command, (char**)argv, envp);
		exit(errno);
	}
	close(pipefd[1]);

	int readErr = 0;
	if (output != NULL) {
		char readBuf[4 * 1024];
		while (true) {
			errno = 0;
			ssize_t readLen = read(pipefd[0], readBuf, sizeof(readBuf));
			if (readLen < 1) break;
			flexBufferGrow((void**)output, *outputLen, outputCap, (size_t)readLen, 1);
			flexBufferAppend(*output, outputLen, readBuf, (size_t)readLen, 1);
		}
		readErr = errno;
	}
	// Always wait so that we don't end up with a zombie process

	int status;
	waitpid(pid, &status, 0);
	close(pipefd[0]);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		lprintf(LogError, "Open vSwitch command %s reported a failure. Exit code: %d\n", command, WEXITSTATUS(status));
		return WEXITSTATUS(status) == 0 ? 1 : WEXITSTATUS(status);
	} else if (readErr != 0) {
		lprintf(LogError, "Error while reading output from Open vSwitch command %s: %s\n", command, strerror(readErr));
		return readErr;
	}
	return 0;
}

static int ovsCommand(const char* dir, ...) {
	va_list args;
	va_start(args, dir);
	return ovsCommandVArg(NULL, NULL, NULL, dir, args);
}

static int ovsCommandOutput(char** output, size_t* outputLen, size_t* outputCap, const char* dir, ...) {
	va_list args;
	va_start(args, dir);
	return ovsCommandVArg(output, outputLen, outputCap, dir, args);
}

static const char* ovsToolVersion(char** output, size_t* outputLen, size_t* outputCap, const char* tool) {
	*outputLen = 0;
	int res = ovsCommandOutput(output, outputLen, outputCap, NULL, tool, "--version", NULL);
	if (res != 0 || *outputLen < 1) return NULL;

	// Take the last token (w.r.t. spaces) on the first line
	char* p = strchr(*output, '\n');
	if (p == NULL) return NULL;
	*p = '\0';
	while (p >= *output && *p != ' ') --p;
	return p+1;
}

char* ovsVersion(bool* validVersion, unsigned int* major, unsigned int* minor) {
	*validVersion = false;

	char* output;
	size_t outputLen, outputCap;
	flexBufferInit((void**)&output, &outputLen, &outputCap);

	const char* neededCommands[] = {
		"ovsdb-tool",
		"ovsdb-server",
		"ovs-vsctl",
		"ovs-vswitchd",
		"ovs-appctl",
		"ovs-ofctl",
		NULL,
	};

	char* version = NULL;
	for (const char** cmd = neededCommands; *cmd != NULL; ++cmd) {
		const char* cmdVer = ovsToolVersion(&output, &outputLen, &outputCap, *cmd);
		if (cmdVer == NULL) return NULL;
		if (version == NULL) {
			version = strdup(cmdVer);
		} else if (strcmp(cmdVer, version) != 0) {
			return NULL;
		}
	}

	flexBufferFree((void**)&output, &outputLen, &outputCap);

	*validVersion = (sscanf(version, "%u.%u.", major, minor) == 2);

	return version;
}

// Determines the compatability arguments required for the current version
static const char* ovsCompatArgs(void) {
	const char* compatArgs = "";

	bool validVer;
	unsigned int majorVer, minorVer;
	char* ver = ovsVersion(&validVer, &majorVer, &minorVer);
	if (ver != NULL) {
		if (validVer && (majorVer > 2 || (majorVer == 2 && minorVer > 4))) {
			lprintf(LogDebug, "Using an OVS version ('%s') with the \"logging bug\". Using workaround.\n", ver);
			// If this is not provided to most commands, the 2.5.0 branch
			// crashes with an assertion failure.
			// TODO: This is probably an OVS bug. Report it!
			compatArgs = "--log-file=/dev/null";
		}
		free(ver);
	}

	return compatArgs;
}

static int ovsModuleLoad(void) {
	// Open vSwitch includes a kernel module component. If it is not loaded,
	// then subsequent OVS commands may fail with inscrutable errors. We check
	// to make sure that it is loaded.
	FILE* modFile = fopen(LKM_LIST_FILE, "re");
	if (modFile == NULL) {
		lprintln(LogWarning, "Failed to open Linux kernel module list from " LKM_LIST_FILE ". If setting up the virtual switch fails, ensure that the '" LKM_OVS_NAME "' module is loaded.");
		return 0;
	}
	char* line = NULL;
	size_t len = 0;
	bool modLoaded = false;
	while (getline(&line, &len, modFile) != -1) {
		char* sep = strchr(line, ' ');
		if (sep != NULL) *sep = '\0';
		if (strcmp(line, LKM_OVS_NAME) == 0) {
			modLoaded = true;
			break;
		}
	}
	if (line != NULL) free(line);
	fclose(modFile);
	if (!modLoaded) {
		lprintln(LogWarning, "The Open vSwitch kernel module ('" LKM_OVS_NAME "') does not appear to be loaded. Attempting to load the module.");
		errno = 0;
		pid_t probePid = fork();
		if (probePid == -1) {
			lprintf(LogError, "Could not fork to load kernel module: %s\n", strerror(errno));
			return errno;
		}
		if (probePid == 0) {
			fclose(stdin);
			fclose(stdout);
			fclose(stderr);
			errno = 0;
			execlp("modprobe", "modprobe", LKM_OVS_NAME, NULL);
			exit(errno);
		}
		int probeStatus;
		waitpid(probePid, &probeStatus, 0);
		if (!WIFEXITED(probeStatus) || WEXITSTATUS(probeStatus) != 0) {
			lprintf(LogWarning, "The Open vSwitch kernel module could not be loaded (modprobe exit code %d). Unless this distribution uses a different name for the module, setting up the virtual switch will fail. The module will need to be loaded manually\n", WEXITSTATUS(probeStatus));
			return 0;
		}
		lprintln(LogInfo, "The Open vSwitch kernel module was loaded successfully.");
	}
	return 0;
}

ovsContext* ovsStart(netContext* net, const char* directory, const char* ovsSchema, bool existing, int* err) {
	lprintf(LogDebug, "%s Open vSwitch instance in namespace %p with state directory %s\n", (existing ? "Connecting to" : "Starting an"), net, directory);

	int localErr;
	if (err == NULL) err = &localErr;

	ovsModuleLoad();

	if (!existing) {
		errno = 0;
		if (mkdir(directory, 0700) != 0 && errno != EEXIST) {
			lprintf(LogError, "Could not create the Open vSwitch state directory '%s': %s\n", directory, strerror(errno));
			*err = errno;
			return NULL;
		}
	}

	ovsContext* ctx = NULL;

	const char* compatArgs = ovsCompatArgs();

	netSwitchNamespace(net);

	// Construct file paths / arguments
	char *dbFile;
	char *ovsdbLogArg, *ovsdbPidArg, *ovsdbSocket, *ovsdbSocketArg, *ovsdbSocketConnArg, *ovsdbControlArg;
	char *ovsLogArg, *ovsPidArg, *ovsControlArg;
	newSprintf(&dbFile, "%s/ovs.db", directory);
	newSprintf(&ovsdbLogArg, "--log-file=%s/ovsdb-server.log", directory);
	newSprintf(&ovsdbPidArg, "--pidfile=%s/ovsdb-server.pid", directory);
	newSprintf(&ovsdbSocket, "%s/ovsdb-server.sock", directory);
	newSprintf(&ovsdbSocketArg, "--remote=punix:%s", ovsdbSocket);
	newSprintf(&ovsdbSocketConnArg, "--db=unix:%s", ovsdbSocket);
	newSprintf(&ovsdbControlArg, "--unixctl=%s/" OVSDB_CTL_FILE, directory);
	newSprintf(&ovsLogArg, "--log-file=%s/ovs-vswitchd.log", directory);
	newSprintf(&ovsPidArg, "--pidfile=%s/ovs-vswitchd.pid", directory);
	newSprintf(&ovsControlArg, "--unixctl=%s/" OVS_CTL_FILE, directory);

	if (!existing) {
		// First, set up the OVSDB daemon. This daemon provides access to the
		// database file that is used to store switch data and manage the other
		// components.

		errno = 0;
		if (unlink(dbFile) != 0 && errno != ENOENT) {
			lprintf(LogError, "Could not delete Open vSwitch database file '%s': %s\n", dbFile, strerror(errno));
			goto abort;
		}

		if (ovsSchema == NULL) ovsSchema = OVS_DEFAULT_SCHEMA_PATH;
		*err = ovsCommand(directory, "ovsdb-tool", "create", dbFile, ovsSchema, NULL);
		if (*err != 0) goto abort;

		*err = ovsCommand(directory, "ovsdb-server", dbFile, "-vconsole:off", "-vsyslog:err", "-vfile:info", "--no-chdir", "--detach", "--monitor", ovsdbLogArg, ovsdbPidArg, ovsdbSocketArg, ovsdbControlArg, NULL);
		if (*err != 0) goto abort;

		*err = ovsCommand(directory, "ovs-vsctl", compatArgs, ovsdbSocketConnArg, "--no-wait", "init", NULL);
		if (*err != 0) goto abort;

		// Next, set up the vswitchd daemon. This daemon manages the virtual
		// switches and their flows.

		*err = ovsCommand(directory, "ovs-vswitchd", &ovsdbSocketConnArg[5], "-vconsole:off", "-vsyslog:err", "-vfile:info", "--mlockall", "--no-chdir", "--detach", "--monitor", ovsLogArg, ovsPidArg, ovsControlArg, NULL);
		if (*err != 0) goto abort;
	}

	ctx = emalloc(sizeof(ovsContext));
	ctx->net = net;
	ctx->directory = directory;
	ctx->dbSocketConnArg = ovsdbSocketConnArg;
	ctx->compatArgs = compatArgs;
	flexBufferInit((void**)&ctx->actionBuffer, NULL, &ctx->actionBufferCap);
	lprintf(LogDebug, "Created Open vSwitch context %p\n", ctx);
	goto cleanup;
abort:
	// We don't free this unless there was an error (it is used in ctx)
	free(ovsdbSocketConnArg);
cleanup:
	free(dbFile);
	free(ovsdbLogArg); free(ovsdbPidArg); free(ovsdbSocket); free(ovsdbSocketArg); free(ovsdbControlArg);
	free(ovsLogArg); free(ovsPidArg); free(ovsControlArg);
	return ctx;
}

int ovsFree(ovsContext* ctx) {
	int err = switchContext(ctx);
	if (err != 0) return err;

	flexBufferFree((void**)&ctx->actionBuffer, NULL, &ctx->actionBufferCap);
	free(ctx->dbSocketConnArg);
	free(ctx);
	return 0;
}

int ovsDestroy(const char* directory) {
	// PATH_MAX is quite large for the stack, so we use the heap
	char* ovsdbControl;
	char* ovsControl;
	newSprintf(&ovsdbControl, "%s/" OVSDB_CTL_FILE, directory);
	newSprintf(&ovsControl, "%s/" OVS_CTL_FILE, directory);

	const char* compatArgs = ovsCompatArgs();

	int err = 0;
	if (access(ovsControl, F_OK) != -1) {
		lprintf(LogDebug, "Shutting down Open vSwitch instance with control socket '%s'\n", ovsControl);
		err = ovsCommand(directory, "ovs-appctl", compatArgs, "-t", ovsControl, "exit", NULL);
		if (err != 0) {
			lprintf(LogError, "Failed to destroy Open vSwitch instance with control socket '%s'. Shut down the Open vSwitch system manually with ovs-appctl before continuing.\n", ovsControl);
		}
	}
	if (access(ovsdbControl, F_OK) != -1) {
		lprintf(LogDebug, "Shutting down OVSDB instance with control socket '%s'\n", ovsdbControl);
		err = ovsCommand(directory, "ovs-appctl", compatArgs, "-t", ovsdbControl, "exit", NULL);
		if (err != 0) {
			lprintf(LogError, "Failed to destroy OVSDB instance with control socket '%s'. Shut down the Open vSwitch system manually with ovs-appctl before continuing.\n", ovsControl);
		}
	}

	free(ovsdbControl);
	free(ovsControl);
	return err;
}

int ovsAddBridge(ovsContext* ctx, const char* name) {
	int err = switchContext(ctx);
	if (err != 0) return err;

	lprintf(LogDebug, "Creating Open vSwitch bridge '%s' in context %p\n", name, ctx);
	err = ovsCommand(ctx->directory, "ovs-vsctl", ctx->compatArgs, ctx->dbSocketConnArg, "add-br", name, NULL);
	return err;
}

int ovsDelBridge(ovsContext* ctx, const char* name) {
	int err = switchContext(ctx);
	if (err != 0) return err;

	char* brMgmt;
	newSprintf(&brMgmt, "%s/%s.mgmt", ctx->directory, name);
	if (access(brMgmt, F_OK) != -1) {
		lprintf(LogDebug, "Deleting Open vSwitch bridge '%s' in context %p\n", name, ctx);
		err = ovsCommand(ctx->directory, "ovs-vsctl", ctx->compatArgs, ctx->dbSocketConnArg, "del-br", name, NULL);
	}
	free(brMgmt);
	return err;
}

int ovsSetBridgeMtu(ovsContext* ctx, const char* bridge, int mtu) {
	if (mtu <= 0) {
		lprintf(LogError, "Requested MTU for OVS bridge is non-positive (%d)\n", mtu);
		return 1;
	}

	int err = switchContext(ctx);
	if (err != 0) return err;

	lprintf(LogDebug, "Setting MTU to %d for Open vSwitch bridge '%s' in context %p\n", mtu, bridge, ctx);

	char mtuStr[12 + (CHAR_BIT * sizeof(int) / 3) + 3];
	sprintf(mtuStr, "mtu_request=%d", mtu);

	err = ovsCommand(ctx->directory, "ovs-vsctl", ctx->compatArgs, ctx->dbSocketConnArg, "set", "int", bridge, mtuStr, NULL);
	return err;
}

int ovsAddPort(ovsContext* ctx, const char* bridge, const char* intfName) {
	int err = switchContext(ctx);
	if (err != 0) return err;

	lprintf(LogDebug, "Adding interface '%s' to Open vSwitch bridge '%s' in context %p\n", intfName, bridge, ctx);
	err = ovsCommand(ctx->directory, "ovs-vsctl", ctx->compatArgs, ctx->dbSocketConnArg, "add-port", bridge, intfName, NULL);
	if (err != 0) return err;

	return 0;
}

int ovsClearFlows(ovsContext* ctx, const char* bridge) {
	int err = switchContext(ctx);
	if (err != 0) return err;

	lprintf(LogDebug, "Removing all OpenFlow rules from bridge '%s' in context %p except for ARP switching\n", bridge, ctx);

	err = ovsCommand(ctx->directory, "ovs-ofctl", ctx->compatArgs, "del-flows", bridge, NULL);
	if (err != 0) return err;

	return 0;
}

int ovsAddArpResponse(ovsContext* ctx, const char* bridge, ip4Addr ip, const macAddr* mac, uint32_t priority) {
	int err = switchContext(ctx);
	if (err != 0) return err;

	char ipStr[IP4_ADDR_BUFLEN];
	ip4AddrToString(ip, ipStr);

	char macStr[MAC_ADDR_BUFLEN];
	macAddrToString(mac, macStr);

	// OVS requires the IP address in big endian hex string format
	char ipOctetStr[4*2+1];
	for (int i = 0; i < 4; ++i) {
		sprintf(&ipOctetStr[i*2], "%02x", (uint8_t)(((uint32_t)ip) >> (8 * i)) & 0xFFU);
	}
	ipOctetStr[4*2] = '\0';

	char macOctetStr[MAC_ADDR_BYTES*2+1];
	for (int i = 0; i < MAC_ADDR_BYTES; ++i) {
		sprintf(&macOctetStr[i*2], "%02x", mac->octets[i]);
	}
	macOctetStr[MAC_ADDR_BYTES*2] = '\0';

	lprintf(LogDebug, "Adding ARP response %s => %s to Open vSwitch bridge '%s' in context %p\n", ipStr, macStr, bridge, ctx);

	// We rewrite the source packet to transform it into an ARP response. There
	// isn't any cleaner way to do this in Open vSwitch, but this approach is
	// well-known online. One of our constraints is that we don't want to send
	// responses for internal ports (e.g., those switch ports connected to the
	// namespaces), so a simple action=NORMAL for ARP traffic is insufficient.

	size_t actionLen = 0;

	flexBufferPrintf((void**)&ctx->actionBuffer, &actionLen, &ctx->actionBufferCap, "dl_type=0x0806, priority=%u,", priority);
	--actionLen; // Remove NUL terminator to concatenate

	flexBufferPrintf((void**)&ctx->actionBuffer, &actionLen, &ctx->actionBufferCap, "nw_dst=%s,", ipStr);
	--actionLen;

	flexBufferPrintf((void**)&ctx->actionBuffer, &actionLen, &ctx->actionBufferCap, "actions=move:NXM_OF_ETH_SRC[]->NXM_OF_ETH_DST[],");
	--actionLen;

	flexBufferPrintf((void**)&ctx->actionBuffer, &actionLen, &ctx->actionBufferCap, "mod_dl_src:%s,", macStr);
	--actionLen;

	flexBufferPrintf((void**)&ctx->actionBuffer, &actionLen, &ctx->actionBufferCap, "load:0x2->NXM_OF_ARP_OP[], move:NXM_NX_ARP_SHA[]->NXM_NX_ARP_THA[], move:NXM_OF_ARP_SPA[]->NXM_OF_ARP_TPA[],");
	--actionLen;

	flexBufferPrintf((void**)&ctx->actionBuffer, &actionLen, &ctx->actionBufferCap, "load:0x%s->NXM_NX_ARP_SHA[],", macOctetStr);
	--actionLen;

	flexBufferPrintf((void**)&ctx->actionBuffer, &actionLen, &ctx->actionBufferCap, "load:0x%s->NXM_OF_ARP_SPA[],", ipOctetStr);
	--actionLen;

	flexBufferPrintf((void**)&ctx->actionBuffer, &actionLen, &ctx->actionBufferCap, "in_port");

	return ovsCommand(ctx->directory, "ovs-ofctl", ctx->compatArgs, "add-flow", bridge, ctx->actionBuffer, NULL);
}

int ovsAddIpFlow(ovsContext* ctx, const char* bridge, uint32_t inPort, const ip4Subnet* srcNet, const ip4Subnet* dstNet, const macAddr* newSrcMac, const macAddr* newDstMac, uint32_t outPort, uint32_t priority) {
	int err = switchContext(ctx);
	if (err != 0) return err;

	char subnetStr[IP4_CIDR_BUFLEN];
	char macStr[MAC_ADDR_BUFLEN];

	lprintHead(LogDebug);
	lprintDirectf(LogDebug, "Adding OpenFlow rule to bridge '%s' in context %p: priority %u, match (", bridge, ctx, priority);

	size_t actionLen = 0;

	flexBufferPrintf((void**)&ctx->actionBuffer, &actionLen, &ctx->actionBufferCap, "ip, priority=%u", priority);
	--actionLen; // Remove NUL terminator to concatenate

	if (inPort > 0) {
		lprintDirectf(LogDebug, "in port = %u", inPort);
		flexBufferPrintf((void**)&ctx->actionBuffer, &actionLen, &ctx->actionBufferCap, ", in_port=%u", inPort);
		--actionLen;
	}
	if (srcNet != NULL) {
		ip4SubnetToString(srcNet, subnetStr);
		lprintDirectf(LogDebug, ", source = %s", subnetStr);
		flexBufferPrintf((void**)&ctx->actionBuffer, &actionLen, &ctx->actionBufferCap, ", nw_src=%s", subnetStr);
		--actionLen;
	}
	if (dstNet != NULL) {
		ip4SubnetToString(dstNet, subnetStr);
		lprintDirectf(LogDebug, ", destination = %s", subnetStr);
		flexBufferPrintf((void**)&ctx->actionBuffer, &actionLen, &ctx->actionBufferCap, ", nw_dst=%s", subnetStr);
		--actionLen;
	}
	lprintDirectf(LogDebug, "), perform (out port = %u", outPort);
	flexBufferPrintf((void**)&ctx->actionBuffer, &actionLen, &ctx->actionBufferCap, ", actions=");
	--actionLen;
	if (newSrcMac != NULL) {
		macAddrToString(newSrcMac, macStr);
		lprintDirectf(LogDebug, "source MAC = %s", macStr);
		flexBufferPrintf((void**)&ctx->actionBuffer, &actionLen, &ctx->actionBufferCap, ", mod_dl_src=%s", macStr);
		--actionLen;
	}
	if (newDstMac != NULL) {
		macAddrToString(newDstMac, macStr);
		lprintDirectf(LogDebug, "destination MAC = %s", macStr);
		flexBufferPrintf((void**)&ctx->actionBuffer, &actionLen, &ctx->actionBufferCap, ", mod_dl_dst=%s", macStr);
		--actionLen;
	}
	lprintDirectf(LogDebug, ") priority %u\n", priority);
	lprintDirectFinish(LogDebug);
	flexBufferPrintf((void**)&ctx->actionBuffer, &actionLen, &ctx->actionBufferCap, ", output:%u", outPort);

	return ovsCommand(ctx->directory, "ovs-ofctl", ctx->compatArgs, "add-flow", bridge, ctx->actionBuffer, NULL);
}
