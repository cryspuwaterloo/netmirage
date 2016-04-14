#pragma once

// This module implements a mapping between namespace names and contexts. It is
// implemented using efficient data structures that cache the most requested
// contexts while maintaining an upper bound on memory use.

#include <stdlib.h>

#include "net.h"
#include "topology.h"

typedef struct netCache_s netCache;

// Allocates a new cache. The maxMemoryUse serves as a guideline for the size of
// the cache, but is not a hard limit; the function is free to exceed or ignore
// the recommendation.
netCache* ncNewCache(uint64_t maxMemoryUse);

// Implicitly calls ncEraseCache and then destroys the cache itself
void ncFreeCache(netCache* cache);

// Attempts to open a network namespace context with the given identifier. If a
// namespace with the given identifier was already opened, the corresponding
// context is retrieved from the cache. Otherwise, the namespace is opened with
// the specified name and stored in the cache. If the cache is full, an existing
// namespace is released and discarded from the cache. excl and err have the
// same meaning as for netOpenNamespace. The active namespace for the process is
// set to the given namespace.
netContext* ncOpenNamespace(netCache* cache, nodeId id, const char* name, bool create, bool excl, int* err);
