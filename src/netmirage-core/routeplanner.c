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
#include "routeplanner.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#include <glib.h>

#include "log.h"
#include "mem.h"

/* We use the Floyd-Warshall algorithm (with edge reconstruction) for APSP. We
 * accept a loss of precision by using floats instead of doubles; this reduces
 * the memory requirements by half (due to padding).
 *
 * This implementation is highly optimized, and thus sacrifices some simplicity
 * and readability. Specifically, the implementation is optimized to improve
 * cache performance and to enable multithreaded computation. These
 * optimizations are necessary in order to compute large-scale static routing
 * tables, since the all-pairs shortest path problem for n nodes is O(n^3), and
 * we intend to support at least tens of thousands of nodes.
 *
 * Observations about realistic input:
 * - The graphs are highly connected (at least 60%). Repeated application of
 *   Dijkstra's algorithm is too slow.
 * - Cache performance is a major concern. After optimizing, we reduced CPU time
 *   by nearly 40%.
 *
 * The core of this implementation follows the approach described by
 * Venkataraman, Sahni, and Mukhopadhyaya in "A Blocked All-Pairs Shortest-Paths
 * Algorithm" (Journal of Experimental Algorithmics, 2003). This approach uses
 * loop tiling in order to improve cache performance. We generally follow the
 * algorithm presented in Figure 6, with minor deviations.
 *
 * As part of the approach, we use the following terminology to describe aspects
 * of the Floyd-Warshall adjacency matrix:
 * - "Cell": data for a single edge. It stores the weight and the next node.
 * - "Block": a square region of cells. Every block has dimensions B x B, where
 *   B is BlockSize.
 * - "Chunk": a rectangular region of blocks. Chunks always contain an integral
 *   number of blocks.
 *
 * The blocks are processed as described by Venkataraman et al. Each "phase" of
 * processing, as described in the original paper, is performed as a chunk
 * processing operation. Since blocks within a chunk (and more generally, a
 * whole phase), are independent, we can process them in parallel. We divide
 * chunks into work units that are submitted to a thread pool for sufficiently
 * large chunks.
 *
 * One final optimization that we employ is using a custom memory storage order.
 * Rather than storing cells in row-major cell order, we store them in row-major
 * block order, and row-major cell order within the blocks. For example, if the
 * block size was 2 and the node count was 6, then the following adjacency
 * matrix lists the array indices corresponding to the cell storage:
 *
 *                Cell storage:                  Block indices:
 *            1  2  | 5  6  | 9  10           (0,0) | (0,1) | (0,2)
 *            3  4  | 7  8  | 11 12                 |       |
 *           -----------------------         -----------------------
 *            13 14 | 17 18 | 21 22           (1,0) | (1,1) | (1,2)
 *            15 16 | 19 20 | 23 24                 |       |
 *           -----------------------         -----------------------
 *            25 26 | 29 30 | 33 34           (2,0) | (2,1) | (2,2)
 *            27 28 | 31 32 | 35 36                 |       |
 * This pattern helps to improve cache performance as the matrix is processed.
 * We refer to this layout as "block order".
 *
 * This implementation is not perfect. There are several known techniques for
 * improving its performance, should that prove necessary:
 * 1) Use SIMD instructions for the core Floyd-Warshall comparison step. This
 *    likely means SSE or AVX intrinsics. This approach would necessitate
 *    storing the weights and "next" identifiers for multiple cells
 *    sequentially (e.g., struct { float weights[8]; nodeId nexts[8]; }; ). This
 *    would complicate cell storage further, but would likely improve
 *    performance.
 * 2) Use hierarchical tiling and ZMorton storage order, such as described by
 *    Park, Penner, and Prasanna in "Optimizing Graph Algorithms for Improved
 *    Cache Performance".
 */

typedef struct {
	float weight;
	nodeId next;
} edgeInfo;

typedef struct {
	edgeInfo* edges;
	nodeId blockRowSize;
	nodeId rangeRows;
	nodeId rangeCols;
	nodeId ijBlock;
	nodeId ikBlock;
	nodeId kjBlock;

	GMutex* todoLock;
	GCond* finished;
	nodeId todoCount;
} rpWorkRange;

typedef struct {
	rpWorkRange* range;
	nodeId startIndex;
} rpWorkUnit;

struct routePlanner {
	edgeInfo* edges;
	nodeId nodeCount;

	nodeId* pathBuffer;
	size_t pathBufferCap;

	GThreadPool* pool;
	rpWorkUnit* units;
	size_t unitsCap;

	GMutex todoLock;
	GCond finished;
};

// These values were empirically selected with guidance from the literature
static const nodeId BlockSize = 16;
static const nodeId BlockArea = 16 * 16;
static const nodeId ThreadedThresholdNodes = 1024;
static const nodeId ThreadWorkSize = 8;

static edgeInfo* rpEdgePtr(routePlanner* planner, nodeId from, nodeId to) {
	nodeId fromBlock = from / BlockSize;
	nodeId toBlock = to / BlockSize;
	nodeId row = from % BlockSize;
	nodeId col = to % BlockSize;
	nodeId blockRowSize = planner->nodeCount * BlockSize;
	size_t index = (fromBlock * blockRowSize) + (toBlock * BlockArea) + (row * BlockSize) + col;
	return &planner->edges[index];
}

routePlanner* rpNewPlanner(nodeId nodeCount) {
	lprintf(LogDebug, "Created a new route planner for %u nodes\n", nodeCount);

	/* We force the number of nodes to be a multiple of the block size. This
	 * trades memory for performance.
	 * Disadvantages:
	 * - We waste memory. In the worst case, we lose:
	 *   (2*nodeCount - BlockSize + 1) * (BlockSize -1) * 8 bytes
	 * - O(nodeCount) additional operations required when pathfinding
	 * - Less cache reuse between the end of a row and the start of the next
	 * Advantages:
	 * - O(nodeCount^2) fewer special-case tests (with good branch prediction)
	 * - We use O(nodeCount^2) space and O(nodeCount^3) time, so the
	 *   disadvantages are negligible
	 */
	nodeId blocks = (nodeCount + BlockSize - 1) / BlockSize;
	nodeCount = blocks * BlockSize;
	lprintf(LogDebug, "Node count was set to %u for block alignment\n", nodeCount);

	routePlanner* planner = malloc(sizeof(routePlanner));
	planner->nodeCount = nodeCount;

	nodeId cellCount;
	emul32(nodeCount, nodeCount, &cellCount);
	planner->edges = eamalloc(cellCount, sizeof(edgeInfo), 0);

	// Set initial weights and "next" identifiers. We traverse the edges in
	// array order, which makes it somewhat difficult to efficiently compute the
	// global column numbers.
	edgeInfo* edge = planner->edges;
	for (nodeId blockRow = 0; blockRow < blocks; ++blockRow) {
		nodeId colOffset = 0;
		for (nodeId blockCol = 0; blockCol < blocks; ++blockCol) {
			for (nodeId row = 0; row < BlockSize; ++row) {
				for (nodeId col = 0; col < BlockSize; ++col) {
					edge->weight = INFINITY;
					edge->next = colOffset + col;
					++edge;
				}
			}
			colOffset += BlockSize;
		}
	}

	flexBufferInit((void**)&planner->pathBuffer, NULL, &planner->pathBufferCap);
	flexBufferInit((void**)&planner->units, NULL, &planner->unitsCap);

	return planner;
}

void rpFreePlan(routePlanner* planner) {
	lprintln(LogDebug, "Releasing route planner resources");
	flexBufferFree((void**)&planner->units, NULL, &planner->unitsCap);
	flexBufferFree((void**)&planner->pathBuffer, NULL, &planner->pathBufferCap);
	free(planner->edges);
	free(planner);
}

void rpSetWeight(routePlanner* planner, nodeId from, nodeId to, float weight) {
	lprintf(LogDebug, "Route weight for %u => %u set to %f\n", from, to, weight);
	rpEdgePtr(planner, from, to)->weight = weight;
}

static void rpAddStep(routePlanner* planner, size_t* steps, nodeId nextStep) {
	flexBufferGrow((void**)&planner->pathBuffer, *steps, &planner->pathBufferCap, 1, sizeof(nodeId));
	flexBufferAppend(planner->pathBuffer, steps, &nextStep, 1, sizeof(nodeId));
}

bool rpGetRoute(routePlanner* planner, nodeId start, nodeId end, nodeId** path, nodeId* steps) {
	// This is the basic Floyd-Warshall path reconstruction technique. The only
	// complication is using rpEdgePtr to access the edges, since they are
	// stored in block layout.

	*path = NULL;
	*steps = 0;

	float pathWeight = rpEdgePtr(planner, start, end)->weight;
	if (pathWeight == INFINITY) {
		lprintf(LogDebug, "No route exists from %u => %u\n", start, end);
		return false;
	}

	size_t longSteps = 0;
	rpAddStep(planner, &longSteps, start);

	nodeId next = start;
	while (next != end) {
		next = rpEdgePtr(planner, next, end)->next;
		rpAddStep(planner, &longSteps, next);
	}

	if (longSteps > MAX_NODE_ID) {
		lprintf(LogError, "BUG: Route length %lu is longer than node count!\n", longSteps);
		return false;
	}
	*steps = (nodeId)longSteps;
	*path = planner->pathBuffer;
	lprintf(LogDebug, "Route from %u => %u has weight %f with %u hops\n", start, end, pathWeight, *steps);
	return true;
}

// Completely process a single block of cells in the current thread
static inline void rpProcessBlock(edgeInfo* edges, nodeId ijBlockStart, nodeId ikBlockStart, nodeId kjBlockStart) {
	for (nodeId k = 0; k < BlockSize; ++k) {
		edgeInfo* ijEdge = &edges[ijBlockStart];
		edgeInfo* ikEdge = &edges[ikBlockStart];
		for (nodeId i = 0; i < BlockSize; ++i) {
			edgeInfo* kjEdge = &edges[kjBlockStart];
			for (nodeId j = 0; j < BlockSize; ++j) {
				float detourWeight = ikEdge->weight + kjEdge->weight;
				if (detourWeight < ijEdge->weight) {
					ijEdge->weight = detourWeight;
					ijEdge->next = ikEdge->next;
				}

				++kjEdge;
				++ijEdge;
			}
			ikEdge += BlockSize;
		}
		++ikBlockStart; // Variable reuse; no longer points to block start
		kjBlockStart += BlockSize;
	}
}

// A pointer to a function that processes a chunk of blocks. We use a pointer so
// that we can easily swap between implementations at runtime based on the
// characteristics of the graph.
typedef void (*rpProcessChunkFunc)(routePlanner* planner, nodeId blockRowSize, nodeId rangeRows, nodeId rangeCols, nodeId ijBlock, nodeId ikBlock, nodeId kjBlock);

// Processes a chunk of blocks in a single thread. This is the most basic
// implementation: simply enumerate the blocks and process each one locally.
static void rpProcessChunkLocal(routePlanner* planner, nodeId blockRowSize, nodeId rangeRows, nodeId rangeCols, nodeId ijBlock, nodeId ikBlock, nodeId kjBlock) {
	edgeInfo* edges = planner->edges;
	for (nodeId row = 0; row < rangeRows; ++row) {
		nodeId ij = ijBlock;
		nodeId kj = kjBlock;
		for (nodeId col = 0; col < rangeCols; ++col) {
			rpProcessBlock(edges, ij, ikBlock, kj);
			ij += BlockArea;
			kj += BlockArea;
		}
		ijBlock += blockRowSize;
		ikBlock += blockRowSize;
	}
}

// This function is the same as rpProcessChunkLocal, but allows the procedure to
// begin in the middle of the procedure. The function will act as if the
// innermost loop has already been processed startIndex times, and will continue
// for ThreadWorkSize steps.
static void rpProcessPartialChunk(edgeInfo* edges, nodeId blockRowSize, nodeId rangeRows, nodeId rangeCols, nodeId ijBlock, nodeId ikBlock, nodeId kjBlock, nodeId startIndex) {
	nodeId row = startIndex / rangeCols;
	nodeId col = startIndex % rangeCols;
	nodeId rowSkip = blockRowSize * row;
	nodeId colSkip = BlockArea * col;
	ijBlock += rowSkip;
	ikBlock += rowSkip;
	nodeId ij = ijBlock + colSkip;
	nodeId kj = kjBlock + colSkip;
	for (nodeId i = 0; i < ThreadWorkSize; ++i) {
		rpProcessBlock(edges, ij, ikBlock, kj);
		ij += BlockArea;
		kj += BlockArea;

		if (++col >= rangeCols) {
			col = 0;
			ijBlock += blockRowSize;
			ikBlock += blockRowSize;
			ij = ijBlock;
			kj = kjBlock;
			if (++row >= rangeRows) return;
		}
	}
}

// Callback for the thread pool. Processes the chunk identified by the work
// unit. If no more work is queued, the finished signal is raised.
static void rpPoolCallback(gpointer data, gpointer user_data) {
	rpWorkUnit* unit = data;
	rpWorkRange* range = unit->range;
	rpProcessPartialChunk(range->edges, range->blockRowSize, range->rangeRows, range->rangeCols, range->ijBlock, range->ikBlock, range->kjBlock, unit->startIndex);
	g_mutex_lock(range->todoLock);
	if (--range->todoCount == 0) {
		g_cond_signal(range->finished);
	}
	g_mutex_unlock(range->todoLock);
}

// Processes a chunk of blocks using a thread pool
static void rpProcessChunkThreaded(routePlanner* planner, nodeId blockRowSize, nodeId rangeRows, nodeId rangeCols, nodeId ijBlock, nodeId ikBlock, nodeId kjBlock) {
	nodeId spaceSize = rangeRows * rangeCols;
	if (spaceSize <= ThreadWorkSize) {
		// The area is too small to justify thread pool overhead
		if (spaceSize > 0) {
			rpProcessChunkLocal(planner, blockRowSize, rangeRows, rangeCols, ijBlock, ikBlock, kjBlock);
		}
		return;
	}

	// Copy starting parameters; available to all threads
	rpWorkRange range;
	range.edges = planner->edges;
	range.todoLock = &planner->todoLock;
	range.finished = &planner->finished;
	range.blockRowSize = blockRowSize;
	range.rangeRows = rangeRows;
	range.rangeCols = rangeCols;
	range.ijBlock = ijBlock;
	range.ikBlock = ikBlock;
	range.kjBlock = kjBlock;

	nodeId tasks = (spaceSize + ThreadWorkSize - 1) / ThreadWorkSize;
	range.todoCount = tasks;

	flexBufferGrow((void**)&planner->units, 0, &planner->unitsCap, (size_t)tasks, sizeof(rpWorkUnit));

	g_mutex_lock(range.todoLock);

	nodeId loopIdx = 0;
	for (nodeId i = 0; i < tasks; ++i, loopIdx += ThreadWorkSize) {
		rpWorkUnit* unit = &planner->units[i];
		unit->range = &range;
		unit->startIndex = loopIdx;
		g_thread_pool_push(planner->pool, unit, NULL);
	}
	while (range.todoCount > 0) {
		g_cond_wait(range.finished, range.todoLock);
	}
	g_mutex_unlock(range.todoLock);
}

int rpPlanRoutes(routePlanner* planner) {
	bool singleThreaded = planner->nodeCount < ThreadedThresholdNodes;

	lprintf(LogInfo, "Constructing routing table for %u nodes (%s)\n", planner->nodeCount, singleThreaded ? "single-threaded" : "multi-threaded");

	rpProcessChunkFunc processRange;
	if (singleThreaded) {
		processRange = &rpProcessChunkLocal;
	} else {
		processRange = &rpProcessChunkThreaded;

		// Initialize the thread pool
		gint threads = (gint)g_get_num_processors();
		lprintf(LogDebug, "Using %d threads for Floyd-Warshall\n", threads);
		GError* err = NULL;
		planner->pool = g_thread_pool_new(&rpPoolCallback, NULL, threads, TRUE, &err);
		if (planner->pool == NULL) {
			lprintf(LogError, "Failed to create thread pool for planning routes. Only direct routes will be available. Error: %s\n", err->message);
			int code = err->code;
			g_error_free(err);
			return code;
		}

		g_mutex_init(&planner->todoLock);
		g_cond_init(&planner->finished);
	}

	// Number of blocks per side of the cube
	nodeId blocks = planner->nodeCount / BlockSize;

	// The number of cells in a complete row of blocks
	nodeId blockRowSize = planner->nodeCount * BlockSize;

	// Number of cells between block (i,i) and block (i+1,i+1)
	nodeId blockDiagonalSize = blockRowSize + BlockArea;

	nodeId blockRowStart = 0; // Offset to (round, 0)
	nodeId nextBlockRow = 0;  // Offset to (round+1, 0)

	nodeId blockColStart = 0; // Offset to (0, round)
	nodeId nextBlockCol = 0;  // Offset to (0, round+1)

	nodeId sdbStart = 0;             // Offset to (round, round), self-dependent
	nodeId rightBlock = BlockArea;   // Offset to (round, round+1)
	nodeId downBlock = blockRowSize; // Offset to (round+1, round)

	nodeId remainingRounds = blocks - 1; // blocks - (round+1)

	// The details of this loop, including the variables, their update order,
	// and the sequence of instructions, has been highly optimized through
	// extensive testing with GCC. Before making any changes (even seemingly
	// insignificant ones), be sure to carefully benchmark the performance.
	for (nodeId round = 0; round < blocks; ++round) {
		blockRowStart = nextBlockRow;
		nextBlockRow += blockRowSize;

		blockColStart = nextBlockCol;
		nextBlockCol += BlockArea;

		// Phase 1: process SDB
		processRange(planner, blockRowSize, 1, 1, sdbStart, sdbStart, sdbStart);

		// We do not follow the order given in Figure 6 of the source paper. The
		// order given below maximizes cache performance (verified empirically).

		// Phase 2: above, left, right, below
		processRange(planner, blockRowSize, round, 1, blockColStart, blockColStart, sdbStart);
		processRange(planner, blockRowSize, 1, round, blockRowStart, sdbStart, blockRowStart);
		processRange(planner, blockRowSize, 1, remainingRounds, rightBlock, sdbStart, rightBlock);
		processRange(planner, blockRowSize, remainingRounds, 1, downBlock, downBlock, sdbStart);

		// Phase 3: above left, above right, below left, below right
		processRange(planner, blockRowSize, round, round, 0, blockColStart, blockRowStart);
		processRange(planner, blockRowSize, round, remainingRounds, nextBlockCol, blockColStart, rightBlock);
		processRange(planner, blockRowSize, remainingRounds, round, nextBlockRow, downBlock, blockRowStart);
		processRange(planner, blockRowSize, remainingRounds, remainingRounds, nextBlockRow + nextBlockCol, downBlock, rightBlock);

		// Move to next diagonal
		sdbStart += blockDiagonalSize;
		rightBlock += blockDiagonalSize;
		downBlock += blockDiagonalSize;
		--remainingRounds;
	}

	if (!singleThreaded) {
		g_mutex_clear(&planner->todoLock);
		g_cond_clear(&planner->finished);
		g_thread_pool_free(planner->pool, FALSE, TRUE);
	}
	return 0;
}
