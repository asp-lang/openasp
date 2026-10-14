// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_TOOL_BYTECODE_FORMAT_H
#define OPENASP_TOOL_BYTECODE_FORMAT_H

#include "bytecode.h"

#include <stdio.h>

typedef enum {
    OASP_TOOL_VALUE_DEBUG,
    OASP_TOOL_VALUE_TYPED
} oasp_tool_value_style;

bool oasp_tool_opcode_valid(unsigned opcode);
const char *oasp_tool_opcode_name(unsigned opcode);
void oasp_tool_print_escaped(FILE *output, oasp_slice value);
void oasp_tool_print_value(FILE *output, const oasp_value *value, oasp_tool_value_style style);
void oasp_tool_print_instruction_bytes(FILE *output, const oasp_instruction *instruction);
void oasp_tool_print_instruction_fields(FILE *output, const oasp_instruction *instruction);

#endif
