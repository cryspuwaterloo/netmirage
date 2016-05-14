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

// Common definitions of logical network topology structures

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t nodeId;
#define MAX_NODE_ID     (UINT32_MAX-1)
#define INVALID_NODE_ID (UINT32_MAX)
// Maximum number of chars required for a decimal representation, with
// terminator:
#define MAX_NODE_ID_BUFLEN (11)

// A node represents a client or AS in a network graph
typedef struct {
	bool client;
	double packetLoss;
	double bandwidthUp;
	double bandwidthDown;
} TopoNode;

// A link represents a network connection between nodes
typedef struct {
	double latency;
	double packetLoss;
	double jitter;
	uint32_t queueLen;
} TopoLink;
