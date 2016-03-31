#pragma once

// This module defines several functions that enable manipulation of virtual
// networks. The functions are implemented using direct interaction with the
// kernel, since they represent one of the most significant performance
// bottlenecks. This module is not thread safe. Namespace changes will affect
// the entire process. Some operations may be asynchronous or synchronous.
// Asynchronous calls will not report kernel errors.

// See the GOTCHAS file for common issues and misconceptions related to this
// module, or if the code stops working in a new kernel version.

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "ip.h"

typedef struct netContext_s netContext;

extern const size_t InterfaceBufLen;

// Initializes the network configuration module. Max length of namespacePrefix
// is theoretically PATH_MAX-1. Returns 0 on success or an error code otherwise.
int netInit(const char* namespacePrefix);

// Frees all resources associated with the net subsystem
void netCleanup(void);

// Opens a namespace with the given name. If the namespace does not exist, it is
// first created. If it already exists and excl is true, an error is raised.
// Namespaces created this way are visible to iproute2. If name is NULL, then
// the context is opened for the default namespace. Automatically switches to
// the namespace when called. Returns a context for the new namespace on
// success, or NULL on error. If err is not NULL, it is set to the error code on
// error. If an error occurs, the active namespace may no longer be valid.
netContext* netOpenNamespace(const char* name, bool excl, int* err);

// Opens a namespace using existing storage space. If reusing is false, then the
// space is completely initialized. If reusing is true, then the context must
// have been previously created with netOpenNamespace and subsequently
// invalidated with netInvalidateContext. On success, this function has the same
// effects as netOpenNamespace. Returns 0 on success or an error code otherwise.
int netOpenNamespaceInPlace(netContext* ctx, bool reusing, const char* name, bool excl);

// Invalidates a context so that its memory can be reused to create a new one.
void netInvalidateContext(netContext* ctx);

// Frees resources associated with a context. This does not delete the
// underlying namespace, or cause the active namespace to switch.
void netCloseNamespace(netContext* ctx, bool inPlace);

// Deletes a namespace. Returns 0 on success. Any associated contexts should be
// freed first. If the active namespace is deleted, the namespace will live
// until all bound processes are closed or switch to another namespace.
int netDeleteNamespace(const char* name);

// Enumerates all of the namespaces on the system matching the prefix. If
// nsCallback returns a non-zero value, enumation is terminated and the value is
// returned to the caller. Returns 0 on success.
typedef int (*netNsCallback)(const char* name, void* userData);
int netEnumNamespaces(netNsCallback callback, void* userData);

// Switches the active namespace for the process. If the context is NULL,
// switches to the default network namespace for the PID namespace. Returns 0
// on success or an error code otherwise.
int netSwitchNamespace(netContext* ctx);

// Creates a virtual Ethernet pair of interfaces with endpoints in the given
// namespaces. If the MAC addresses are not NULL, they are used to configure the
// new interfaces.
int netCreateVethPair(const char* name1, const char* name2, netContext* ctx1, netContext* ctx2, const macAddr* addr1, const macAddr* addr2, bool sync);

// Returns the interface index for an interface. On error, returns -1 and sets
// err (if provided) to the error code.
int netGetInterfaceIndex(netContext* ctx, const char* name, int* err);

// Moves an interface from one namespace to another. Returns 0 on success or an
// error code otherwise.
int netMoveInterface(netContext* srcCtx, int devIdx, netContext* dstCtx, bool sync);

// Adds an IPv4 address to an interface. subnetBits specifies the prefix length
// of the subnet. Returns 0 on success or an error code otherwise.
int netAddInterfaceAddrIPv4(netContext* ctx, int devIdx, ip4Addr addr, uint8_t subnetBits, ip4Addr broadcastAddr, ip4Addr anycastAddr, bool sync);

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

// Uses Linux Traffic Control to apply shaping to outgoing packets on an
// interface. By default, interfaces have no delays or bandwidth limits.
// lossRate should be a value between 0.0 and 1.0. Passing 0.0 for rateMbit
// means that no limits are applied. queueLen specifies the maximum number of
// packets in the queue before packets are dropped. A value of 0 for queueLen
// uses the traffic control default value. Returns 0 on success or an error code
// otherwise.
int netSetEgressShaping(netContext* ctx, int devIdx, double delayMs, double jitterMs, double lossRate, double rateMbit, uint32_t queueLen, bool sync);

// Adds a static ARP entry to the routing table. Returns 0 on success or an
// error code otherwise.
int netAddStaticArp(netContext* ctx, const char* intfName, ip4Addr ip, const macAddr* mac);

// Retrieves the MAC address corresponding to an IP address from the ARP cache.
// Returns 0 on success or an error code otherwise. If EAGAIN is returned, then
// the entry was not found in the cache.
int netGetRemoteMacAddr(netContext* ctx, const char* intfName, ip4Addr ip, macAddr* result);

// Retrieves the MAC address associated with a local interface. Returns 0 on
// success or an error code otherwise.
int netGetLocalMacAddr(netContext* ctx, const char* name, macAddr* result);

// Enables or disables packet routing between interfaces in the active
// namespace. Returns 0 on success or an error code otherwise.
int netSetForwarding(bool enabled);

// Allows or disallows Martian packets in the active namespace. This setting is
// only guaranteed to operate properly if it is set before any interfaces are
// added to the namespace; if interfaces were added before this call, then the
// implications are subtle and depend on the details of the calls. Returns 0 on
// success or an error code otherwise.
int netSetMartians(bool allow);

// Enables or disables IPv6 in the active namespace. Returns 0 on success or an
// error code otherwise.
int netSetIPv6(bool enabled);

// Gets the garbage collector thresholds for the system-wide ARP hash table.
// Must be called within the init namespace. Returns 0 on success or an error
// code otherwise.
int netGetArpTableSize(int* thresh1, int* thresh2, int* thresh3);

// Sets the garbage collector thresholds for the system-wide ARP hash table.
// Must be called within the init namespace. Returns 0 on success or an error
// code otherwise.
int netSetArpTableSize(int thresh1, int thresh2, int thresh3);

typedef enum {
	TableMain,
	TableLocal,
} RoutingTable;

typedef enum {
	ScopeLink,
	ScopeGlobal,
} RoutingScope;

// Adds a static routing entry to the given routing table. The destination is
// given by dstAddr with the subnetBits most significant bits specifying the
// subnet. Packets are routed via the specified gatewayAddr. dstDevIdx
// identifies the interface through which the packets should be sent. The
// target gateway must be reachable when this command is executed. Moreover, the
// all bits in dstAddr not covered by subnetBits should be set to 0. If
// gatewayAddr is 0, then no gateway is used. Returns 0 on success or an error
// code otherwise.
int netAddRoute(netContext* ctx, RoutingTable table, RoutingScope scope, ip4Addr dstAddr, uint8_t subnetBits, ip4Addr gatewayAddr, int dstDevIdx, bool sync);

// This function is the same as netAddRoute, except it allows the caller to
// provide an explicit table identifier (e.g., to add routes to custom tables).
int netAddRouteToTable(netContext* ctx, uint8_t table, RoutingScope scope, ip4Addr dstAddr, uint8_t subnetBits, ip4Addr gatewayAddr, int dstDevIdx, bool sync);

// Adds a new rule to Linux's policy routing system. The rule matches packets
// within the given subnet. If inputIntf is not NULL, then the rule matches only
// packets coming from the named interface. Packets matched by this rule will be
// routed according to the given routing table. The rule priority specifies the
// order in which rules are evaluated. There is an unchangeable default rule
// with priority 0 that causes the local routing table to be scanned first.
// Returns 0 on success or an error code otherwise.
int netAddRule(netContext* ctx, const ip4Subnet* subnet, const char* inputIntf, RoutingTable table, uint32_t priority, bool sync);

// This function is the same as netAddRule, except it allows the caller to
// provide an explicit table identifier.
int netAddRuleForTable(netContext* ctx, const ip4Subnet* subnet, const char* inputIntf, uint8_t table, uint32_t priority, bool sync);
