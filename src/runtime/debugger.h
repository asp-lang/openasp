// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Optional VM debugger interface. The VM publishes borrowed, read-only
 * execution views immediately before dispatching an instruction. Callbacks may
 * inspect or update script state through the normal runtime APIs, but all
 * pointers in an event expire when the callback returns.
 */

#ifndef OPENASP_DEBUGGER_H
#define OPENASP_DEBUGGER_H

#include "bytecode.h"
#include "runtime.h"

typedef enum {
    OASP_DEBUG_INSTRUCTION,
    OASP_DEBUG_ERROR
} oasp_debug_event_kind;

typedef struct {
    oasp_debug_event_kind kind;
    oasp_runtime *runtime;
    const oasp_bytecode *bytecode;
    const oasp_instruction *instruction;
    const oasp_frame *frame;
    const oasp_value *stack;
    size_t stack_depth;
    size_t pc;
    size_t frame_depth;
    const char *source_name;
    oasp_slice source;
} oasp_debug_event;

typedef bool (*oasp_debug_hook_fn)(void *context, const oasp_debug_event *event);
typedef void (*oasp_debug_prepare_fork_fn)(void *context, oasp_runtime *runtime);
typedef void (*oasp_debug_forked_fn)(void *context, oasp_runtime *runtime, pid_t pid);

struct oasp_debugger {
    oasp_debug_hook_fn hook;
    oasp_debug_prepare_fork_fn prepare_fork;
    oasp_debug_forked_fn forked;
    void *context;
};

void oasp_runtime_set_debugger(oasp_runtime *runtime, const oasp_debugger *debugger);
bool oasp_runtime_debug_instruction(oasp_runtime *runtime, const oasp_bytecode *bytecode, const oasp_instruction *instruction, const oasp_value *stack, size_t stack_depth,
                                    size_t pc);
void oasp_runtime_debug_error(oasp_runtime *runtime);
void oasp_runtime_debug_prepare_fork(oasp_runtime *runtime);
void oasp_runtime_debug_forked(oasp_runtime *runtime, pid_t pid);

#endif
