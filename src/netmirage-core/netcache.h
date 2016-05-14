/*******************************************************************************
 * Copyright © 2016 Nik Unger, Ian Goldberg, Qatar University, and the Qatar
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
