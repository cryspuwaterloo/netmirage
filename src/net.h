#pragma once

// This module defines several functions that enable manipulation of virtual
// networks. The functions are implemented using direct interaction with the
// kernel, since they represent one of the most significant performance
// bottlenecks. This module is not thread safe. Namespace changes will affect
// the entire process.

#include <stdbool.h>
#include <stdint.h>

typedef struct netContext_s netContext;

// Sets the prefix to use for creating namespaces. Max length is theoretically
// PATH_MAX-1.
void setNamespacePrefix(const char* prefix);

// Creates a new namespace with the given name. Visible to "ip". Automatically
// switches to the new namespace when called. Returns a context for the new
// namespace on success, or NULL on error. If err is not NULL, it is set to the
// error code on error. If an error occurs, the active namespace may no longer
// be valid.
netContext* netOpenNamespace(const char* name, int* err, bool excl);

// Frees resources associated with a context. This does not delete the
// underlying namespace, or cause the active namespace to switch.
void netFreeContext(netContext* ctx);

// Deletes a namespace. Returns 0 on success. Any associated contexts should be
// freed first. If the active namespace is deleted, the namespace will live
// until all bound processes are closed or switch to another namespace.
int netDeleteNamespace(const char* name);

// Switches the active namespace for the process. If the context is NULL,
// switches to the default network namespace for the PID namespace. Returns 0
// on success or an error code otherwise.
int netSwitchContext(netContext* ctx);

// The calls that use netlink can be asynchronous or synchronous. Asynchronous
// calls will not report kernel errors.

// Creates a virtual ethernet pair of interfaces with endpoints in the given
// context namespaces.
int netCreateVethPair(const char* name1, const char* name2, netContext* ctx1, netContext* ctx2, bool sync);

// Returns the interface index for an interface. On error, returns -1 and sets
// err (if provided) to the error code.
int netGetInterfaceIndex(netContext* ctx, const char* name, int* err);

// Adds an IPv4 address to an interface. addr should be an address in network
// byte order. subnetBits specifies the prefix length of the subnet. Returns 0
// on success or an error code otherwise.
int netAddInterfaceAddrIPv4(netContext* ctx, int devIdx, uint32_t addr, uint8_t subnetBits, uint32_t broadcastAddr, uint32_t anycastAddr, bool sync);

// Deletes an IPv4 address from the interface. Interfaces may have multiple
// addresses; this deletes only one of them. Returns 0 on success or an error
// code otherwise.
int netDelInterfaceAddrIPv4(netContext* ctx, int devIdx, bool sync);

// Brings an interface up or shuts it down. Returns 0 on success or an error
// code otherwise.
int netSetInterfaceUp(netContext* ctx, const char* name, bool up);

// Turns GRO (generic receive offload) on or off for an interface. Returns 0 on
// success or an error code otherwise.
int netSetInterfaceGro(netContext* ctx, const char* name, bool enabled);
