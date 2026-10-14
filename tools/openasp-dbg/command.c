// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#define _POSIX_C_SOURCE 200809L

#include "dbg.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *oasp_dbg_trim(char *text) {
    while (isspace((unsigned char)*text))
        text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return text;
}

static char *oasp_dbg_argument(char *command) {
    while (*command && !isspace((unsigned char)*command))
        command++;
    if (*command)
        *command++ = '\0';
    return oasp_dbg_trim(command);
}

static bool oasp_dbg_unsigned(const char *text, unsigned *result) {
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !text[0] || !end || *end || value > UINT_MAX)
        return false;
    *result = (unsigned)value;
    return true;
}

static void oasp_dbg_resume(oasp_dbg *debugger, oasp_dbg_mode mode) {
    debugger->mode = mode;
    debugger->origin_depth = debugger->event->frame_depth;
    debugger->origin_line = debugger->event->instruction->line;
    debugger->origin_pc = debugger->event->pc;
    debugger->origin_bytecode = debugger->event->bytecode;
}

static const char *oasp_dbg_frame_name(const oasp_frame *frame) {
    if (!frame)
        return "<global>";
    if (frame->routine && frame->routine->name.data)
        return frame->routine->name.data;
    return frame->object ? "<class>" : "<frame>";
}

static size_t oasp_dbg_frame_name_length(const oasp_frame *frame) {
    if (frame && frame->routine && frame->routine->name.data)
        return frame->routine->name.length;
    return strlen(oasp_dbg_frame_name(frame));
}

static bool oasp_dbg_slice_equal(oasp_slice left, oasp_slice right) {
    if (left.length != right.length)
        return false;
    for (size_t i = 0; i < left.length; i++) {
        if (tolower((unsigned char)left.data[i]) !=
            tolower((unsigned char)right.data[i]))
            return false;
    }
    return true;
}

static void oasp_dbg_print_frame(oasp_dbg *debugger, size_t index) {
    const oasp_frame *frame = oasp_dbg_frame_at(debugger->event, index);
    if (!frame && index)
        return;
    const oasp_bytecode *bytecode = index ? frame->debug_bytecode : debugger->event->bytecode;
    size_t pc = index ? frame->debug_pc : debugger->event->pc;
    uint32_t line = bytecode && pc < bytecode->length
                        ? bytecode->instructions[pc].line
                        : 0;
    fprintf(debugger->output, "%c#%-3zu %.*s at %s:%" PRIu32 " pc=%zu\n",
            index == debugger->selected_frame ? '*' : ' ', index,
            (int)oasp_dbg_frame_name_length(frame), oasp_dbg_frame_name(frame),
            debugger->event->source_name ? debugger->event->source_name : "<script>",
            line, pc);
}

static void oasp_dbg_backtrace(oasp_dbg *debugger) {
    if (!debugger->event->frame) {
        oasp_dbg_print_frame(debugger, 0);
        return;
    }
    size_t index = 0;
    for (const oasp_frame *frame = debugger->event->frame; frame; frame = frame->parent)
        oasp_dbg_print_frame(debugger, index++);
}

static void oasp_dbg_print_bindings(FILE *output, const oasp_binding *binding) {
    for (; binding; binding = binding->next) {
        fprintf(output, "%.*s = ", (int)binding->name.length, binding->name.data);
        oasp_dbg_print_value(output, &binding->value);
        fputc('\n', output);
    }
}

static void oasp_dbg_info_locals(oasp_dbg *debugger) {
    const oasp_frame *frame = oasp_dbg_frame_at(debugger->event, debugger->selected_frame);
    if (!frame) {
        fputs("No local frame selected.\n", debugger->output);
        return;
    }
    oasp_dbg_print_bindings(debugger->output, frame->locals);
    for (size_t i = 0; i < frame->slot_count; i++) {
        if (!frame->slots[i].name.data)
            continue;
        fprintf(debugger->output, "%.*s = ", (int)frame->slots[i].name.length,
                frame->slots[i].name.data);
        oasp_dbg_print_value(debugger->output, &frame->slots[i].value);
        fputc('\n', debugger->output);
    }
}

static void oasp_dbg_info_args(oasp_dbg *debugger) {
    const oasp_frame *frame = oasp_dbg_frame_at(debugger->event, debugger->selected_frame);
    if (!frame || !frame->routine || !frame->routine_bytecode) {
        fputs("No routine arguments.\n", debugger->output);
        return;
    }
    for (size_t i = 0; i < frame->routine->parameter_count; i++) {
        oasp_parameter_bytecode parameter;
        if (!oasp_bytecode_routine_parameter(frame->routine_bytecode, frame->routine, i,
                                             &parameter))
            continue;
        const oasp_value *value = NULL;
        for (size_t slot = 0; slot < frame->slot_count; slot++) {
            if (oasp_dbg_slice_equal(frame->slots[slot].name, parameter.name)) {
                value = &frame->slots[slot].value;
                break;
            }
        }
        fprintf(debugger->output, "%.*s = ", (int)parameter.name.length, parameter.name.data);
        if (value)
            oasp_dbg_print_value(debugger->output, value);
        else
            fputs("<unavailable>", debugger->output);
        fputc('\n', debugger->output);
    }
}

static void oasp_dbg_info_globals(oasp_dbg *debugger) {
    oasp_runtime *runtime = debugger->event->runtime;
    oasp_dbg_print_bindings(debugger->output, runtime->variables);
    for (size_t i = 0; i < runtime->global_slot_count; i++) {
        oasp_slice name;
        if (!runtime->global_slots[i] || !runtime->global_slot_bytecode ||
            !oasp_bytecode_global_slot_at(runtime->global_slot_bytecode, i, &name))
            continue;
        fprintf(debugger->output, "%.*s = ", (int)name.length, name.data);
        oasp_dbg_print_value(debugger->output, &runtime->global_slots[i]->value);
        fputc('\n', debugger->output);
    }
}

static void oasp_dbg_print_expression(oasp_dbg *debugger, const char *expression) {
    oasp_value value;
    if (oasp_dbg_eval(debugger, expression, &value)) {
        fputs("$ = ", debugger->output);
        oasp_dbg_print_value(debugger->output, &value);
        fputc('\n', debugger->output);
    }
}

static bool oasp_dbg_set_variable(oasp_dbg *debugger, char *argument) {
    if (!strncmp(argument, "variable ", 9))
        argument = oasp_dbg_trim(argument + 9);
    char *equals = strchr(argument, '=');
    if (!equals) {
        fputs("Usage: set variable NAME = EXPRESSION\n", debugger->output);
        return false;
    }
    *equals = '\0';
    char *name = oasp_dbg_trim(argument);
    char *expression = oasp_dbg_trim(equals + 1);
    if (!*name || !*expression || strpbrk(name, " \t()")) {
        fputs("Only a simple variable name may be assigned.\n", debugger->output);
        return false;
    }
    oasp_value value;
    if (!oasp_dbg_eval(debugger, expression, &value))
        return false;
    oasp_runtime *runtime = debugger->event->runtime;
    oasp_frame *saved = runtime->frame;
    runtime->frame = (oasp_frame *)oasp_dbg_frame_at(debugger->event,
                                                     debugger->selected_frame);
    oasp_slice variable = {name, strlen(name)};
    oasp_value owned = oasp_runtime_value_copy(runtime, &value, true);
    bool assigned = oasp_runtime_set(runtime, variable, &owned, false);
    runtime->frame = saved;
    if (!assigned)
        fputs("Unable to assign variable.\n", debugger->output);
    return assigned;
}

static void oasp_dbg_add_watch(oasp_dbg *debugger, const char *expression) {
    if (!*expression || debugger->watch_count == OASP_DBG_MAX_WATCHES) {
        fputs("Usage: watch EXPRESSION (or watchpoint limit reached)\n", debugger->output);
        return;
    }
    oasp_dbg_watch *watch = &debugger->watches[debugger->watch_count];
    memset(watch, 0, sizeof(*watch));
    watch->id = debugger->next_id++;
    watch->name = strdup(expression);
    if (!watch->name) {
        fputs("Out of memory.\n", debugger->output);
        return;
    }
    debugger->watch_count++;
    fprintf(debugger->output, "Watchpoint %u: %s\n", watch->id, watch->name);
}

static void oasp_dbg_delete(oasp_dbg *debugger, const char *argument) {
    unsigned id;
    if (!oasp_dbg_unsigned(argument, &id)) {
        fputs("Usage: delete ID\n", debugger->output);
        return;
    }
    if (oasp_dbg_delete_breakpoint(debugger, id))
        return;
    for (size_t i = 0; i < debugger->watch_count; i++) {
        if (debugger->watches[i].id != id)
            continue;
        free(debugger->watches[i].name);
        free(debugger->watches[i].previous);
        if (i + 1 < debugger->watch_count)
            memmove(&debugger->watches[i], &debugger->watches[i + 1],
                    (debugger->watch_count - i - 1) * sizeof(debugger->watches[i]));
        debugger->watch_count--;
        return;
    }
    fprintf(debugger->output, "No breakpoint or watchpoint number %u.\n", id);
}

static void oasp_dbg_toggle(oasp_dbg *debugger, const char *argument, bool enabled) {
    unsigned id;
    oasp_dbg_breakpoint *breakpoint =
        oasp_dbg_unsigned(argument, &id) ? oasp_dbg_find_breakpoint(debugger, id) : NULL;
    if (!breakpoint)
        fputs("Usage: enable ID / disable ID\n", debugger->output);
    else
        breakpoint->enabled = enabled;
}

static void oasp_dbg_condition(oasp_dbg *debugger, char *argument) {
    char *expression = oasp_dbg_argument(argument);
    unsigned id;
    oasp_dbg_breakpoint *breakpoint =
        oasp_dbg_unsigned(argument, &id) ? oasp_dbg_find_breakpoint(debugger, id) : NULL;
    if (!breakpoint) {
        fputs("Usage: condition ID [EXPRESSION]\n", debugger->output);
        return;
    }
    char *copy = *expression ? strdup(expression) : NULL;
    if (*expression && !copy) {
        fputs("Out of memory.\n", debugger->output);
        return;
    }
    free(breakpoint->condition);
    breakpoint->condition = copy;
}

static void oasp_dbg_ignore(oasp_dbg *debugger, char *argument) {
    char *count_text = oasp_dbg_argument(argument);
    unsigned id;
    unsigned count;
    oasp_dbg_breakpoint *breakpoint =
        oasp_dbg_unsigned(argument, &id) ? oasp_dbg_find_breakpoint(debugger, id) : NULL;
    if (!breakpoint || !oasp_dbg_unsigned(count_text, &count)) {
        fputs("Usage: ignore ID COUNT\n", debugger->output);
        return;
    }
    breakpoint->ignore_count = count;
}

static bool oasp_dbg_set_fork_option(oasp_dbg *debugger, const char *argument) {
    if (!strncmp(argument, "follow-fork-mode ", 17)) {
        const char *value = argument + 17;
        if (!strcmp(value, "parent"))
            debugger->follow_fork_mode = OASP_DBG_FOLLOW_PARENT;
        else if (!strcmp(value, "child"))
            debugger->follow_fork_mode = OASP_DBG_FOLLOW_CHILD;
        else {
            fputs("Usage: set follow-fork-mode parent|child\n", debugger->output);
            return true;
        }
        return true;
    }
    if (!strncmp(argument, "detach-on-fork ", 15)) {
        const char *value = argument + 15;
        if (!strcmp(value, "on"))
            debugger->detach_on_fork = true;
        else if (!strcmp(value, "off"))
            debugger->detach_on_fork = false;
        else {
            fputs("Usage: set detach-on-fork on|off\n", debugger->output);
            return true;
        }
        return true;
    }
    return false;
}

static void oasp_dbg_show(oasp_dbg *debugger, const char *argument) {
    if (!strcmp(argument, "follow-fork-mode")) {
        fprintf(debugger->output, "The debugger follows the %s process after fork.\n",
                debugger->follow_fork_mode == OASP_DBG_FOLLOW_CHILD ? "child" : "parent");
    } else if (!strcmp(argument, "detach-on-fork")) {
        fprintf(debugger->output, "Detaching after fork is %s.\n",
                debugger->detach_on_fork ? "on" : "off");
    } else {
        fputs("Usage: show follow-fork-mode|detach-on-fork\n", debugger->output);
    }
}

static void oasp_dbg_info(oasp_dbg *debugger, const char *argument) {
    if (!strcmp(argument, "breakpoints") || !strcmp(argument, "break"))
        oasp_dbg_print_breakpoints(debugger);
    else if (!strcmp(argument, "locals"))
        oasp_dbg_info_locals(debugger);
    else if (!strcmp(argument, "args"))
        oasp_dbg_info_args(debugger);
    else if (!strcmp(argument, "globals"))
        oasp_dbg_info_globals(debugger);
    else if (!strcmp(argument, "line"))
        oasp_dbg_print_location(debugger, debugger->event);
    else if (!strcmp(argument, "opcode"))
        oasp_dbg_print_instruction(debugger->output, debugger->event->bytecode,
                                   debugger->event->pc);
    else if (!strcmp(argument, "inferiors")) {
        fprintf(debugger->output, "* process %ld (%s)\n", (long)getpid(),
                debugger->attached ? "attached" : "detached");
        if (debugger->related_pid > 0)
            fprintf(debugger->output, "  process %ld (%s)\n",
                    (long)debugger->related_pid,
                    debugger->related_attached ? "attached" : "detached");
    }
    else
        fputs("Usage: info breakpoints|locals|args|globals|line|opcode|inferiors\n",
              debugger->output);
}

static void oasp_dbg_source_commands(oasp_dbg *debugger, const char *path) {
    FILE *input = fopen(path, "r");
    if (!input) {
        fprintf(debugger->output, "Cannot open command file '%s'.\n", path);
        return;
    }
    char line[4096];
    while (debugger->mode == OASP_DBG_STOP && fgets(line, sizeof(line), input)) {
        char *command = oasp_dbg_trim(line);
        if (*command && *command != '#')
            (void)oasp_dbg_execute_command(debugger, command);
    }
    fclose(input);
}

static void oasp_dbg_help(FILE *output) {
    fputs(
        "Commands:\n"
        "  run|continue|step|next|finish|stepi|nexti|until\n"
        "  break [FILE:]LINE|FUNCTION, tbreak LOCATION\n"
        "  delete ID, enable ID, disable ID, condition ID EXPR, ignore ID COUNT\n"
        "  watch EXPR, catch error\n"
        "  backtrace, frame N, up, down\n"
        "  print EXPR, set variable NAME = EXPR\n"
        "  set follow-fork-mode parent|child, set detach-on-fork on|off\n"
        "  show follow-fork-mode|detach-on-fork, info inferiors\n"
        "  info breakpoints|locals|args|globals|line|opcode\n"
        "  list [LINE], disassemble, source FILE, quit\n",
        output);
}

bool oasp_dbg_execute_command(oasp_dbg *debugger, char *line) {
    char *command = oasp_dbg_trim(line);
    char *argument = oasp_dbg_argument(command);
    if (!*command)
        return true;
    if (!strcmp(command, "continue") || !strcmp(command, "c") || !strcmp(command, "run") ||
        !strcmp(command, "r"))
        oasp_dbg_resume(debugger, OASP_DBG_CONTINUE);
    else if (!strcmp(command, "step") || !strcmp(command, "s"))
        oasp_dbg_resume(debugger, OASP_DBG_STEP);
    else if (!strcmp(command, "next") || !strcmp(command, "n"))
        oasp_dbg_resume(debugger, OASP_DBG_NEXT);
    else if (!strcmp(command, "finish"))
        oasp_dbg_resume(debugger, OASP_DBG_FINISH);
    else if (!strcmp(command, "stepi") || !strcmp(command, "si"))
        oasp_dbg_resume(debugger, OASP_DBG_STEPI);
    else if (!strcmp(command, "nexti") || !strcmp(command, "ni"))
        oasp_dbg_resume(debugger, OASP_DBG_NEXTI);
    else if (!strcmp(command, "until"))
        oasp_dbg_resume(debugger, OASP_DBG_UNTIL);
    else if (!strcmp(command, "break") || !strcmp(command, "b") ||
             !strcmp(command, "tbreak")) {
        char location[64];
        if (!*argument) {
            snprintf(location, sizeof(location), "%" PRIu32,
                     debugger->event->instruction->line);
            argument = location;
        }
        oasp_dbg_breakpoint *breakpoint =
            oasp_dbg_add_breakpoint(debugger, argument, !strcmp(command, "tbreak"));
        if (breakpoint)
            fprintf(debugger->output, "%sBreakpoint %u at %s\n",
                    breakpoint->temporary ? "Temporary " : "", breakpoint->id, argument);
        else
            fputs("Invalid breakpoint location or breakpoint limit reached.\n",
                  debugger->output);
    } else if (!strcmp(command, "delete") || !strcmp(command, "d"))
        oasp_dbg_delete(debugger, argument);
    else if (!strcmp(command, "enable"))
        oasp_dbg_toggle(debugger, argument, true);
    else if (!strcmp(command, "disable"))
        oasp_dbg_toggle(debugger, argument, false);
    else if (!strcmp(command, "condition"))
        oasp_dbg_condition(debugger, argument);
    else if (!strcmp(command, "ignore"))
        oasp_dbg_ignore(debugger, argument);
    else if (!strcmp(command, "watch"))
        oasp_dbg_add_watch(debugger, argument);
    else if (!strcmp(command, "catch") && !strcmp(argument, "error")) {
        debugger->catch_errors = true;
        fputs("Catchpoint: ASP errors\n", debugger->output);
    } else if (!strcmp(command, "backtrace") || !strcmp(command, "bt"))
        oasp_dbg_backtrace(debugger);
    else if (!strcmp(command, "frame")) {
        unsigned index;
        if (!oasp_dbg_unsigned(argument, &index) ||
            (!oasp_dbg_frame_at(debugger->event, index) && index)) {
            fputs("Invalid frame number.\n", debugger->output);
        } else {
            debugger->selected_frame = index;
            oasp_dbg_print_frame(debugger, index);
        }
    } else if (!strcmp(command, "up")) {
        if (oasp_dbg_frame_at(debugger->event, debugger->selected_frame + 1))
            debugger->selected_frame++;
        oasp_dbg_print_frame(debugger, debugger->selected_frame);
    } else if (!strcmp(command, "down")) {
        if (debugger->selected_frame)
            debugger->selected_frame--;
        oasp_dbg_print_frame(debugger, debugger->selected_frame);
    } else if (!strcmp(command, "print") || !strcmp(command, "p"))
        oasp_dbg_print_expression(debugger, argument);
    else if (!strcmp(command, "set")) {
        if (!oasp_dbg_set_fork_option(debugger, argument))
            (void)oasp_dbg_set_variable(debugger, argument);
    } else if (!strcmp(command, "show"))
        oasp_dbg_show(debugger, argument);
    else if (!strcmp(command, "info"))
        oasp_dbg_info(debugger, argument);
    else if (!strcmp(command, "list") || !strcmp(command, "l")) {
        unsigned center = debugger->event->instruction->line;
        if (*argument && !oasp_dbg_unsigned(argument, &center))
            fputs("Usage: list [LINE]\n", debugger->output);
        else
            oasp_dbg_list_source(debugger, debugger->event, center, 5);
    } else if (!strcmp(command, "disassemble") || !strcmp(command, "disas"))
        oasp_dbg_disassemble(debugger->output, debugger->event->bytecode, 0, 0);
    else if (!strcmp(command, "source"))
        oasp_dbg_source_commands(debugger, argument);
    else if (!strcmp(command, "help") || !strcmp(command, "h"))
        oasp_dbg_help(debugger->output);
    else if (!strcmp(command, "quit") || !strcmp(command, "q"))
        debugger->quit = true;
    else {
        fprintf(debugger->output, "Undefined command: %s. Try 'help'.\n", command);
        debugger->command_error = true;
        return false;
    }
    return true;
}
