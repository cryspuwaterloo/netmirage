#pragma once

// This module enables interaction with Open vSwitch. This module is not
// thread-safe. Calling any of the functions in this module will result in the
// active network namespace for the process being changed.

#include "net.h"

typedef struct ovsContext_s ovsContext;

// Returns a human-readable version string for the Open vSwitch installation. If
// Open vSwitch is not installed or is not accessible, returns NULL. The caller
// is responsible for freeing this string.
char* ovsVersion(void);

// Starts up an isolated Open vSwitch instance in the given namespace. This
// instance includes its own configuration database server and switching daemon.
// All of the files (e.g., management sockets, PID files, logs) are stored in
// the given directory. If ovsSchema is set, it is used to load the OVS schema
// for OVSDB. If ovsSchema is NULL, then the default path is used. Returns a new
// OVS context on success. If an error occurs, returns NULL and sets err to the
// error code (if err is not NULL).
ovsContext* ovsStart(netContext* net, const char* directory, const char* ovsSchema, int* err);

// Releases resources associated with a given Open vSwitch instance, but allows
// it to continue running.
int ovsFree(ovsContext* ctx);

// Destroys a runnning Open vSwitch instance based in the given directory.
// Returns 0 on success or an error code otherwise.
int ovsDestroy(const char* directory);
