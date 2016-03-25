#pragma once

// This module implements an all-pairs shortest path algorithm for computing
// static routing for a network graph.

#include <stdbool.h>
#include <stdlib.h>

#include "topology.h"

typedef struct routePlanner_s routePlanner;

// Creates a new route planner for nodeCount nodes. Initially, all edges in the
// graph are untraversable. Returns NULL if an error occurred.
routePlanner* rfNewPlanner(nodeId nodeCount);

// Releases all resources associated with a route planner.
void rfFreePlan(routePlanner* planner);

// Sets the link weight between two nodes.
void rfSetWeight(routePlanner* planner, nodeId from, nodeId to, float weight);

// Discovers the shortest routes between all nodes in the graph. If new edge
// weights are set after planning the routes, this function must be called again
// before requesting shortest paths. Returns 0 on success or an error code
// otherwise.
int rfPlanRoutes(routePlanner* planner);

// Finds the shortest route from a starting node to an ending node. Must be
// called after rfPlanRoutes. If no path exists, the function returns false.
// Otherwise, it returns true, "path" points to an array of node indices
// beginning with "start" and ending with "end", and "steps" is set to the
// number of array elements. This array is invalidated by a subsequent call to
// rfGetRoute or rfFreePlan.
bool rfGetRoute(routePlanner* planner, nodeId start, nodeId end, nodeId** path, nodeId* steps);
