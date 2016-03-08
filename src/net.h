#pragma once

// This module defines several functions that enable manipulation of virtual networks.
// The functions are implemented using direct interaction with the kernel, since they
// represent one of the most significant performance bottlenecks. This module is not
// thread safe. Additionally, all calls will affect the entire process.

// Sets the prefix to use for creating namespaces. Max length is theoretically PATH_MAX-1.
void setNamespacePrefix(const char* prefix);

// Creates a new namespace with the given name. Visible to "ip". Automatically switches
// to the new namespace when called. Returns 0 on success.
int createNetNamespace(const char* name);

// Deletes a namespace. Returns 0 on success.
int deleteNetNamespace(const char* name);

// Switches the active namespace for the process. If name is NULL or an empty string,
// switches to the default network namespace for the PID namespace. Returns 0 on success.
int switchNetNamespace(const char* name);
