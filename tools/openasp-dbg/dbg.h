// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_DBG_H
#define OPENASP_DBG_H

#include "debugger.h"
#include "parser.h"
#include "value.h"

#include <stdio.h>
#include <sys/types.h>

#define OASP_DBG_MAX_BREAKPOINTS 128
#define OASP_DBG_MAX_WATCHES 32

typedef enum {
    OASP_DBG_STOP,
    OASP_DBG_CONTINUE,
    OASP_DBG_STEP,
    OASP_DBG_NEXT,
    OASP_DBG_FINISH,
    OASP_DBG_STEPI,
    OASP_DBG_NEXTI,
    OASP_DBG_UNTIL
} oasp_dbg_mode;

typedef enum {
    OASP_DBG_FOLLOW_PARENT,
    OASP_DBG_FOLLOW_CHILD
} oasp_dbg_follow_fork_mode;

typedef struct {
    unsigned id;
    bool enabled;
    bool temporary;
    char *file;
    char *function;
    char *condition;
    size_t line;
    size_t ignore_count;
    size_t hit_count;
} oasp_dbg_breakpoint;

typedef struct {
    unsigned id;
    char *name;
    char *previous;
    bool initialized;
} oasp_dbg_watch;

typedef struct {
    oasp_debugger interface;
    FILE *input;
    FILE *output;
    int command_pipe[2];
    oasp_dbg_breakpoint breakpoints[OASP_DBG_MAX_BREAKPOINTS];
    size_t breakpoint_count;
    oasp_dbg_watch watches[OASP_DBG_MAX_WATCHES];
    size_t watch_count;
    unsigned next_id;
    oasp_dbg_mode mode;
    size_t origin_depth;
    size_t origin_line;
    size_t origin_pc;
    const oasp_bytecode *origin_bytecode;
    size_t selected_frame;
    bool initial_stop;
    bool quit;
    bool attached;
    bool detach_on_fork;
    bool related_attached;
    bool catch_errors;
    bool command_error;
    oasp_dbg_follow_fork_mode follow_fork_mode;
    pid_t related_pid;
    uint64_t last_error_generation;
    char *last_command;
    const oasp_debug_event *event;
} oasp_dbg;

void oasp_dbg_init(oasp_dbg *debugger, FILE *input, FILE *output);
void oasp_dbg_dispose(oasp_dbg *debugger);
bool oasp_dbg_hook(void *context, const oasp_debug_event *event);
void oasp_dbg_prepare_fork(void *context, oasp_runtime *runtime);
void oasp_dbg_forked(void *context, oasp_runtime *runtime, pid_t pid);

bool oasp_dbg_execute_command(oasp_dbg *debugger, char *line);
bool oasp_dbg_eval(oasp_dbg *debugger, const char *expression, oasp_value *result);
const oasp_frame *oasp_dbg_frame_at(const oasp_debug_event *event, size_t index);

oasp_dbg_breakpoint *oasp_dbg_add_breakpoint(oasp_dbg *debugger, const char *location, bool temporary);
bool oasp_dbg_delete_breakpoint(oasp_dbg *debugger, unsigned id);
oasp_dbg_breakpoint *oasp_dbg_find_breakpoint(oasp_dbg *debugger, unsigned id);
oasp_dbg_breakpoint *oasp_dbg_breakpoint_hit(oasp_dbg *debugger, const oasp_debug_event *event);
void oasp_dbg_print_breakpoints(const oasp_dbg *debugger);

bool oasp_dbg_source_line(oasp_slice source, size_t line, oasp_slice *result);
void oasp_dbg_print_location(const oasp_dbg *debugger, const oasp_debug_event *event);
void oasp_dbg_list_source(const oasp_dbg *debugger, const oasp_debug_event *event, size_t center, size_t radius);

void oasp_dbg_print_value(FILE *output, const oasp_value *value);
void oasp_dbg_print_instruction(FILE *output, const oasp_bytecode *bytecode, size_t pc);
void oasp_dbg_disassemble(FILE *output, const oasp_bytecode *bytecode, size_t start, size_t limit);

#endif
