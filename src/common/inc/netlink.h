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

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// This module defines functions for sending and receiving rtnetlink messages.
// This module is not thread-safe.

typedef struct nlContext nlContext;

// Initializes the netlink subsystem
void nlInit(void);

// Frees all resources associated with the netlink subsystem
void nlCleanup(void);

// Creates a new rtnetlink context. Returns NULL on error, in which case *err
// is set to the error code if it is provided. The netlink context implicitly
// operates in the namespaces that were active at the time of creation. This
// cannot be changed.
nlContext* nlNewContext(int* err);

// Creates a new rtnetlink context using existing storage space. If reusing is
// false, then the space is completely initialized. If reusing is true, then the
// context must have been previously created with nlNewContext and subsequently
// invalidated with nlInvalidateContext. Returns 0 on success or an error code
// otherwise.
int nlNewContextInPlace(nlContext* ctx);

// Invalidates a context so that its memory can be reused to create a new one.
void nlInvalidateContext(nlContext* ctx);

// Frees a context. Calls made after freeing the context yield undefined
// behavior.
void nlFreeContext(nlContext* ctx, bool inPlace);

// Initiates the construction of a new request packet. The contents of the
// packet are modified by using the appending functions below. Once the contents
// have been added, the message can be sent to the kernel. Callers should
// specify NLM_F_ACK in the message flags if they intend to wait for an
// acknowledgment. Any response message in the context is discarded by calling
// this function. All contexts share a message buffer, and so message
// construction cannot be interleaved between contexts.
void nlInitMessage(nlContext* ctx, uint16_t msgType, uint16_t msgFlags);

void nlBufferAppend(nlContext* ctx, const void* buffer, size_t len);
int nlPushAttr(nlContext* ctx, unsigned short type);
int nlPopAttr(nlContext* ctx);

// Handler function for responses from the kernel. Some requests produce
// multiple responses. If a non-zero result is returned, the error is passed to
// the caller of the send function.
typedef int (*nlResponseHandler)(const nlContext* ctx, const void* data, uint32_t len, uint16_t type, uint16_t flags, void* arg);

// Sends the message under construction to the kernel. If waitResponse is set to
// true, then this function will block until confirmation is received. If no
// acknowledgment is requested, kernel errors are silently dropped. The message
// being constructed is discarded by calling this function, so the caller should
// not attempt to re-send it. If waitResponse is true and handler is non-NULL,
// then the handler is called for each response message from the kernel. The
// handler can use the subsequent functions to process the data in the response
// message. arg is passed to the handler.
int nlSendMessage(nlContext* ctx, bool waitResponse, nlResponseHandler handler, void* arg);
