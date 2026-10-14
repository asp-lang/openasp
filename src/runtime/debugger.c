// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "debugger.h"

void oasp_runtime_set_debugger(oasp_runtime *runtime, const oasp_debugger *debugger) {
    if (runtime)
        runtime->debugger = debugger;
}

bool oasp_runtime_debug_instruction(oasp_runtime *runtime, const oasp_bytecode *bytecode, const oasp_instruction *instruction, const oasp_value *stack, size_t stack_depth,
                                    size_t pc) {
    if (!runtime || !bytecode || !instruction)
        return false;
    size_t frame_depth = 0;
    for (oasp_frame *frame = runtime->frame; frame; frame = frame->parent)
        frame_depth++;
    if (runtime->frame) {
        runtime->frame->debug_bytecode = bytecode;
        runtime->frame->debug_pc = pc;
    } else {
        runtime->debug_root_bytecode = bytecode;
        runtime->debug_root_pc = pc;
    }
    if (!runtime->debugger || !runtime->debugger->hook)
        return true;
    oasp_debug_event event = {
        .kind = OASP_DEBUG_INSTRUCTION,
        .runtime = runtime,
        .bytecode = bytecode,
        .instruction = instruction,
        .frame = runtime->frame,
        .stack = stack,
        .stack_depth = stack_depth,
        .pc = pc,
        .frame_depth = frame_depth,
        .source_name = runtime->debug_source_name,
        .source = runtime->debug_source,
    };
    bool keep_running = runtime->debugger->hook(runtime->debugger->context, &event);
    if (!keep_running)
        runtime->debugger_aborted = true;
    return keep_running;
}

void oasp_runtime_debug_error(oasp_runtime *runtime) {
    if (!runtime || !runtime->debugger || !runtime->debugger->hook)
        return;
    const oasp_bytecode *bytecode = runtime->frame ? runtime->frame->debug_bytecode
                                                   : runtime->debug_root_bytecode;
    size_t pc = runtime->frame ? runtime->frame->debug_pc : runtime->debug_root_pc;
    const oasp_instruction *instruction =
        bytecode ? oasp_bytecode_instruction_at(bytecode, pc) : NULL;
    if (!instruction)
        return;
    size_t frame_depth = 0;
    for (oasp_frame *frame = runtime->frame; frame; frame = frame->parent)
        frame_depth++;
    oasp_debug_event event = {
        .kind = OASP_DEBUG_ERROR,
        .runtime = runtime,
        .bytecode = bytecode,
        .instruction = instruction,
        .frame = runtime->frame,
        .pc = pc,
        .frame_depth = frame_depth,
        .source_name = runtime->debug_source_name,
        .source = runtime->debug_source,
    };
    if (!runtime->debugger->hook(runtime->debugger->context, &event)) {
        runtime->debugger_aborted = true;
        runtime->stopped = true;
    }
}

void oasp_runtime_debug_prepare_fork(oasp_runtime *runtime) {
    if (runtime && runtime->debugger && runtime->debugger->prepare_fork)
        runtime->debugger->prepare_fork(runtime->debugger->context, runtime);
}

void oasp_runtime_debug_forked(oasp_runtime *runtime, pid_t pid) {
    if (runtime && runtime->debugger && runtime->debugger->forked)
        runtime->debugger->forked(runtime->debugger->context, runtime, pid);
}
