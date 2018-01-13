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
// This implementation stores a hash map from namespace identifiers (32-bit
// ints) to nodes of a linked list. The linked list orders nodes based on their
// creation time. Each node embeds a netContext. The oldest nodes are evicted
// when the map runs out of space. The logic here is that, in our normal use
// case, nodes are requested in relatively short "bursts". The cache helps, but
// a more advanced eviction scheme would sacrifice cache coherence for little
// gain.

#include "netcache.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <glib.h>

#include "mem.h"
#include "net.h"
#include "netlink.h"

#include "net.inl"

static const uint64_t MIN_ENTRIES = 100;

typedef struct ncNode ncNode;
struct ncNode {
	gpointer key;
	netContext ctx;
	ncNode* newer;
};

struct netCache {
	uint64_t maxEntries;
	ncNode* oldest;
	ncNode* newest;
	GHashTable* map;
};

static gpointer ncMakeKey(nodeId id) {
	return GINT_TO_POINTER(g_int_hash(&id));
}

netCache* ncNewCache(uint64_t maxMemoryUse) {
	netCache* cache = emalloc(sizeof(netCache));
	const uint64_t nodeMemoryFudgeFactor = 140; // Rough estimate of glib overhead
	cache->maxEntries = (uint64_t)((double)maxMemoryUse / (double)(sizeof(ncNode) + nodeMemoryFudgeFactor));
	if (cache->maxEntries < MIN_ENTRIES) cache->maxEntries = MIN_ENTRIES;
	cache->map = g_hash_table_new(&g_direct_hash, &g_direct_equal);
	cache->oldest = NULL;
	cache->newest = NULL;
	return cache;
}

void ncFreeCache(netCache* cache) {
	GHashTableIter it;
	g_hash_table_iter_init(&it, cache->map);

	gpointer val;
	while (g_hash_table_iter_next(&it, NULL, &val)) {
		ncNode* node = val;
		netCloseNamespace(&node->ctx, true);
		free(node);
	}

	g_hash_table_destroy(cache->map);
	free(cache);
}

netContext* ncOpenNamespace(netCache* cache, nodeId id, const char* name, bool create, bool excl, int* err) {
	gpointer key = ncMakeKey(id);
	gpointer val = g_hash_table_lookup(cache->map, key);
	if (val != NULL) { // Found in hash table
		ncNode* node = val;
		int res = netSwitchNamespace(&node->ctx);
		if (res != 0) {
			if (err != NULL) *err = res;
			return NULL;
		}
		return &node->ctx;
	}

	ncNode* node;
	bool reusing;
	if ((uint64_t)g_hash_table_size(cache->map) < cache->maxEntries) {
		node = emalloc(sizeof(ncNode));
		reusing = false;
	} else {
		node = cache->oldest;
		cache->oldest = node->newer;
		g_hash_table_remove(cache->map, node->key);
		netInvalidateContext(&node->ctx);
		reusing = true;
	}
	node->key = key;
	node->newer = NULL;

	int res = netOpenNamespaceInPlace(&node->ctx, reusing, name, create, excl);
	if (res != 0) {
		free(node);
		if (err != NULL) *err = res;
		return NULL;
	}

	if (cache->newest != NULL) cache->newest->newer = node;
	cache->newest = node;
	if (cache->oldest == NULL) cache->oldest = node;
	g_hash_table_insert(cache->map, key, node);
	return &node->ctx;
}
