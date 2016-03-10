#pragma once

// This module defines several functions that enable manipulation of virtual
// networks. The functions are implemented using direct interaction with the
// kernel, since they represent one of the most significant performance
// bottlenecks. This module is not thread safe. Namespace changes will affect
// the entire process.

#include <stdbool.h>

#include "netlink.h"

// Sets the prefix to use for creating namespaces. Max length is theoretically
// PATH_MAX-1.
void setNamespacePrefix(const char* prefix);

// Creates a new namespace with the given name. Visible to "ip". Automatically
// switches to the new namespace when called. Returns a file descriptor for the
// new namespace on success, or -1 on error. If err is not NULL, it is set to
// the error code on error.
int openNetNamespace(const char* name, int* err, bool excl);

// Deletes a namespace. Returns 0 on success. Any file descriptors should be
// closed first. If the active namespace is deleted, the namespace will live
// until all bound processes are closed or switch to another namespace.
int deleteNetNamespace(const char* name);

// Switches the active namespace for the process. Returns 0 on success.
int switchNetNamespace(int nsFd);

// Switches the active namespace for the process. If name is NULL or an empty
// string, switches to the default network namespace for the PID namespace.
// Returns 0 on success.
int switchNetNamespaceName(const char* name);

// The calls that use netlink can be asynchronous or synchronous. Asynchronous
// calls will not report kernel errors.

// Creates a virtual ethernet pair of interfaces with endpoints in the given
// namespaces.
int createVethPair(nlContext* ctx, const char* name1, const char* name2, int ns1, int ns2, bool sync);
