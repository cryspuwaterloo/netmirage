#pragma once

// This module enables interaction with Open vSwitch. This module is not
// thread-safe. Calling any of the functions in this module will result in the
// active network namespace for the process being changed.

#include "ip.h"
#include "net.h"

typedef struct ovsContext_s ovsContext;

// Returns a human-readable version string for the Open vSwitch installation. If
// Open vSwitch is not installed or is not accessible, returns NULL. The caller
// is responsible for freeing this string.
char* ovsVersion(void);

// Starts up an isolated Open vSwitch instance in the given namespace. This
// instance includes its own configuration database server and switching daemon.
// All of the files (e.g., management sockets, PID files, logs) are stored in
// the given directory. directory should be a valid pointer until ovsFree is
// called. If ovsSchema is set, it is used to load the OVS schema for OVSDB. If
// ovsSchema is NULL, then the default path is used. Returns a new OVS context
// on success. If an error occurs, returns NULL and sets err to the error code
// (if err is not NULL).
ovsContext* ovsStart(netContext* net, const char* directory, const char* ovsSchema, int* err);

// Releases resources associated with a given Open vSwitch instance, but allows
// it to continue running.
int ovsFree(ovsContext* ctx);

// Destroys a runnning Open vSwitch instance based in the given directory.
// Returns 0 on success or an error code otherwise.
int ovsDestroy(const char* directory);

// Adds a new bridge to the given Open vSwitch instance. Returns 0 on success or
// an error code otherwise.
int ovsAddBridge(ovsContext* ctx, const char* name);

// Adds a port to the bridge in the given Open vSwitch instance. If portId is
// not NULL, it is set to the new port index. Returns 0 on success or an error
// code otherwise.
int ovsAddPort(ovsContext* ctx, const char* bridge, const char* intfName, uint32_t* portId);

// Deletes all flows in a bridge except for normal responses to ARP packets. All
// other traffic will be silently dropped. Returns 0 on success or an error code
// otherwise.
int ovsArpOnly(ovsContext* ctx, const char* bridge, uint32_t priority);

// Adds a new IPv4 flow to a bridge. Matches traffic the comes from the inPort
// and with the given source and destination subnets. If inPort is 0, any port
// will match. If srcNet or dstNet are NULL, then they are not used for
// matching. If non-NULL, newSrcMac and newDstMac specify new MAC addresses for
// the Ethernet layer of the outgoing packet. The packet is sent out of outPort.
// Returns 0 on success or an error code otherwise.
int ovsAddIpFlow(ovsContext* ctx, const char* bridge, uint32_t inPort, const ip4Subnet* srcNet, const ip4Subnet* dstNet, const macAddr* newSrcMac, const macAddr* newDstMac, uint32_t outPort, uint32_t priority);
