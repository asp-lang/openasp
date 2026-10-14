// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#define _POSIX_C_SOURCE 200809L

#include "dbg.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static bool oasp_dbg_parse_line(const char *text, size_t *line) {
    if (!text || !*text)
        return false;
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || !value || !end || *end || value > SIZE_MAX)
        return false;
    *line = (size_t)value;
    return true;
}

static bool oasp_dbg_file_equal(const char *left, const char *right) {
    if (!left || !right)
        return false;
    if (!strcmp(left, right))
        return true;
    const char *left_separator = strrchr(left, '/');
    if (left_separator) {
        if (left[0] == '/')
            return false;
        size_t left_length = strlen(left);
        size_t right_length = strlen(right);
        return right_length > left_length &&
               right[right_length - left_length - 1] == '/' &&
               !strcmp(right + right_length - left_length, left);
    }
    const char *left_base = strrchr(left, '/');
    const char *right_base = strrchr(right, '/');
    left_base = left_base ? left_base + 1 : left;
    right_base = right_base ? right_base + 1 : right;
    return !strcmp(left_base, right_base);
}

oasp_dbg_breakpoint *oasp_dbg_add_breakpoint(oasp_dbg *debugger, const char *location, bool temporary) {
    if (!debugger || !location || !*location ||
        debugger->breakpoint_count == OASP_DBG_MAX_BREAKPOINTS)
        return NULL;
    oasp_dbg_breakpoint candidate = {0};
    candidate.id = debugger->next_id++;
    candidate.enabled = true;
    candidate.temporary = temporary;
    const char *colon = strrchr(location, ':');
    if (colon && colon != location && oasp_dbg_parse_line(colon + 1, &candidate.line)) {
        size_t length = (size_t)(colon - location);
        candidate.file = malloc(length + 1);
        if (!candidate.file)
            return NULL;
        memcpy(candidate.file, location, length);
        candidate.file[length] = '\0';
    } else if (!oasp_dbg_parse_line(location, &candidate.line)) {
        candidate.function = strdup(location);
        if (!candidate.function)
            return NULL;
    }
    debugger->breakpoints[debugger->breakpoint_count] = candidate;
    return &debugger->breakpoints[debugger->breakpoint_count++];
}

oasp_dbg_breakpoint *oasp_dbg_find_breakpoint(oasp_dbg *debugger, unsigned id) {
    if (!debugger)
        return NULL;
    for (size_t i = 0; i < debugger->breakpoint_count; i++) {
        if (debugger->breakpoints[i].id == id)
            return &debugger->breakpoints[i];
    }
    return NULL;
}

bool oasp_dbg_delete_breakpoint(oasp_dbg *debugger, unsigned id) {
    oasp_dbg_breakpoint *breakpoint = oasp_dbg_find_breakpoint(debugger, id);
    if (!breakpoint)
        return false;
    free(breakpoint->file);
    free(breakpoint->function);
    free(breakpoint->condition);
    size_t index = (size_t)(breakpoint - debugger->breakpoints);
    if (index + 1 < debugger->breakpoint_count)
        memmove(&debugger->breakpoints[index], &debugger->breakpoints[index + 1],
                (debugger->breakpoint_count - index - 1) * sizeof(*breakpoint));
    debugger->breakpoint_count--;
    return true;
}

static bool oasp_dbg_function_entry(const oasp_debug_event *event, const char *name) {
    for (size_t i = 0; i < event->bytecode->routine_count; i++) {
        oasp_routine_bytecode routine;
        if (!oasp_bytecode_routine_at(event->bytecode, i, &routine))
            continue;
        if (routine.entry == event->pc && strlen(name) == routine.name.length &&
            oasp_ascii_equal_n(routine.name.data, routine.name.length, name))
            return true;
    }
    return false;
}

static size_t oasp_dbg_instruction_region_entry(const oasp_debug_event *event) {
    if (event->frame && event->frame->routine &&
        event->frame->routine_bytecode == event->bytecode &&
        event->pc >= event->frame->routine->entry &&
        event->pc < event->frame->routine->limit)
        return event->frame->routine->entry;
    for (size_t i = 0; i < event->bytecode->routine_count; i++) {
        oasp_routine_bytecode routine;
        if (oasp_bytecode_routine_at(event->bytecode, i, &routine) &&
            event->pc >= routine.entry && event->pc < routine.limit)
            return routine.entry;
    }
    for (size_t i = 0; i < event->bytecode->class_count; i++) {
        oasp_class_bytecode class_def;
        if (oasp_bytecode_class_at(event->bytecode, i, &class_def) &&
            event->pc >= class_def.entry && event->pc < class_def.limit)
            return class_def.entry;
    }
    return 0;
}

static bool oasp_dbg_breakpoint_matches(const oasp_dbg_breakpoint *breakpoint, const oasp_debug_event *event) {
    if (!breakpoint->enabled)
        return false;
    if (breakpoint->function)
        return oasp_dbg_function_entry(event, breakpoint->function);
    if (breakpoint->file &&
        !oasp_dbg_file_equal(breakpoint->file, event->source_name))
        return false;
    if (breakpoint->line != event->instruction->line)
        return false;
    size_t entry = oasp_dbg_instruction_region_entry(event);
    for (size_t pc = entry; pc < event->pc; pc++) {
        if (event->bytecode->instructions[pc].line == breakpoint->line)
            return false;
    }
    return true;
}

oasp_dbg_breakpoint *oasp_dbg_breakpoint_hit(oasp_dbg *debugger, const oasp_debug_event *event) {
    if (!debugger || !event)
        return NULL;
    for (size_t i = 0; i < debugger->breakpoint_count; i++) {
        oasp_dbg_breakpoint *breakpoint = &debugger->breakpoints[i];
        if (!oasp_dbg_breakpoint_matches(breakpoint, event))
            continue;
        breakpoint->hit_count++;
        if (breakpoint->ignore_count) {
            breakpoint->ignore_count--;
            continue;
        }
        if (breakpoint->condition) {
            oasp_value condition;
            if (!oasp_dbg_eval(debugger, breakpoint->condition, &condition) ||
                !oasp_value_truthy(&condition))
                continue;
        }
        return breakpoint;
    }
    return NULL;
}

void oasp_dbg_print_breakpoints(const oasp_dbg *debugger) {
    fprintf(debugger->output, "Num Type        Enb Hits Ignore What\n");
    for (size_t i = 0; i < debugger->breakpoint_count; i++) {
        const oasp_dbg_breakpoint *breakpoint = &debugger->breakpoints[i];
        fprintf(debugger->output, "%-3u breakpoint  %c   %-4zu %-6zu ",
                breakpoint->id, breakpoint->enabled ? 'y' : 'n',
                breakpoint->hit_count, breakpoint->ignore_count);
        if (breakpoint->function)
            fprintf(debugger->output, "%s", breakpoint->function);
        else if (breakpoint->file)
            fprintf(debugger->output, "%s:%zu", breakpoint->file, breakpoint->line);
        else
            fprintf(debugger->output, "line %zu", breakpoint->line);
        if (breakpoint->temporary)
            fputs(" (temporary)", debugger->output);
        if (breakpoint->condition)
            fprintf(debugger->output, " if %s", breakpoint->condition);
        fputc('\n', debugger->output);
    }
}
