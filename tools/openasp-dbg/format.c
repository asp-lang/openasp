// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "dbg.h"
#include "bytecode_format.h"

void oasp_dbg_print_value(FILE *output, const oasp_value *value) {
    oasp_tool_print_value(output, value, OASP_TOOL_VALUE_DEBUG);
}

void oasp_dbg_print_instruction(FILE *output, const oasp_bytecode *bytecode, size_t pc) {
    if (!bytecode || pc >= bytecode->length)
        return;
    const oasp_instruction *instruction = &bytecode->instructions[pc];
    fprintf(output, "=> %06zx  ", pc * sizeof(*instruction));
    oasp_tool_print_instruction_bytes(output, instruction);
    fprintf(output, "  %-29s ", oasp_tool_opcode_name(instruction->opcode));
    oasp_tool_print_instruction_fields(output, instruction);
    fputc('\n', output);
}

void oasp_dbg_disassemble(FILE *output, const oasp_bytecode *bytecode, size_t start, size_t limit) {
    if (!bytecode)
        return;
    if (start > bytecode->length)
        start = bytecode->length;
    if (!limit || limit > bytecode->length)
        limit = bytecode->length;
    for (size_t pc = start; pc < limit; pc++)
        oasp_dbg_print_instruction(output, bytecode, pc);
}
