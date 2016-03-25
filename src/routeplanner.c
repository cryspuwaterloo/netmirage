#include "routeplanner.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#include "mem.h"

// We use the standard unoptimized Floyd-Warshall algorithm (with edge
// reconstruction) for APSP. We accept a loss of precision by using floats
// instead of doubles; this reduces the memory requirements by half (due to
// padding).

// TODO: This is too slow for large networks. Use the optimized version.

typedef struct {
	float weight;
	nodeId next;
} edgeInfo;

struct routePlanner_s {
	edgeInfo* edges;
	nodeId nodeCount;
	nodeId* pathBuffer;
	size_t pathBufferCap;
};

static edgeInfo* rfEdgePtr(routePlanner* planner, nodeId from, nodeId to) {
	return &planner->edges[from * planner->nodeCount + to];
}

#include <stdio.h>
routePlanner* rfNewPlanner(nodeId nodeCount) {
	printf("%lu\n", sizeof(edgeInfo));
	routePlanner* planner = malloc(sizeof(routePlanner));
	planner->nodeCount = nodeCount;

	size_t cellCount;
	emul(nodeCount, nodeCount, &cellCount);
	planner->edges = eamalloc(cellCount, sizeof(edgeInfo), 0);

	for (nodeId i = 0; i < nodeCount; ++i) {
		for (nodeId j = 0; j < nodeCount; ++j) {
			edgeInfo* edge = rfEdgePtr(planner, i, j);
			edge->weight = INFINITY;
			edge->next = j;
		}
	}

	flexBufferInit((void**)&planner->pathBuffer, NULL, &planner->pathBufferCap);

	return planner;
}

void rfFreePlan(routePlanner* planner) {
	flexBufferFree((void**)&planner->pathBuffer, NULL, &planner->pathBufferCap);
	free(planner->edges);
	free(planner);
}

void rfSetWeight(routePlanner* planner, nodeId from, nodeId to, float weight) {
	rfEdgePtr(planner, from, to)->weight = weight;
}

void rfPlanRoutes(routePlanner* planner) {
	for (nodeId k = 0; k < planner->nodeCount; ++k) {
		for (nodeId i = 0; i < planner->nodeCount; ++i) {
			for (nodeId j = 0; j < planner->nodeCount; ++j) {
				edgeInfo* edge1 = rfEdgePtr(planner, i, k);
				edgeInfo* edge2 = rfEdgePtr(planner, k, j);
				edgeInfo* directEdge = rfEdgePtr(planner, i, j);
				float detourWeight = edge1->weight + edge2->weight;
				if (detourWeight < directEdge->weight) {
					directEdge->weight = detourWeight;
					directEdge->next = edge1->next;
				}
			}
		}
	}
}

static void rfAddStep(routePlanner* planner, size_t* steps, nodeId nextStep) {
	flexBufferGrow((void**)&planner->pathBuffer, *steps, &planner->pathBufferCap, 1, sizeof(nodeId));
	flexBufferAppend(planner->pathBuffer, steps, &nextStep, 1, sizeof(nodeId));
}

bool rfGetRoute(routePlanner* planner, nodeId start, nodeId end, nodeId** path, nodeId* steps) {
	*path = NULL;
	*steps = 0;

	if (rfEdgePtr(planner, start, end)->weight == INFINITY) return false;

	size_t longSteps = 0;
	rfAddStep(planner, &longSteps, start);

	while (start != end) {
		start = rfEdgePtr(planner, start, end)->next;
		rfAddStep(planner, &longSteps, start);
	}

	if (longSteps > MAX_NODE_ID) return false; // Should not be possible
	*steps = (nodeId)longSteps;
	*path = planner->pathBuffer;
	return true;
}
