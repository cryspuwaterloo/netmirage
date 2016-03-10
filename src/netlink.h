#pragma once

#include <stddef.h>
#include <stdint.h>

// This module defines functions for sending and receiving rtnetlink messages.

// Individual contexts are not thread-safe, but multiple threads may use their
// own contexts simultaneously.
typedef struct nlContext_s nlContext;

// Creates a new rtnetlink context. Returns NULL on error, in which case *err
// is set to the error code if it is provided.
nlContext* nlNewContext(int* err);

// Frees a context. Calls made after freeing the context yield undefined
// behavior.
int nlFreeContext(nlContext* ctx);

// Initiates the construction of a new request packet. The contents of the
// packet are modified by using the appending functions below. Once the contents
// have been added, the message can be sent to the kernel. Callers should
// specify NLM_F_ACK in the message flags if they intend to wait for an
// acknowledgment.
void nlInitMessage(nlContext* ctx, uint16_t msgType, uint16_t msgFlags);

void nlBufferAppend(nlContext* ctx, const void* buffer, size_t len);
int nlPushAttr(nlContext* ctx, unsigned short type);
int nlPopAttr(nlContext* ctx);

// Sends the message under construction to the kernel. If the message has the
// NLM_F_ACK flag, this function will block until confirmation is received. If
// no acknowledgment is requested, kernel errors are silently dropped. If this
// function returns an error, the message being constructed may be corrupted,
// so the caller must not try to send the message again without first
// reconstructing it.
int nlSendMessage(nlContext* ctx);
