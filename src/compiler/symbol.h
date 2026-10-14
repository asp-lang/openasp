// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/compiler/symbol.h`): Compiler pipeline. Arena-owned tokens, AST nodes,
 * symbols, bytecode, and immutable images move through this stage without per-node frees.
 * Length/count variables bound every indexed operation and malformed input must fail before
 * execution.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#ifndef OPENASP_SYMBOL_H
#define OPENASP_SYMBOL_H

#include "parser.h"

typedef enum {
    OASP_SYMBOL_DYNAMIC,
    OASP_SYMBOL_LOCAL,
    OASP_SYMBOL_PARAMETER,
    OASP_SYMBOL_MODULE_GLOBAL,
    OASP_SYMBOL_ROUTINE,
    OASP_SYMBOL_CLASS_METHOD,
    OASP_SYMBOL_BUILTIN
} oasp_symbol_kind;

typedef enum {
    OASP_BUILTIN_DYNAMIC,
    OASP_BUILTIN_RESPONSE_WRITE,
    OASP_BUILTIN_CSTR,
    OASP_BUILTIN_LEN,
    OASP_BUILTIN_ISEMPTY,
    OASP_BUILTIN_SERVER_HTMLENCODE,
    OASP_BUILTIN_SERVER_URLENCODE,
    OASP_BUILTIN_TRIM,
    OASP_BUILTIN_LCASE,
    OASP_BUILTIN_UCASE,
    OASP_BUILTIN_REPLACE,
    OASP_BUILTIN_COUNT
} oasp_builtin_id;

typedef enum {
    OASP_MEMBER_DYNAMIC,
    OASP_MEMBER_FIELDS,
    OASP_MEMBER_ITEM,
    OASP_MEMBER_MOVENEXT,
    OASP_MEMBER_EOF,
    OASP_MEMBER_EXECUTE,
    OASP_MEMBER_OPEN,
    OASP_MEMBER_CLOSE,
    OASP_MEMBER_COUNT
} oasp_member_id;

typedef struct {
    oasp_symbol_kind kind;
    uint32_t id;
    uint32_t slot;
    oasp_value_kind value_kind;
} oasp_symbol;

typedef struct oasp_symbol_resolution oasp_symbol_resolution;

uint32_t oasp_symbol_id(oasp_slice name);
oasp_builtin_id oasp_builtin_id_for_name(oasp_slice name);
oasp_member_id oasp_member_id_for_name(oasp_slice name);
bool oasp_symbol_resolve_program(oasp_arena *arena, const oasp_block *program, bool module_dynamic, oasp_symbol_resolution **result);
bool oasp_symbol_lookup(const oasp_symbol_resolution *resolution, const oasp_expr *expr, oasp_symbol *result);

#endif
