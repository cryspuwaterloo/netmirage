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
#pragma once

#include <stdio.h>

#include "topology.h"

typedef struct {
	// An opaque string token identifying the node in the file's namespace. Its
	// encoding is undefined, and thus should not be shown to the user.
	const char* name;

	TopoNode t;
} GmlNode;

typedef struct {
	// Opaque string tokens denoting the start and end of the link
	const char* sourceName;
	const char* targetName;

	float weightUp;
	float weightDown;
	TopoLink t;
} GmlLink;

// Callback for when nodes are read from the GraphML file. Node is valid only
// for the duration of the call. Non-zero return values terminate parsing.
typedef int (*NewNodeFunc)(const GmlNode* node, void* userData);

// Callback for when links are read from the GraphML file. Link is valid only
// for the duration of the call. Non-zero return values terminate parsing.
typedef int (*NewLinkFunc)(const GmlLink* link, void* userData);

// Parses a GraphML file from a stream. Returns 0 for success.
int gmlParse(FILE* input, NewNodeFunc newNode, NewLinkFunc newLink, void* userData, const char* clientType, const char* weightKey);

// Parses a GraphML file stored on the disk. Returns 0 for success.
int gmlParseFile(const char* filename, NewNodeFunc newNode, NewLinkFunc newLink, void* userData, const char* clientType, const char* weightKey);

// Parses a GraphML file stored in memory. Returns 0 for success.
int gmlParseMemory(char* buffer, int size, NewNodeFunc newNode, NewLinkFunc newLink, void* userData, const char* clientType, const char* weightKey);
