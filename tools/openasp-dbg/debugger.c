// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#define _POSIX_C_SOURCE 200809L

#include "dbg.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const oasp_frame *oasp_dbg_frame_at(const oasp_debug_event *event, size_t index) {
    const oasp_frame *frame = event ? event->frame : NULL;
    while (frame && index--)
        frame = frame->parent;
    return frame;
}

static oasp_frame *oasp_dbg_selected_frame(oasp_dbg *debugger) {
    return (oasp_frame *)oasp_dbg_frame_at(debugger->event, debugger->selected_frame);
}

bool oasp_dbg_eval(oasp_dbg *debugger, const char *expression, oasp_value *result) {
    if (!debugger || !debugger->event || !expression || !*expression || !result)
        return false;
    oasp_arena *arena = oasp_arena_create(4096);
    if (!arena)
        return false;
    oasp_parser parser;
    bool initialized = oasp_parser_init(&parser, arena, expression, strlen(expression));
    oasp_expr *expr = initialized ? oasp_parse_expression(&parser) : NULL;
    const oasp_token *token = initialized ? oasp_parser_current(&parser) : NULL;
    bool valid = expr && token && token->kind == OASP_TOKEN_EOF;
    if (!valid) {
        fprintf(debugger->output, "Expression error: %s\n",
                initialized && parser.error ? parser.error : "invalid expression");
    } else {
        oasp_runtime *runtime = debugger->event->runtime;
        oasp_frame *saved = runtime->frame;
        int error_number = runtime->error_number;
        const char *error_description = runtime->error_description;
        const char *error_source = runtime->error_source;
        size_t error_line = runtime->error_line;
        uint64_t error_generation = runtime->error_generation;
        bool error_pending = runtime->error_pending;
        const oasp_debugger *runtime_debugger = runtime->debugger;
        runtime->frame = oasp_dbg_selected_frame(debugger);
        runtime->debugger = NULL;
        *result = oasp_runtime_eval(runtime, expr);
        runtime->debugger = runtime_debugger;
        runtime->frame = saved;
        valid = runtime->error_generation == error_generation;
        if (valid && (result->kind == OASP_VALUE_STRING || result->kind == OASP_VALUE_DATE))
            *result = oasp_runtime_value_copy(runtime, result, true);
        valid = valid && runtime->error_generation == error_generation;
        if (!valid)
            fprintf(debugger->output, "Evaluation error: %s\n",
                    runtime->error_description ? runtime->error_description : "runtime error");
        runtime->error_number = error_number;
        runtime->error_description = error_description;
        runtime->error_source = error_source;
        runtime->error_line = error_line;
        runtime->error_generation = error_generation;
        runtime->error_pending = error_pending;
    }
    if (initialized)
        oasp_parser_dispose(&parser);
    oasp_arena_destroy(arena);
    return valid;
}

static char *oasp_dbg_value_text(const oasp_value *value) {
    char *text = NULL;
    size_t length = 0;
    FILE *stream = open_memstream(&text, &length);
    if (!stream)
        return NULL;
    oasp_dbg_print_value(stream, value);
    if (fclose(stream)) {
        free(text);
        return NULL;
    }
    return text;
}

static bool oasp_dbg_watch_hit(oasp_dbg *debugger) {
    for (size_t i = 0; i < debugger->watch_count; i++) {
        oasp_value value;
        if (!oasp_dbg_eval(debugger, debugger->watches[i].name, &value))
            continue;
        char *current = oasp_dbg_value_text(&value);
        if (!current)
            continue;
        oasp_dbg_watch *watch = &debugger->watches[i];
        bool changed = watch->initialized && strcmp(watch->previous, current);
        if (changed) {
            fprintf(debugger->output, "Watchpoint %u: %s\nOld value = %s\nNew value = %s\n",
                    watch->id, watch->name, watch->previous, current);
        }
        free(watch->previous);
        watch->previous = current;
        watch->initialized = true;
        if (changed)
            return true;
    }
    return false;
}

static bool oasp_dbg_mode_stops(const oasp_dbg *debugger, const oasp_debug_event *event) {
    bool pc_changed = event->bytecode != debugger->origin_bytecode ||
                      event->pc != debugger->origin_pc;
    bool line_changed = event->instruction->line != debugger->origin_line;
    switch (debugger->mode) {
    case OASP_DBG_STOP:
        return true;
    case OASP_DBG_CONTINUE:
        return false;
    case OASP_DBG_STEP:
        return pc_changed && (line_changed || event->frame_depth != debugger->origin_depth);
    case OASP_DBG_NEXT:
        return pc_changed && event->frame_depth <= debugger->origin_depth && line_changed;
    case OASP_DBG_FINISH:
        return event->frame_depth < debugger->origin_depth;
    case OASP_DBG_STEPI:
        return pc_changed;
    case OASP_DBG_NEXTI:
        return pc_changed && event->frame_depth <= debugger->origin_depth;
    case OASP_DBG_UNTIL:
        return event->frame_depth < debugger->origin_depth ||
               (event->frame_depth == debugger->origin_depth &&
                event->instruction->line > debugger->origin_line);
    }
    return false;
}

static bool oasp_dbg_acquire_commands(oasp_dbg *debugger) {
    if (debugger->command_pipe[0] < 0)
        return true;
    unsigned char token;
    ssize_t received;
    do {
        received = read(debugger->command_pipe[0], &token, sizeof(token));
    } while (received < 0 && errno == EINTR);
    return received == (ssize_t)sizeof(token);
}

static void oasp_dbg_release_commands(oasp_dbg *debugger) {
    if (debugger->command_pipe[1] < 0)
        return;
    const unsigned char token = 1;
    ssize_t written;
    do {
        written = write(debugger->command_pipe[1], &token, sizeof(token));
    } while (written < 0 && errno == EINTR);
    (void)written;
}

static void oasp_dbg_command_loop(oasp_dbg *debugger) {
    if (!oasp_dbg_acquire_commands(debugger)) {
        debugger->quit = true;
        return;
    }
    char line[4096];
    while (!debugger->quit && debugger->mode == OASP_DBG_STOP) {
        if (isatty(fileno(debugger->input))) {
            fputs("(openasp-dbg) ", debugger->output);
            fflush(debugger->output);
        }
        if (!fgets(line, sizeof(line), debugger->input)) {
            debugger->quit = true;
            break;
        }
        size_t length = strlen(line);
        while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = '\0';
        const char *command = line;
        if (!length && debugger->last_command)
            command = debugger->last_command;
        else if (length) {
            char *copy = strdup(line);
            if (copy) {
                free(debugger->last_command);
                debugger->last_command = copy;
            }
        }
        char editable[4096];
        if (snprintf(editable, sizeof(editable), "%s", command) >= (int)sizeof(editable)) {
            fputs("Command is too long.\n", debugger->output);
            continue;
        }
        (void)oasp_dbg_execute_command(debugger, editable);
    }
    oasp_dbg_release_commands(debugger);
}

void oasp_dbg_init(oasp_dbg *debugger, FILE *input, FILE *output) {
    memset(debugger, 0, sizeof(*debugger));
    debugger->interface.hook = oasp_dbg_hook;
    debugger->interface.prepare_fork = oasp_dbg_prepare_fork;
    debugger->interface.forked = oasp_dbg_forked;
    debugger->interface.context = debugger;
    debugger->input = input;
    debugger->output = output;
    debugger->command_pipe[0] = -1;
    debugger->command_pipe[1] = -1;
    if (!pipe(debugger->command_pipe)) {
        const unsigned char token = 1;
        if (write(debugger->command_pipe[1], &token, sizeof(token)) !=
            (ssize_t)sizeof(token)) {
            close(debugger->command_pipe[0]);
            close(debugger->command_pipe[1]);
            debugger->command_pipe[0] = -1;
            debugger->command_pipe[1] = -1;
        }
    }
    (void)setvbuf(debugger->input, NULL, _IONBF, 0);
    (void)setvbuf(debugger->output, NULL, _IONBF, 0);
    debugger->next_id = 1;
    debugger->mode = OASP_DBG_STOP;
    debugger->initial_stop = true;
    debugger->attached = true;
    debugger->detach_on_fork = true;
    debugger->follow_fork_mode = OASP_DBG_FOLLOW_PARENT;
}

void oasp_dbg_dispose(oasp_dbg *debugger) {
    if (!debugger)
        return;
    for (size_t i = 0; i < debugger->breakpoint_count; i++) {
        free(debugger->breakpoints[i].file);
        free(debugger->breakpoints[i].function);
        free(debugger->breakpoints[i].condition);
    }
    for (size_t i = 0; i < debugger->watch_count; i++) {
        free(debugger->watches[i].name);
        free(debugger->watches[i].previous);
    }
    free(debugger->last_command);
    if (debugger->command_pipe[0] >= 0)
        close(debugger->command_pipe[0]);
    if (debugger->command_pipe[1] >= 0)
        close(debugger->command_pipe[1]);
}

void oasp_dbg_prepare_fork(void *context, oasp_runtime *runtime) {
    (void)runtime;
    oasp_dbg *debugger = context;
    fflush(debugger->output);
}

void oasp_dbg_forked(void *context, oasp_runtime *runtime, pid_t pid) {
    oasp_dbg *debugger = context;
    if (pid < 0) {
        fprintf(debugger->output, "[Process.Fork failed: %s]\n", strerror(errno));
        return;
    }
    bool child = pid == 0;
    pid_t self = getpid();
    debugger->related_pid = child ? getppid() : pid;
    debugger->related_attached = !debugger->detach_on_fork;
    bool followed = child ? debugger->follow_fork_mode == OASP_DBG_FOLLOW_CHILD
                          : debugger->follow_fork_mode == OASP_DBG_FOLLOW_PARENT;
    if (debugger->detach_on_fork && !followed) {
        fprintf(debugger->output, "[Detaching after fork from %s process %ld]\n",
                child ? "child" : "parent", (long)self);
        debugger->attached = false;
        oasp_runtime_set_debugger(runtime, NULL);
        return;
    }
    if (debugger->detach_on_fork) {
        fprintf(debugger->output, "[Following %s process %ld after fork]\n",
                child ? "child" : "parent", (long)self);
    } else {
        fprintf(debugger->output, "[New inferior: %s process %ld]\n",
                child ? "child" : "parent", (long)self);
    }
}

bool oasp_dbg_hook(void *context, const oasp_debug_event *event) {
    oasp_dbg *debugger = context;
    debugger->event = event;
    debugger->selected_frame = 0;
    if (event->kind == OASP_DEBUG_ERROR) {
        if (debugger->catch_errors &&
            event->runtime->error_generation != debugger->last_error_generation) {
            fprintf(debugger->output, "[Inferior process %ld]\n", (long)getpid());
            fprintf(debugger->output, "Catchpoint: ASP error %d: %s\n",
                    event->runtime->error_number,
                    event->runtime->error_description
                        ? event->runtime->error_description
                        : "");
            debugger->last_error_generation = event->runtime->error_generation;
            debugger->mode = OASP_DBG_STOP;
            oasp_dbg_print_location(debugger, event);
            oasp_dbg_command_loop(debugger);
        }
        bool keep_running = !debugger->quit;
        debugger->event = NULL;
        return keep_running;
    }
    bool stop = debugger->initial_stop;
    bool inferior_announced = false;
    debugger->initial_stop = false;
    oasp_dbg_breakpoint *breakpoint = oasp_dbg_breakpoint_hit(debugger, event);
    if (breakpoint) {
        fprintf(debugger->output, "[Inferior process %ld]\n", (long)getpid());
        inferior_announced = true;
        fprintf(debugger->output, "%sBreakpoint %u, ",
                breakpoint->temporary ? "Temporary " : "", breakpoint->id);
        unsigned id = breakpoint->id;
        bool temporary = breakpoint->temporary;
        stop = true;
        if (temporary)
            (void)oasp_dbg_delete_breakpoint(debugger, id);
    }
    if (oasp_dbg_watch_hit(debugger))
        stop = true;
    if (oasp_dbg_mode_stops(debugger, event))
        stop = true;
    if (stop) {
        debugger->mode = OASP_DBG_STOP;
        if (!inferior_announced)
            fprintf(debugger->output, "[Inferior process %ld]\n", (long)getpid());
        oasp_dbg_print_location(debugger, event);
        oasp_dbg_command_loop(debugger);
    }
    bool keep_running = !debugger->quit;
    debugger->event = NULL;
    return keep_running;
}
