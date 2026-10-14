// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "dbg.h"
#include "bytecode_format.h"

#include <inttypes.h>

bool oasp_dbg_source_line(oasp_slice source, size_t line, oasp_slice *result) {
    if (!line || !result)
        return false;
    size_t current = 1;
    size_t start = 0;
    for (size_t i = 0; i <= source.length; i++) {
        bool end = i == source.length || source.data[i] == '\n' || source.data[i] == '\r';
        if (!end)
            continue;
        if (current == line) {
            result->data = source.data + start;
            result->length = i - start;
            return true;
        }
        if (i < source.length && source.data[i] == '\r' && i + 1 < source.length &&
            source.data[i + 1] == '\n')
            i++;
        start = i + 1;
        current++;
    }
    return false;
}

void oasp_dbg_print_location(const oasp_dbg *debugger, const oasp_debug_event *event) {
    if (!debugger || !event)
        return;
    const char *name = event->source_name ? event->source_name : "<script>";
    fprintf(debugger->output, "%s:%" PRIu32 " pc=%zu %s\n", name, event->instruction->line,
            event->pc, oasp_tool_opcode_name(event->instruction->opcode));
    oasp_slice line;
    if (oasp_dbg_source_line(event->source, event->instruction->line, &line))
        fprintf(debugger->output, "%" PRIu32 "\t%.*s\n", event->instruction->line,
                (int)line.length, line.data);
}

void oasp_dbg_list_source(const oasp_dbg *debugger, const oasp_debug_event *event, size_t center, size_t radius) {
    if (!debugger || !event)
        return;
    if (!center)
        center = event->instruction->line;
    size_t first = center > radius ? center - radius : 1;
    size_t last = center > SIZE_MAX - radius ? SIZE_MAX : center + radius;
    for (size_t line_number = first; line_number <= last; line_number++) {
        oasp_slice line;
        if (!oasp_dbg_source_line(event->source, line_number, &line))
            break;
        fprintf(debugger->output, "%c%5zu\t%.*s\n",
                line_number == event->instruction->line ? '>' : ' ', line_number,
                (int)line.length, line.data);
        if (line_number == SIZE_MAX)
            break;
    }
}
