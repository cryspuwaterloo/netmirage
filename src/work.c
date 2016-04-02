#define _GNU_SOURCE

#include "work.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <unistd.h>

#include "ip.h"
#include "log.h"
#include "mem.h"
#include "net.h"
#include "topology.h"
#include "worker.h"

/* The architecture of this implementation is motivated by two goals:
 * 1) Enable parallel execution of kernel commands
 * 2) Isolate elevated privileges from the main program I/O
 * The primary constraint informing the design is that many of the kernel calls
 * performed by the functions in net.h operate on the process' active network
 * namespace. Since there can only be one active network namespace per PID, this
 * means that achieving parallelization requires a process pool rather than a
 * thread pool.
 *
 * Given these objectives and constraint, we use the following architecture:
 * - An unprivileged main process
 * - Calls to the work module generate work order structures defining the call
 * - A custom thread pool running in the main process receives work orders
 * - Each order thread in the main process serializes incoming orders and sends
 *   them through a pipe to the associated worker process
 * - Worker processes call the appropriate kernel interfaces to fulfill orders
 * - Worker processes send serialized responses or log messages back through a
 *   reverse pipe to the main process, as necessary
 * - In the main process, each worker has an associated response thread that
 *   receives responses and relays them to the main thread
 *
 * All of the state associated with a worker (e.g, pipe descriptors and thread
 * pointers) is stored in a "workplace". There are two different perspectives of
 * a workplace: the struct stored by the main process, and the one stored by the
 * child process. This module defines the management of these states and the
 * communication mechanism. The "worker" module defines the actual procedures
 * performed by the child processes.
 */

typedef enum {
	WorkerPing,
	WorkerTerminate,
	WorkerConfigure,
	WorkerGetEdgeRemoteMac,
	WorkerGetEdgeLocalMac,
	WorkerAddRoot,
	WorkerAddEdgeInterface,
	WorkerAddHost,
	WorkerSetSelfLink,
	WorkerEnsureSystemScaling,
	WorkerAddLink,
	WorkerAddInternalRoutes,
	WorkerAddClientRoutes,
	WorkerAddEdgeRoutes,
	WorkerDestroyHosts,
} WorkerOrderCode;

typedef struct {
	WorkerOrderCode code;
	union {
		struct {
			LogLevel logThreshold;
			bool logColorize;
			size_t nsPrefixLen;
			size_t ovsDirLen;
			size_t ovsSchemaLen;
			uint64_t softMemCap;
			char* nsPrefix;
			char* ovsDir;
			char* ovsSchema;
		} configure;
		struct {
			char intfName[INTERFACE_BUF_LEN];
			ip4Addr ip;
		} getEdgeRemoteMac;
		struct {
			char intfName[INTERFACE_BUF_LEN];
		} getEdgeLocalMac;
		struct {
			ip4Addr addrSelf;
			ip4Addr addrOther;
			bool existing;
		} addRoot;
		struct {
			nodeId id;
			ip4Addr ip;
			macAddr macs[NEEDED_MACS_CLIENT];
			TopoNode node;
		} addHost;
		struct {
			nodeId id;
			TopoLink link;
		} setSelfLink;
		struct {
			uint64_t linkCount;
			nodeId nodeCount;
			nodeId clientNodes;
		} ensureSystemScaling;
		struct {
			nodeId sourceId;
			nodeId targetId;
			ip4Addr sourceIp;
			ip4Addr targetIp;
			macAddr macs[NEEDED_MACS_LINK];
			TopoLink link;
		} addLink;
		struct {
			nodeId id1;
			nodeId id2;
			ip4Addr ip1;
			ip4Addr ip2;
			ip4Subnet subnet1;
			ip4Subnet subnet2;
		} addInternalRoutes;
		struct {
			nodeId clientId;
			macAddr clientMacs[NEEDED_MACS_CLIENT];
			ip4Subnet subnet;
			uint32_t edgePort;
		} addClientRoutes;
		struct {
			ip4Subnet edgeSubnet;
			uint32_t edgePort;
			macAddr edgeLocalMac;
			macAddr edgeRemoteMac;
		} addEdgeRoutes;
		struct {
			char intfName[INTERFACE_BUF_LEN];
		} addEdgeInterface;
	};
} WorkerOrder;

typedef enum {
	ResponseError,
	ResponsePong,
	ResponseLogPrint,
	ResponseLogEnd,
	ResponseGotMac,
	ResponseAddedEdgeInterface,
} WorkerResponseCode;

typedef struct {
	WorkerResponseCode code;
	union {
		struct {
			int code;
		} error;
		struct {
			size_t len;
		} logMessage;
		struct {
			macAddr mac;
		} gotMac;
		struct {
			uint32_t portId;
		} addedEdgeInterface;
	};
} WorkerResponse;

// Workplace context from the perspective of the main process
typedef struct {
	bool established;
	GThread* sendThread;
	GThread* responseThread;
	int ordersFd;    // Write end of work order pipe
	int responsesFd; // Read end of work response pipe

	char* logBuffer;
	size_t logLen;
	size_t logCap;
} WorkplaceMain; // TODO rename if no WorkplaceChild

// Module state for the main process
static struct {
	GMutex lock;

	guint poolSize;
	WorkplaceMain* workplaces;

	// State for handling outgoing orders:

	GAsyncQueue* orderQueue;

	uint32_t unsentOrders;
	GCond allOrdersSent;

	// State for responses from the child processes:

	bool receivedError;
	int errorCode;

	bool responseQueued;
	GCond receivedResponse;
	WorkerResponse response;

	guint pongsExpected;
	GCond pongsFinished;
} workMain;

// Memory clearing functions to prevent irrelevant alerts from debuggers
#ifdef DEBUG
#define ZERO_ORDER(order) do{ memset((order), 0, sizeof(WorkerOrder)); }while(0)
#define ZERO_RESPONSE(resp) do{ memset((resp), 0, sizeof(WorkerResponse)); }while(0)
#else
#define ZERO_ORDER(order) do{}while(0)
#define ZERO_RESPONSE(resp) do{}while(0)
#endif

static WorkerOrder* newOrder(WorkerOrderCode code) {
	WorkerOrder* order = emalloc(sizeof(WorkerOrder));
	ZERO_ORDER(order);
	order->code = code;
	return order;
}

static bool writeAll(int fd, const void* data, size_t len) {
	const char* p = data;
	ssize_t toWrite = (ssize_t)len;
	while (toWrite > 0) {
		ssize_t written = write(fd, p, (size_t)toWrite);
		if (written <= 0) return false;
		toWrite -= written;
		p += written;
	}
	return true;
}

static bool readAll(int fd, void* data, size_t len) {
	char* p = data;
	ssize_t toRead = (ssize_t)len;
	while (toRead > 0) {
		ssize_t readBytes = read(fd, p, (size_t)toRead);
		if (readBytes <= 0) return false;
		toRead -= readBytes;
		p += readBytes;
	}
	return true;
}

// Releases memory associated with an order (but not the order itself)
static void freeOrderContents(WorkerOrder* order) {
	if (order->code == WorkerConfigure) {
		free(order->configure.nsPrefix);
		free(order->configure.ovsDir);
		free(order->configure.ovsSchema);
	}
}

// Serializes a work order and sends it through the pipe to a child process
static bool writeOrderToWorkplace(WorkerOrder* order, WorkplaceMain* wp) {
	lprintf(LogDebug, "Sending order code %d to child in workplace %p\n", order->code, wp);
	if (!writeAll(wp->ordersFd, order, sizeof(WorkerOrder))) goto fail;

	// Write extraneous buffers
	if (order->code == WorkerConfigure) {
		if (!writeAll(wp->ordersFd, order->configure.nsPrefix, order->configure.nsPrefixLen)) goto fail;
		else if (!writeAll(wp->ordersFd, order->configure.ovsDir, order->configure.ovsDirLen)) goto fail;
		else if (!writeAll(wp->ordersFd, order->configure.ovsSchema, order->configure.ovsSchemaLen)) goto fail;
	}
	return true;
fail:
	lprintf(LogError, "Failed to send worker order code %d to child in workplace %p\n", order->code, wp);
	return false;
}

// Deserializes a work order from stdin
static bool readOrder(WorkerOrder* order) {
	if (!readAll(STDIN_FILENO, order, sizeof(WorkerOrder))) return false;

	// Read extraneous buffers
	if (order->code == WorkerConfigure) {
		order->configure.nsPrefix = ecalloc(order->configure.nsPrefixLen+1, 1);
		order->configure.ovsDir  = ecalloc(order->configure.ovsDirLen+1, 1);
		order->configure.ovsSchema = ecalloc(order->configure.ovsSchemaLen+1, 1);

		bool failed = false;
		if (!readAll(STDIN_FILENO, order->configure.nsPrefix, order->configure.nsPrefixLen)) failed = true;
		else if (!readAll(STDIN_FILENO, order->configure.ovsDir, order->configure.ovsDirLen)) failed = true;
		else if (!readAll(STDIN_FILENO, order->configure.ovsSchema, order->configure.ovsSchemaLen)) failed = true;
		if (failed) {
			freeOrderContents(order);
			return false;
		}
	}
	return true;
}

static void waitForSending(void) {
	g_mutex_lock(&workMain.lock);
	lprintln(LogDebug, "Waiting until all orders are sent to child processes");
	while (workMain.unsentOrders > 0) {
		g_cond_wait(&workMain.allOrdersSent, &workMain.lock);
	}
	g_mutex_unlock(&workMain.lock);
}

// Called by main process => main thread. The caller must hold workMain.lock
static int waitForResponse(WorkerResponseCode expectedCode) {
	lprintln(LogDebug, "Waiting for response from worker pool");
	while (!workMain.receivedError && !workMain.responseQueued) {
		g_cond_wait(&workMain.receivedResponse, &workMain.lock);
	}
	int err = 0;
	if (workMain.receivedError) {
		err = workMain.errorCode;
	} else if (workMain.response.code != expectedCode) {
		lprintf(LogError, "Unexpected response code %d from worker pool\n", workMain.response.code);
		err = 1;
	} else {
		workMain.responseQueued = false;
	}
	return err;
}

// Called by main process => main thread
static int sendOrder(WorkerOrder* order) {
	g_mutex_lock(&workMain.lock);
	bool abort = workMain.receivedError;
	g_mutex_unlock(&workMain.lock);
	if (abort) return workMain.errorCode;

	g_mutex_lock(&workMain.lock);
	++workMain.unsentOrders;
	g_async_queue_push(workMain.orderQueue, order);
	g_mutex_unlock(&workMain.lock);

	return 0;
}

// Called by main process => main thread
static bool broadcastOrder(WorkerOrder* order) {
	// Make sure that all sender threads are blocked reading from the queue
	waitForSending();

	lprintf(LogDebug, "Broadcasting order code %d to all child processes\n", order->code);

	// Send the order directly to each child process
	bool success = true;
	for (guint i = 0; i < workMain.poolSize; ++i) {
		WorkplaceMain* wp = &workMain.workplaces[i];
		if (!writeOrderToWorkplace(order, wp)) {
			lprintf(LogWarning, "Could not broadcast configuration order to child in workplace %p\n", wp);
			success = false;
		}
	}
	return success;
}

// The entry point for the send threads in the main process
static void* sendThread(gpointer data) {
	WorkplaceMain* wp = data;

	bool loop = true;
	while (loop) {
		gpointer item = g_async_queue_pop(workMain.orderQueue);
		WorkerOrder* order = item;
		if (order->code == WorkerTerminate) {
			loop = false;
		} else {
			writeOrderToWorkplace(order, wp);
		}
		freeOrderContents(order);
		free(order);

		g_mutex_lock(&workMain.lock);
		--workMain.unsentOrders;
		if (workMain.unsentOrders == 0) g_cond_signal(&workMain.allOrdersSent);
		g_mutex_unlock(&workMain.lock);
	}

	lprintf(LogDebug, "Order sending thread for workplace %p shutting down\n", wp);
	close(wp->ordersFd);
	return NULL;
}

// The entry point for the response threads in the main process
static void* responseThread(gpointer data) {
	WorkplaceMain* wp = data;

	while (true) {
		WorkerResponse resp;
		if (!readAll(wp->responsesFd, &resp, sizeof(WorkerResponse))) break;

		switch (resp.code) {
		case ResponsePong:
			g_mutex_lock(&workMain.lock);
			--workMain.pongsExpected;
			if (workMain.pongsExpected == 0) g_cond_signal(&workMain.pongsFinished);
			g_mutex_unlock(&workMain.lock);
			break;
		case ResponseLogPrint:
			flexBufferGrow((void**)&wp->logBuffer, wp->logLen, &wp->logCap, resp.logMessage.len, 1);
			if (!readAll(wp->responsesFd, &wp->logBuffer[wp->logLen], resp.logMessage.len)) goto done;
			wp->logLen += resp.logMessage.len;
			break;
		case ResponseLogEnd:
			flexBufferGrowAppendStr((void**)&wp->logBuffer, &wp->logLen, &wp->logCap, "");
			lprintRaw(wp->logBuffer);
			wp->logLen = 0;
			break;
		case ResponseError:
			g_mutex_lock(&workMain.lock);
			workMain.errorCode = resp.error.code;
			workMain.receivedError = true;

			// We need to signal other conditions just in case an error occurred
			// while we were waiting for something else. Normally, we just
			// handle errors asynchronously.
			g_cond_signal(&workMain.receivedResponse);
			g_cond_signal(&workMain.pongsFinished);

			g_mutex_unlock(&workMain.lock);
			break;
		default:
			g_mutex_lock(&workMain.lock);
			workMain.response = resp;
			workMain.responseQueued = true;
			g_cond_signal(&workMain.receivedResponse);
			g_mutex_unlock(&workMain.lock);
		}
	}

done:
	lprintf(LogDebug, "Response thread for workplace %p shutting down\n", wp);
	close(wp->responsesFd);
	return NULL;
}

// Called by child process
static void childLogPrint(const char* msg) {
	WorkerResponse resp;
	ZERO_RESPONSE(&resp);
	if (msg == NULL) {
		resp.code = ResponseLogEnd;
	} else {
		resp.code = ResponseLogPrint;
		resp.logMessage.len = strlen(msg);
	}
	resp.code = (msg == NULL ? ResponseLogEnd : ResponseLogPrint);
	writeAll(STDOUT_FILENO, &resp, sizeof(WorkerResponse));
	if (msg != NULL) {
		writeAll(STDOUT_FILENO, msg, resp.logMessage.len);
	}
}

// Called by child process
static void respondError(int code) {
	lprintf(LogError, "Sending error code %d to parent process\n", code);
	WorkerResponse resp;
	ZERO_RESPONSE(&resp);
	resp.code = ResponseError;
	resp.error.code = code;
	writeAll(STDOUT_FILENO, &resp, sizeof(WorkerResponse));
}

// The entry point for child processes
static int childProcess(guint id) {
	char prefix[20];
	snprintf(prefix, 20, " [W%u]", id);

	bool parentColorized = logColorized();
	logSetCallback(&childLogPrint);
	logSetColorize(parentColorized);
	logSetPrefix(prefix);

	bool initialized = false;

	while (true) {
		WorkerOrder order;
		if (!readOrder(&order)) break;
		lprintf(LogDebug, "Received order code %d\n", order.code);

		// The worker must only be initialized once
		if (initialized == (order.code == WorkerConfigure)) {
			lprintf(LogError, "Unexpected order code %d for %s worker\n", order.code, initialized ? "initialized" : "uninitialized");
			respondError(1);
		} else {
			int err = 0;
			switch (order.code) {
			case WorkerPing: {
				WorkerResponse resp;
				ZERO_RESPONSE(&resp);
				resp.code = ResponsePong;
				writeAll(STDOUT_FILENO, &resp, sizeof(WorkerResponse));
				break;
			}
			case WorkerConfigure: {
				logSetColorize(order.configure.logColorize);
				logSetThreshold(order.configure.logThreshold);
				lprintf(LogDebug, "Configuring worker process\n");
				err = workerInit(order.configure.nsPrefix, order.configure.ovsDir, order.configure.ovsSchema, order.configure.softMemCap);
				if (err == 0) {
					initialized = true;
				} else {
					lprintln(LogError, "Failed to initialize worker due to malformed configuration order");
				}
				break;
			}
			case WorkerGetEdgeRemoteMac: {
				WorkerResponse resp;
				ZERO_RESPONSE(&resp);
				resp.code = ResponseGotMac;

				err = workerGetEdgeRemoteMac(order.getEdgeRemoteMac.intfName, order.getEdgeRemoteMac.ip, &resp.gotMac.mac);
				if (err == 0) writeAll(STDOUT_FILENO, &resp, sizeof(WorkerResponse));
				break;
			}
			case WorkerGetEdgeLocalMac: {
				WorkerResponse resp;
				ZERO_RESPONSE(&resp);
				resp.code = ResponseGotMac;

				err = workerGetEdgeLocalMac(order.getEdgeLocalMac.intfName, &resp.gotMac.mac);
				if (err == 0) writeAll(STDOUT_FILENO, &resp, sizeof(WorkerResponse));
				break;
			}
			case WorkerAddRoot:
				err = workerAddRoot(order.addRoot.addrSelf, order.addRoot.addrOther, order.addRoot.existing);
				break;
			case WorkerAddEdgeInterface: {
				WorkerResponse resp;
				ZERO_RESPONSE(&resp);
				resp.code = ResponseAddedEdgeInterface;

				err = workerAddEdgeInterface(order.addEdgeInterface.intfName, &resp.addedEdgeInterface.portId);
				if (err == 0) writeAll(STDOUT_FILENO, &resp, sizeof(WorkerResponse));
				break;
			}
			case WorkerAddHost:
				err = workerAddHost(order.addHost.id, order.addHost.ip, order.addHost.macs, &order.addHost.node);
				break;
			case WorkerSetSelfLink:
				err = workerSetSelfLink(order.setSelfLink.id, &order.setSelfLink.link);
				break;
			case WorkerEnsureSystemScaling:
				err = workerEnsureSystemScaling(order.ensureSystemScaling.linkCount, order.ensureSystemScaling.nodeCount, order.ensureSystemScaling.clientNodes);
				break;
			case WorkerAddLink:
				err = workerAddLink(order.addLink.sourceId, order.addLink.targetId, order.addLink.sourceIp, order.addLink.targetIp, order.addLink.macs, &order.addLink.link);
				break;
			case WorkerAddInternalRoutes:
				err = workerAddInternalRoutes(order.addInternalRoutes.id1, order.addInternalRoutes.id2, order.addInternalRoutes.ip1, order.addInternalRoutes.ip2, &order.addInternalRoutes.subnet1, &order.addInternalRoutes.subnet2);
				break;
			case WorkerAddClientRoutes:
				err = workerAddClientRoutes(order.addClientRoutes.clientId, order.addClientRoutes.clientMacs, &order.addClientRoutes.subnet, order.addClientRoutes.edgePort);
				break;
			case WorkerAddEdgeRoutes:
				err = workerAddEdgeRoutes(&order.addEdgeRoutes.edgeSubnet, order.addEdgeRoutes.edgePort, &order.addEdgeRoutes.edgeLocalMac, &order.addEdgeRoutes.edgeRemoteMac);
				break;
			case WorkerDestroyHosts:
				err = workerDestroyHosts();
				break;
			default:
				lprintf(LogError, "Unknown order code %d\n", order.code);
				err = 1;
				break;
			}
			if (err != 0) respondError(err);
		}
		freeOrderContents(&order);
	}
	lprintln(LogDebug, "Child process terminating");
	if (initialized) {
		return workerCleanup();
	}
	return 0;
}

// Called by main process => main thread
static bool initWorkplaceMainForks(WorkplaceMain* wpm, guint id) {
	int pipefd[2];

	flexBufferInit((void**)&wpm->logBuffer, &wpm->logLen, &wpm->logCap);

	if (pipe(pipefd) != 0) return false;
	int childOrdersFd = pipefd[0];
	wpm->ordersFd = pipefd[1];

	if (pipe(pipefd) != 0) goto pipeAbort;
	wpm->responsesFd = pipefd[0];
	int childResponsesFd = pipefd[1];

	pid_t pid = fork();
	if (pid == 0) { // Child process
		// Close all file descriptors meant for the main process
		for (guint i = 0; i < workMain.poolSize; ++i) {
			WorkplaceMain* otherWpm = &workMain.workplaces[i];
			if (!otherWpm->established && otherWpm != wpm) continue;
			close(otherWpm->ordersFd);
			close(otherWpm->responsesFd);
		}

		// Redirect standard streams to pipes
		if (dup2(childOrdersFd, STDIN_FILENO) == -1) exit(1);
		if (dup2(childResponsesFd, STDOUT_FILENO) == -1) exit(1);

		// Close extraneous streams
		close(childOrdersFd);
		close(childResponsesFd);
		close(STDERR_FILENO);

		exit(childProcess(id));
	} else if (pid == -1) goto forkAbort;

	// Parent process

	close(childOrdersFd);
	close(childResponsesFd);

	lprintf(LogDebug, "Child process with PID %u created for workplace %p\n", pid, wpm);

	return true;

forkAbort:
	close(wpm->responsesFd);
	close(childResponsesFd);
pipeAbort:
	close(childOrdersFd);
	close(wpm->ordersFd);
	return false;
}

// Called by main process => main thread
static void initWorkplaceMainThreads(WorkplaceMain* wp) {
	lprintf(LogDebug, "Launching worker threads for workplace %p\n", wp);
	wp->sendThread = g_thread_new("SendThread", &sendThread, wp);
	wp->responseThread = g_thread_new("ResponseThread", &responseThread, wp);
}

// Called by main process => main thread
static void freeWorkplaceMain(WorkplaceMain* wpm) {
	flexBufferFree((void**)&wpm->logBuffer, &wpm->logLen, &wpm->logCap);
}

// Called by main process => main thread
int workInit(void) {
	workMain.poolSize = g_get_num_processors();
	workMain.workplaces = eamalloc(workMain.poolSize, sizeof(WorkplaceMain), 0);
	workMain.orderQueue = g_async_queue_new_full(&g_free);
	workMain.unsentOrders = 0;
	workMain.responseQueued = false;

	lprintf(LogDebug, "Initializing %u worker processes\n", workMain.poolSize);

	for (guint i = 0; i < workMain.poolSize; ++i) {
		workMain.workplaces[i].established = false;
	}

	// First, spawn the child processes
	bool setupSuccess = true;
	for (guint i = 0; i < workMain.poolSize; ++i) {
		bool success  = initWorkplaceMainForks(&workMain.workplaces[i], i);
		if (success) {
			workMain.workplaces[i].established = true;
		} else {
			lprintf(LogError, "Failed to launch child process %u\n", i);
			setupSuccess = false;
		}
	}

	// Spawn associated threads for successfully started children. We do this as
	// a separate step so that the forks occur while the main process is still
	// single threaded. This prevents the possibility of permanently locked
	// synchronization objects in libraries.
	for (guint i = 0; i < workMain.poolSize; ++i) {
		if (!workMain.workplaces[i].established) continue;
		initWorkplaceMainThreads(&workMain.workplaces[i]);
	}

	if (!setupSuccess) {
		lprintln(LogDebug, "Performing cleanup of partially constructed worker subsystem");
		workCleanup();
		return 1;
	}
	return 0;
}

// Called by main process => main thread
int workConfigure(LogLevel logThreshold, bool logColorize, const char* nsPrefix, const char* ovsDir, const char* ovsSchema, uint64_t softMemCap) {
	WorkerOrder order;
	order.code = WorkerConfigure;
	order.configure.logThreshold = logThreshold;
	order.configure.logColorize = logColorize;
	order.configure.nsPrefixLen = strlen(nsPrefix);
	order.configure.ovsDirLen = strlen(ovsDir);
	order.configure.ovsSchemaLen = (ovsSchema == NULL ? 0 : strlen(ovsSchema));
	order.configure.softMemCap = (uint64_t)llrint((double)softMemCap / (double)workMain.poolSize);
	order.configure.nsPrefix = strdup(nsPrefix);
	order.configure.ovsDir = strdup(ovsDir);
	order.configure.ovsSchema = strdup(ovsSchema == NULL ? "" : ovsSchema);
	bool success = broadcastOrder(&order);
	freeOrderContents(&order);
	return success ? 0 : 1;
}

// Called by main process => main thread
int workCleanup(void) {
	int err = 0;
	int res;

	// Reset any existing error so that the send threads will accept our
	// termination order
	g_mutex_lock(&workMain.lock);
	if (workMain.receivedError) {
		err = workMain.errorCode;
		workMain.receivedError = false;
	}
	g_mutex_unlock(&workMain.lock);

	// Send enough WorkerTerminate orders to stop all order threads. This will
	// cause the processes to exit, which will cause the response threads to
	// exit.
	lprintln(LogDebug, "Sending termination orders to worker threads");
	for (guint i = 0; i < workMain.poolSize; ++i) {
		if (!workMain.workplaces[i].established) continue;
		res = sendOrder(newOrder(WorkerTerminate));
		if (err == 0) err = res;
	}

	// Wait until all threads exit
	waitForSending();
	for (guint i = 0; i < workMain.poolSize; ++i) {
		if (!workMain.workplaces[i].established) continue;
		g_thread_join(workMain.workplaces[i].sendThread);
		g_thread_join(workMain.workplaces[i].responseThread);
	}

	// Everything is terminated, so we can release the resources
	lprintln(LogDebug, "Releasing resources for worker subsystem");
	if (err == 0) err = res;
	for (guint i = 0; i < workMain.poolSize; ++i) {
		if (!workMain.workplaces[i].established) continue;
		freeWorkplaceMain(&workMain.workplaces[i]);
	}
	free(workMain.workplaces);
	g_async_queue_unref(workMain.orderQueue);
	return err;
}

// Called by main process => main thread
int workJoin(bool resetError) {
	lprintf(LogDebug, "Performing join on worker pool%s to ensure that all work is finished\n", (resetError ? " (and resetting error state)" : ""));

	// Flush all previous work
	waitForSending();

	g_mutex_lock(&workMain.lock);
	workMain.pongsExpected = workMain.poolSize;
	if (resetError) {
		workMain.receivedError = false;
	}
	g_mutex_unlock(&workMain.lock);

	// Request ping replies from all processes. This demonstrates that all work
	// has been finished by the processes (not merely sent to them).
	WorkerOrder order;
	order.code = WorkerPing;
	if (!broadcastOrder(&order)) return 1;
	g_mutex_lock(&workMain.lock);
	while (workMain.pongsExpected > 0) {
		g_cond_wait(&workMain.pongsFinished, &workMain.lock);
		if (workMain.receivedError) {
			if (resetError) workMain.receivedError = false;
			else break;
		}
	}
	int err = 0;
	if (workMain.receivedError) err = workMain.errorCode;
	g_mutex_unlock(&workMain.lock);

	lprintln(LogDebug, "Worker pool has finished all of its work");
	return err;
}

// All of the following functions expose worker functionality to the main thread
// of the main process

int workGetEdgeRemoteMac(const char* intfName, ip4Addr ip, macAddr* edgeRemoteMac) {
	WorkerOrder* order = newOrder(WorkerGetEdgeRemoteMac);
	strncpy(order->getEdgeRemoteMac.intfName, intfName, INTERFACE_BUF_LEN);
	order->getEdgeRemoteMac.ip = ip;
	int err = sendOrder(order);
	if (err != 0) return err;

	g_mutex_lock(&workMain.lock);
	err = waitForResponse(ResponseGotMac);
	if (err == 0) {
		memcpy(edgeRemoteMac->octets, workMain.response.gotMac.mac.octets, MAC_ADDR_BYTES);
	}
	g_mutex_unlock(&workMain.lock);
	return err;
}

int workGetEdgeLocalMac(const char* intfName, macAddr* edgeLocalMac) {
	WorkerOrder* order = newOrder(WorkerGetEdgeLocalMac);
	strncpy(order->getEdgeLocalMac.intfName, intfName, INTERFACE_BUF_LEN);
	int err = sendOrder(order);
	if (err != 0) return err;

	g_mutex_lock(&workMain.lock);
	err = waitForResponse(ResponseGotMac);
	if (err == 0) {
		memcpy(edgeLocalMac->octets, workMain.response.gotMac.mac.octets, MAC_ADDR_BYTES);
	}
	g_mutex_unlock(&workMain.lock);
	return err;
}

int workAddRoot(ip4Addr addrSelf, ip4Addr addrOther) {
	WorkerOrder* loadOrder = newOrder(WorkerAddRoot);
	loadOrder->addRoot.addrSelf = addrSelf;
	loadOrder->addRoot.addrOther = addrOther;
	loadOrder->addRoot.existing = true;

	WorkerOrder* createOrder = emalloc(sizeof(WorkerOrder));
	*createOrder = *loadOrder;
	createOrder->addRoot.existing = false;

	// First, instruct any one worker to create the root namespace
	int err = sendOrder(createOrder);
	if (err != 0) return err;

	err = workJoin(false);
	if (err != 0) return err;

	// Next, make sure that all workers load root namespace contexts
	bool success = broadcastOrder(loadOrder);
	free(loadOrder);
	return (success ? 0 : 1);
}

int workAddEdgeInterface(const char* intfName, uint32_t* portId) {
	WorkerOrder* order = newOrder(WorkerAddEdgeInterface);
	strncpy(order->addEdgeInterface.intfName, intfName, INTERFACE_BUF_LEN);
	int err = sendOrder(order);
	if (err != 0) return err;

	g_mutex_lock(&workMain.lock);
	err = waitForResponse(ResponseAddedEdgeInterface);
	if (err == 0) {
		*portId = workMain.response.addedEdgeInterface.portId;
	}
	g_mutex_unlock(&workMain.lock);
	return err;
}

int workAddHost(nodeId id, ip4Addr ip, macAddr macs[], const TopoNode* node) {
	WorkerOrder* order = newOrder(WorkerAddHost);
	order->addHost.id = id;
	order->addHost.ip = ip;
	if (node->client) {
		for (int i = 0; i < NEEDED_MACS_CLIENT; ++i) {
			memcpy(order->addHost.macs[i].octets, macs[i].octets, MAC_ADDR_BYTES);
		}
	}
	order->addHost.node = *node;
	return sendOrder(order);
}

int workSetSelfLink(nodeId id, const TopoLink* link) {
	WorkerOrder* order = newOrder(WorkerSetSelfLink);
	order->setSelfLink.id = id;
	order->setSelfLink.link = *link;
	return sendOrder(order);
}

int workEnsureSystemScaling(uint64_t linkCount, nodeId nodeCount, nodeId clientNodes) {
	WorkerOrder* order = newOrder(WorkerEnsureSystemScaling);
	order->ensureSystemScaling.linkCount = linkCount;
	order->ensureSystemScaling.nodeCount = nodeCount;
	order->ensureSystemScaling.clientNodes = clientNodes;
	return sendOrder(order);
}

int workAddLink(nodeId sourceId, nodeId targetId, ip4Addr sourceIp, ip4Addr targetIp, macAddr macs[], const TopoLink* link) {
	WorkerOrder* order = newOrder(WorkerAddLink);
	order->addLink.sourceId = sourceId;
	order->addLink.targetId = targetId;
	order->addLink.sourceIp = sourceIp;
	order->addLink.targetIp = targetIp;
	for (int i = 0; i < NEEDED_MACS_LINK; ++i) {
		memcpy(order->addLink.macs[i].octets, macs[i].octets, MAC_ADDR_BYTES);
	}
	order->addLink.link = *link;
	return sendOrder(order);
}

int workAddInternalRoutes(nodeId id1, nodeId id2, ip4Addr ip1, ip4Addr ip2, const ip4Subnet* subnet1, const ip4Subnet* subnet2) {
	WorkerOrder* order = newOrder(WorkerAddInternalRoutes);
	order->addInternalRoutes.id1 = id1;
	order->addInternalRoutes.id2 = id2;
	order->addInternalRoutes.ip1 = ip1;
	order->addInternalRoutes.ip2 = ip2;
	order->addInternalRoutes.subnet1 = *subnet1;
	order->addInternalRoutes.subnet2 = *subnet2;
	return sendOrder(order);
}

int workAddClientRoutes(nodeId clientId, macAddr clientMacs[], const ip4Subnet* subnet, uint32_t edgePort) {
	WorkerOrder* order = newOrder(WorkerAddClientRoutes);
	order->addClientRoutes.clientId = clientId;
	for (int i = 0; i < NEEDED_MACS_CLIENT; ++i) {
		memcpy(order->addClientRoutes.clientMacs[i].octets, clientMacs[i].octets, MAC_ADDR_BYTES);
	}
	order->addClientRoutes.subnet = *subnet;
	order->addClientRoutes.edgePort = edgePort;
	return sendOrder(order);
}

int workAddEdgeRoutes(const ip4Subnet* edgeSubnet, uint32_t edgePort, const macAddr* edgeLocalMac, const macAddr* edgeRemoteMac) {
	WorkerOrder* order = newOrder(WorkerAddEdgeRoutes);
	order->addEdgeRoutes.edgeSubnet = *edgeSubnet;
	order->addEdgeRoutes.edgePort = edgePort;
	memcpy(order->addEdgeRoutes.edgeLocalMac.octets, edgeLocalMac->octets, MAC_ADDR_BYTES);
	memcpy(order->addEdgeRoutes.edgeRemoteMac.octets, edgeRemoteMac->octets, MAC_ADDR_BYTES);
	return sendOrder(order);
}

int workDestroyHosts(void) {
	return sendOrder(newOrder(WorkerDestroyHosts));
}
