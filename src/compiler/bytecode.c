// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/compiler/bytecode.c`): Compiler pipeline. Arena-owned tokens, AST
 * nodes, symbols, bytecode, and immutable images move through this stage without per-node
 * frees. Length/count variables bound every indexed operation and malformed input must fail
 * before execution.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#include "bytecode.h"
#include "symbol.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OASP_BC_NONE UINT32_MAX

typedef struct oasp_loop_patch {
    struct oasp_loop_patch *next;
    size_t pc;
} oasp_loop_patch;

typedef struct oasp_loop_context {
    struct oasp_loop_context *parent;
    oasp_loop_patch *exits;
    int exit_kind;
} oasp_loop_context;

typedef struct {
    oasp_arena *arena;
    oasp_arena *scratch;
    oasp_bytecode *bytecode;
    const oasp_block *program;
    const oasp_symbol_resolution *symbols;
    oasp_loop_context *loop;
    oasp_slice *local_slot_names;
    size_t local_slot_count;
    size_t local_slot_capacity;
    size_t name_capacity;
    size_t constant_capacity;
    size_t instruction_capacity;
    bool in_routine;
    bool object_routine;
    bool unresolved_globals_dynamic;
    bool failed;
} oasp_bytecode_compiler;

static bool oasp_resolved_symbol(const oasp_bytecode_compiler *compiler, const oasp_expr *expr, oasp_symbol *symbol) {
    return compiler->symbols && oasp_symbol_lookup(compiler->symbols, expr, symbol);
}

const oasp_instruction *oasp_bytecode_instruction_at(const oasp_bytecode *bytecode, size_t index) {
    return bytecode && bytecode->instructions && index < bytecode->length ? &bytecode->instructions[index] : NULL;
}

static bool oasp_name_equal(oasp_slice left, oasp_slice right) {
    if (left.length != right.length)
        return false;
    for (size_t i = 0; i < left.length; i++)
        if (tolower((unsigned char)left.data[i]) != tolower((unsigned char)right.data[i]))
            return false;
    return true;
}

static bool oasp_is_intrinsic_err(oasp_slice name) { return oasp_ascii_equal(name, "err"); }

static size_t oasp_slot_index(const oasp_slice *names, size_t count, oasp_slice name) {
    for (size_t i = 0; i < count; i++)
        if (oasp_name_equal(names[i], name))
            return i;
    return SIZE_MAX;
}

static bool oasp_simple_identifier(const oasp_expr *expr) {
    return expr && expr->kind == OASP_EXPR_IDENTIFIER && !memchr(expr->name.data, '.', expr->name.length);
}

static size_t oasp_global_slot_index(const oasp_bytecode_compiler *compiler, oasp_slice name);
static size_t oasp_local_slot_index(const oasp_bytecode_compiler *compiler, oasp_slice name);

static bool oasp_dotted_parts(oasp_slice name, oasp_slice *base, oasp_slice *member) {
    const char *dot = memchr(name.data, '.', name.length);
    if (!dot || dot == name.data || dot == name.data + name.length - 1 ||
        memchr(dot + 1, '.', (size_t)(name.data + name.length - dot - 1)))
        return false;
    base->data = name.data;
    base->length = (size_t)(dot - name.data);
    member->data = dot + 1;
    member->length = name.length - base->length - 1;
    return true;
}

static bool oasp_dotted_root_slot(const oasp_bytecode_compiler *compiler, oasp_slice name, int32_t *encoded_slot, oasp_slice *member) {
    oasp_slice base;
    if (!oasp_dotted_parts(name, &base, member))
        return false;
    size_t slot = oasp_local_slot_index(compiler, base);
    if (slot != SIZE_MAX && slot < INT32_MAX) {
        *encoded_slot = (int32_t)slot + 1;
        return true;
    }
    slot = compiler->in_routine ? SIZE_MAX : oasp_global_slot_index(compiler, base);
    if (slot != SIZE_MAX && slot < INT32_MAX) {
        *encoded_slot = -(int32_t)slot - 1;
        return true;
    }
    return false;
}

static oasp_member_id oasp_dotted_member_id(oasp_slice name) {
    size_t offset = name.length;
    while (offset && name.data[offset - 1] != '.')
        offset--;
    return offset && offset < name.length
               ? oasp_member_id_for_name((oasp_slice){name.data + offset, name.length - offset})
               : OASP_MEMBER_DYNAMIC;
}

static const oasp_expr *oasp_include_path(const oasp_stmt *stmt) {
    const oasp_expr *call = stmt ? stmt->value : NULL;
    if (!call || stmt->kind != OASP_STMT_CALL || call->kind != OASP_EXPR_CALL || call->arg_count != 1 || !call->left ||
        call->left->kind != OASP_EXPR_IDENTIFIER || !oasp_ascii_equal(call->left->name, "__openasp_include") || !call->args[0] ||
        call->args[0]->kind != OASP_EXPR_LITERAL || call->args[0]->literal.kind != OASP_VALUE_STRING)
        return NULL;
    return call->args[0];
}

static void oasp_add_slot(oasp_slice *names, size_t *count, size_t capacity, oasp_slice name) {
    if (!oasp_is_intrinsic_err(name) && name.length && *count < capacity && oasp_slot_index(names, *count, name) == SIZE_MAX)
        names[(*count)++] = name;
}

static size_t oasp_count_slot_candidates(const oasp_block *block) {
    size_t count = 0;
    for (const oasp_stmt *stmt = block->first; stmt; stmt = stmt->next) {
        if (stmt->kind == OASP_STMT_DIM)
            count += stmt->declaration_count;
        else if (stmt->kind == OASP_STMT_CONST)
            count += stmt->constant_count;
        else if (stmt->kind == OASP_STMT_ASSIGN || stmt->kind == OASP_STMT_SET_ASSIGN || stmt->kind == OASP_STMT_FOR ||
                 stmt->kind == OASP_STMT_FOR_EACH)
            count++;
        if (stmt->kind == OASP_STMT_IF) {
            count += oasp_count_slot_candidates(&stmt->body) + oasp_count_slot_candidates(&stmt->alternative);
        } else if (stmt->kind == OASP_STMT_WHILE || stmt->kind == OASP_STMT_DO || stmt->kind == OASP_STMT_FOR ||
                   stmt->kind == OASP_STMT_FOR_EACH || stmt->kind == OASP_STMT_WITH) {
            count += oasp_count_slot_candidates(&stmt->body);
        } else if (stmt->kind == OASP_STMT_SELECT) {
            for (const oasp_case_branch *branch = stmt->cases; branch; branch = branch->next)
                count += oasp_count_slot_candidates(&branch->body);
        }
    }
    return count;
}

static size_t oasp_global_slot_index(const oasp_bytecode_compiler *compiler, oasp_slice name) {
    return oasp_slot_index(compiler->bytecode->global_slot_names, compiler->bytecode->global_slot_count, name);
}

static size_t oasp_local_slot_index(const oasp_bytecode_compiler *compiler, oasp_slice name) {
    return oasp_slot_index(compiler->local_slot_names, compiler->local_slot_count, name);
}

static size_t oasp_static_routine_index(const oasp_bytecode_compiler *compiler, oasp_slice name) {
    if (!compiler || !compiler->program || compiler->object_routine)
        return SIZE_MAX;
    size_t index = 0;
    for (const oasp_stmt *stmt = compiler->program->first; stmt; stmt = stmt->next) {
        if (stmt->kind != OASP_STMT_ROUTINE)
            continue;
        if (oasp_name_equal(stmt->name, name))
            return index;
        index++;
    }
    return SIZE_MAX;
}

static void oasp_collect_global_slots(oasp_bytecode_compiler *compiler, const oasp_block *block, size_t capacity) {
    for (const oasp_stmt *stmt = block->first; stmt; stmt = stmt->next) {
        if (stmt->kind == OASP_STMT_DIM) {
            for (size_t i = 0; i < stmt->declaration_count; i++)
                oasp_add_slot(compiler->bytecode->global_slot_names, &compiler->bytecode->global_slot_count, capacity, stmt->declarations[i].name);
        } else if (stmt->kind == OASP_STMT_CONST) {
            for (size_t i = 0; i < stmt->constant_count; i++)
                oasp_add_slot(compiler->bytecode->global_slot_names, &compiler->bytecode->global_slot_count, capacity, stmt->constants[i].name);
        } else if (!compiler->unresolved_globals_dynamic && (stmt->kind == OASP_STMT_FOR || stmt->kind == OASP_STMT_FOR_EACH)) {
            oasp_add_slot(compiler->bytecode->global_slot_names, &compiler->bytecode->global_slot_count, capacity, stmt->name);
        } else if (!compiler->unresolved_globals_dynamic &&
                   (stmt->kind == OASP_STMT_ASSIGN || stmt->kind == OASP_STMT_SET_ASSIGN) && oasp_simple_identifier(stmt->target)) {
            oasp_add_slot(compiler->bytecode->global_slot_names, &compiler->bytecode->global_slot_count, capacity, stmt->target->name);
        }
        if (stmt->kind == OASP_STMT_IF) {
            oasp_collect_global_slots(compiler, &stmt->body, capacity);
            oasp_collect_global_slots(compiler, &stmt->alternative, capacity);
        } else if (stmt->kind == OASP_STMT_WHILE || stmt->kind == OASP_STMT_DO || stmt->kind == OASP_STMT_FOR ||
                   stmt->kind == OASP_STMT_FOR_EACH || stmt->kind == OASP_STMT_WITH) {
            oasp_collect_global_slots(compiler, &stmt->body, capacity);
        } else if (stmt->kind == OASP_STMT_SELECT) {
            for (const oasp_case_branch *branch = stmt->cases; branch; branch = branch->next)
                oasp_collect_global_slots(compiler, &branch->body, capacity);
        }
    }
}

static void oasp_collect_local_slots(oasp_bytecode_compiler *compiler, const oasp_block *block) {
    for (const oasp_stmt *stmt = block->first; stmt; stmt = stmt->next) {
        if (stmt->kind == OASP_STMT_DIM) {
            for (size_t i = 0; i < stmt->declaration_count; i++)
                oasp_add_slot(compiler->local_slot_names, &compiler->local_slot_count, compiler->local_slot_capacity, stmt->declarations[i].name);
        } else if (stmt->kind == OASP_STMT_CONST) {
            for (size_t i = 0; i < stmt->constant_count; i++)
                oasp_add_slot(compiler->local_slot_names, &compiler->local_slot_count, compiler->local_slot_capacity, stmt->constants[i].name);
        } else if ((stmt->kind == OASP_STMT_FOR || stmt->kind == OASP_STMT_FOR_EACH) && !compiler->unresolved_globals_dynamic &&
                   oasp_global_slot_index(compiler, stmt->name) == SIZE_MAX) {
            oasp_add_slot(compiler->local_slot_names, &compiler->local_slot_count, compiler->local_slot_capacity, stmt->name);
        } else if (!compiler->object_routine && !compiler->unresolved_globals_dynamic &&
                   (stmt->kind == OASP_STMT_ASSIGN || stmt->kind == OASP_STMT_SET_ASSIGN) && oasp_simple_identifier(stmt->target) &&
                   oasp_global_slot_index(compiler, stmt->target->name) == SIZE_MAX) {
            oasp_add_slot(compiler->local_slot_names, &compiler->local_slot_count, compiler->local_slot_capacity, stmt->target->name);
        }
        if (stmt->kind == OASP_STMT_IF) {
            oasp_collect_local_slots(compiler, &stmt->body);
            oasp_collect_local_slots(compiler, &stmt->alternative);
        } else if (stmt->kind == OASP_STMT_WHILE || stmt->kind == OASP_STMT_DO || stmt->kind == OASP_STMT_FOR ||
                   stmt->kind == OASP_STMT_FOR_EACH || stmt->kind == OASP_STMT_WITH) {
            oasp_collect_local_slots(compiler, &stmt->body);
        } else if (stmt->kind == OASP_STMT_SELECT) {
            for (const oasp_case_branch *branch = stmt->cases; branch; branch = branch->next)
                oasp_collect_local_slots(compiler, &branch->body);
        }
    }
}

static size_t oasp_count_expr(const oasp_expr *expr) {
    if (!expr)
        return 1;
    size_t count = 1 + oasp_count_expr(expr->left) + oasp_count_expr(expr->right);
    for (size_t i = 0; i < expr->arg_count; i++)
        count += oasp_count_expr(expr->args[i]);
    return count;
}

static void oasp_count_expr_metadata(const oasp_expr *expr, size_t *names, size_t *constants) {
    if (!expr)
        return;
    if (expr->name.length)
        (*names)++;
    if (expr->kind == OASP_EXPR_LITERAL)
        (*constants)++;
    oasp_count_expr_metadata(expr->left, names, constants);
    oasp_count_expr_metadata(expr->right, names, constants);
    for (size_t i = 0; i < expr->arg_count; i++)
        oasp_count_expr_metadata(expr->args[i], names, constants);
}

static size_t oasp_count_block(const oasp_block *block) {
    size_t count = 0;
    for (const oasp_stmt *stmt = block->first; stmt; stmt = stmt->next) {
        switch (stmt->kind) {
        case OASP_STMT_DIM:
        case OASP_STMT_REDIM:
            for (size_t i = 0; i < stmt->declaration_count; i++) {
                count++;
                for (size_t j = 0; j < stmt->declarations[i].dimension_count; j++)
                    count += oasp_count_expr(stmt->declarations[i].dimensions[j]);
            }
            break;
        case OASP_STMT_CONST:
            for (size_t i = 0; i < stmt->constant_count; i++)
                count += oasp_count_expr(stmt->constants[i].value) + 1;
            break;
        case OASP_STMT_ERASE:
            for (size_t i = 0; i < stmt->expression_count; i++)
                count += oasp_count_expr(stmt->expressions[i]) + 1;
            break;
        case OASP_STMT_ASSIGN:
        case OASP_STMT_SET_ASSIGN:
            count += oasp_count_expr(stmt->value) + oasp_count_expr(stmt->target) + 1;
            break;
        case OASP_STMT_CALL:
            count += oasp_count_expr(stmt->value) + 1;
            break;
        case OASP_STMT_IF:
            count += oasp_count_expr(stmt->condition) + oasp_count_block(&stmt->body) + oasp_count_block(&stmt->alternative) + 2;
            break;
        case OASP_STMT_WHILE:
        case OASP_STMT_DO:
            count += (stmt->condition ? oasp_count_expr(stmt->condition) + 1 : 0) + oasp_count_block(&stmt->body) + 1;
            break;
        case OASP_STMT_FOR:
            count += oasp_count_expr(stmt->value) + oasp_count_expr(stmt->limit) + (stmt->step ? oasp_count_expr(stmt->step) : 0) +
                     oasp_count_block(&stmt->body) + 2;
            break;
        case OASP_STMT_FOR_EACH:
            count += oasp_count_expr(stmt->value) + oasp_count_block(&stmt->body) + 2;
            break;
        case OASP_STMT_SELECT:
            count += oasp_count_expr(stmt->value) + 3;
            for (const oasp_case_branch *branch = stmt->cases; branch; branch = branch->next) {
                for (size_t i = 0; i < branch->value_count; i++)
                    count += oasp_count_expr(branch->values[i]) + 3;
                count += oasp_count_block(&branch->body) + 3;
            }
            break;
        case OASP_STMT_WITH:
            count += oasp_count_expr(stmt->value) + oasp_count_block(&stmt->body) + 2;
            break;
        case OASP_STMT_ROUTINE:
        case OASP_STMT_CLASS:
        case OASP_STMT_NOOP:
            break;
        default:
            count++;
            break;
        }
    }
    return count;
}

static bool oasp_count_definition_instructions(const oasp_block *block, size_t *count) {
    for (const oasp_stmt *stmt = block->first; stmt; stmt = stmt->next) {
        if (stmt->kind == OASP_STMT_ROUTINE) {
            size_t body_count = oasp_count_block(&stmt->body);
            if (body_count >= SIZE_MAX - *count)
                return false;
            *count += body_count + 1;
            continue;
        }
        if (stmt->kind == OASP_STMT_CLASS) {
            size_t class_count = oasp_count_block(&stmt->body);
            if (class_count >= SIZE_MAX - *count)
                return false;
            *count += class_count + 1;
            for (const oasp_stmt *member = stmt->body.first; member; member = member->next) {
                if (member->kind != OASP_STMT_ROUTINE)
                    continue;
                size_t member_count = oasp_count_block(&member->body);
                if (member_count >= SIZE_MAX - *count)
                    return false;
                *count += member_count + 1;
            }
            continue;
        }
        if (!oasp_count_definition_instructions(&stmt->body, count) ||
            !oasp_count_definition_instructions(&stmt->alternative, count))
            return false;
        for (const oasp_case_branch *branch = stmt->cases; branch; branch = branch->next)
            if (!oasp_count_definition_instructions(&branch->body, count))
                return false;
    }
    return true;
}

static void oasp_count_metadata(const oasp_block *block, size_t *names, size_t *constants, size_t *routines, size_t *classes) {
    for (const oasp_stmt *stmt = block->first; stmt; stmt = stmt->next) {
        *names += 1 + stmt->name_count + stmt->declaration_count + stmt->constant_count + stmt->parameter_count;
        *constants += stmt->constant_count;
        if (stmt->kind == OASP_STMT_ROUTINE) {
            (*routines)++;
            oasp_count_metadata(&stmt->body, names, constants, routines, classes);
        } else if (stmt->kind == OASP_STMT_CLASS) {
            (*classes)++;
            oasp_count_metadata(&stmt->body, names, constants, routines, classes);
        }
        oasp_count_expr_metadata(stmt->target, names, constants);
        oasp_count_expr_metadata(stmt->value, names, constants);
        oasp_count_expr_metadata(stmt->condition, names, constants);
        oasp_count_expr_metadata(stmt->limit, names, constants);
        oasp_count_expr_metadata(stmt->step, names, constants);
        for (size_t i = 0; i < stmt->expression_count; i++)
            oasp_count_expr_metadata(stmt->expressions[i], names, constants);
        for (size_t i = 0; i < stmt->declaration_count; i++)
            for (size_t j = 0; j < stmt->declarations[i].dimension_count; j++)
                oasp_count_expr_metadata(stmt->declarations[i].dimensions[j], names, constants);
        for (size_t i = 0; i < stmt->constant_count; i++)
            oasp_count_expr_metadata(stmt->constants[i].value, names, constants);
        if (stmt->kind != OASP_STMT_ROUTINE && stmt->kind != OASP_STMT_CLASS) {
            oasp_count_metadata(&stmt->body, names, constants, routines, classes);
            oasp_count_metadata(&stmt->alternative, names, constants, routines, classes);
            for (const oasp_case_branch *branch = stmt->cases; branch; branch = branch->next) {
                for (size_t i = 0; i < branch->value_count; i++)
                    oasp_count_expr_metadata(branch->values[i], names, constants);
                oasp_count_metadata(&branch->body, names, constants, routines, classes);
            }
        }
    }
}

static uint32_t oasp_name_pool(oasp_bytecode_compiler *compiler, oasp_slice name) {
    size_t found = oasp_slot_index(compiler->bytecode->names, compiler->bytecode->name_count, name);
    if (found != SIZE_MAX)
        return found <= UINT32_MAX ? (uint32_t)found : OASP_BC_NONE;
    if (!name.length || compiler->bytecode->name_count >= compiler->name_capacity || compiler->bytecode->name_count > UINT32_MAX) {
        compiler->failed = true;
        return OASP_BC_NONE;
    }
    size_t index = compiler->bytecode->name_count++;
    compiler->bytecode->names[index] = name;
    return (uint32_t)index;
}

static bool oasp_value_same(const oasp_value *left, const oasp_value *right) {
    if (left->kind != right->kind)
        return false;
    switch (left->kind) {
    case OASP_VALUE_BOOL:
        return left->as.boolean == right->as.boolean;
    case OASP_VALUE_INT:
        return left->as.integer == right->as.integer;
    case OASP_VALUE_FLOAT:
        return left->as.floating == right->as.floating;
    case OASP_VALUE_STRING:
    case OASP_VALUE_DATE:
        return left->as.string.length == right->as.string.length &&
               (!left->as.string.length || !memcmp(left->as.string.data, right->as.string.data, left->as.string.length));
    default:
        return true;
    }
}

static uint32_t oasp_constant_pool(oasp_bytecode_compiler *compiler, const oasp_value *value) {
    for (size_t i = 0; i < compiler->bytecode->constant_count; i++)
        if (oasp_value_same(&compiler->bytecode->constants[i], value))
            return (uint32_t)i;
    if (compiler->bytecode->constant_count >= compiler->constant_capacity || compiler->bytecode->constant_count > UINT32_MAX) {
        compiler->failed = true;
        return OASP_BC_NONE;
    }
    size_t index = compiler->bytecode->constant_count++;
    compiler->bytecode->constants[index] = *value;
    return (uint32_t)index;
}

static size_t oasp_emit(oasp_bytecode_compiler *compiler, oasp_opcode opcode, size_t line) {
    if (compiler->failed || compiler->bytecode->length >= compiler->instruction_capacity ||
        compiler->bytecode->length > UINT32_MAX) {
        compiler->failed = true;
        oasp_instruction *sentinel = &compiler->bytecode->instructions[compiler->instruction_capacity];
        memset(sentinel, 0, sizeof(*sentinel));
        return compiler->instruction_capacity;
    }
    size_t pc = compiler->bytecode->length++;
    oasp_instruction *instruction = &compiler->bytecode->instructions[pc];
    memset(instruction, 0xff, sizeof(*instruction));
    instruction->opcode = (uint8_t)opcode;
    instruction->flags = 0;
    instruction->reserved = 0;
    instruction->line = line > UINT32_MAX ? UINT32_MAX : (uint32_t)line;
    instruction->slot = 0;
    return pc;
}

static bool oasp_expr_is_unary(oasp_expr_kind kind) {
    return kind == OASP_EXPR_NEGATE || kind == OASP_EXPR_POSITIVE || kind == OASP_EXPR_NOT;
}

static bool oasp_expr_is_binary(oasp_expr_kind kind) { return kind >= OASP_EXPR_CONCAT && kind <= OASP_EXPR_IS_NOT; }

static oasp_opcode oasp_expr_opcode(oasp_expr_kind kind) {
    static const oasp_opcode opcodes[] = {
        [OASP_EXPR_NEGATE] = OASP_BC_NEGATE,
        [OASP_EXPR_POSITIVE] = OASP_BC_POSITIVE,
        [OASP_EXPR_NOT] = OASP_BC_NOT,
        [OASP_EXPR_CONCAT] = OASP_BC_CONCAT,
        [OASP_EXPR_EQ] = OASP_BC_EQ,
        [OASP_EXPR_NE] = OASP_BC_NE,
        [OASP_EXPR_LT] = OASP_BC_LT,
        [OASP_EXPR_LE] = OASP_BC_LE,
        [OASP_EXPR_GT] = OASP_BC_GT,
        [OASP_EXPR_GE] = OASP_BC_GE,
        [OASP_EXPR_ADD] = OASP_BC_ADD,
        [OASP_EXPR_SUBTRACT] = OASP_BC_SUBTRACT,
        [OASP_EXPR_MULTIPLY] = OASP_BC_MULTIPLY,
        [OASP_EXPR_DIVIDE] = OASP_BC_DIVIDE,
        [OASP_EXPR_INT_DIVIDE] = OASP_BC_INT_DIVIDE,
        [OASP_EXPR_MOD] = OASP_BC_MOD,
        [OASP_EXPR_POWER] = OASP_BC_POWER,
        [OASP_EXPR_AND] = OASP_BC_AND,
        [OASP_EXPR_OR] = OASP_BC_OR,
        [OASP_EXPR_XOR] = OASP_BC_XOR,
        [OASP_EXPR_EQV] = OASP_BC_EQV,
        [OASP_EXPR_IMP] = OASP_BC_IMP,
        [OASP_EXPR_IS] = OASP_BC_IS,
        [OASP_EXPR_IS_NOT] = OASP_BC_IS_NOT,
    };
    return kind < sizeof(opcodes) / sizeof(opcodes[0]) ? opcodes[kind] : OASP_BC_COUNT;
}

static oasp_value_kind oasp_expr_static_type(const oasp_bytecode_compiler *compiler, const oasp_expr *expr) {
    if (!expr)
        return OASP_VALUE_EMPTY;
    if (expr->kind == OASP_EXPR_LITERAL)
        return expr->literal.kind;
    if (expr->kind == OASP_EXPR_CONCAT)
        return OASP_VALUE_STRING;
    if (expr->kind >= OASP_EXPR_EQ && expr->kind <= OASP_EXPR_GE)
        return OASP_VALUE_BOOL;
    if (expr->kind == OASP_EXPR_CALL && expr->left && expr->left->kind == OASP_EXPR_IDENTIFIER) {
        oasp_symbol symbol;
        if (oasp_resolved_symbol(compiler, expr->left, &symbol) && symbol.kind == OASP_SYMBOL_BUILTIN) {
            switch ((oasp_builtin_id)symbol.slot) {
            case OASP_BUILTIN_CSTR:
            case OASP_BUILTIN_SERVER_HTMLENCODE:
            case OASP_BUILTIN_SERVER_URLENCODE:
            case OASP_BUILTIN_TRIM:
            case OASP_BUILTIN_LCASE:
            case OASP_BUILTIN_UCASE:
            case OASP_BUILTIN_REPLACE:
                return OASP_VALUE_STRING;
            case OASP_BUILTIN_LEN:
                return OASP_VALUE_INT;
            case OASP_BUILTIN_ISEMPTY:
                return OASP_VALUE_BOOL;
            default:
                break;
            }
        }
    }
    return OASP_VALUE_EMPTY;
}

static bool oasp_fold_compare(oasp_expr_kind kind, const oasp_value *left, const oasp_value *right, oasp_value *result) {
    int comparison;
    if (left->kind == OASP_VALUE_INT && right->kind == OASP_VALUE_INT) {
        comparison = left->as.integer < right->as.integer ? -1 : left->as.integer > right->as.integer ? 1 : 0;
    } else if (left->kind == OASP_VALUE_FLOAT && right->kind == OASP_VALUE_FLOAT) {
        comparison = left->as.floating < right->as.floating ? -1 : left->as.floating > right->as.floating ? 1 : 0;
    } else if (left->kind == OASP_VALUE_STRING && right->kind == OASP_VALUE_STRING) {
        size_t common = left->as.string.length < right->as.string.length ? left->as.string.length : right->as.string.length;
        comparison = common ? memcmp(left->as.string.data, right->as.string.data, common) : 0;
        if (!comparison)
            comparison = left->as.string.length < right->as.string.length ? -1
                         : left->as.string.length > right->as.string.length ? 1
                                                                           : 0;
    } else if (left->kind == OASP_VALUE_BOOL && right->kind == OASP_VALUE_BOOL) {
        comparison = left->as.boolean < right->as.boolean ? -1 : left->as.boolean > right->as.boolean ? 1 : 0;
    } else {
        return false;
    }
    bool value = kind == OASP_EXPR_EQ ? comparison == 0
                 : kind == OASP_EXPR_NE ? comparison != 0
                 : kind == OASP_EXPR_LT ? comparison < 0
                 : kind == OASP_EXPR_LE ? comparison <= 0
                 : kind == OASP_EXPR_GT ? comparison > 0
                                        : comparison >= 0;
    *result = oasp_value_bool(value);
    return true;
}

static bool oasp_fold_literal(oasp_bytecode_compiler *compiler, const oasp_expr *expr, oasp_value *result) {
    if (!expr)
        return false;
    if (expr->kind == OASP_EXPR_LITERAL) {
        *result = expr->literal;
        return true;
    }
    oasp_value left;
    if (oasp_expr_is_unary(expr->kind)) {
        if (!oasp_fold_literal(compiler, expr->left, &left))
            return false;
        if (expr->kind == OASP_EXPR_POSITIVE && (left.kind == OASP_VALUE_INT || left.kind == OASP_VALUE_FLOAT)) {
            *result = left;
            return true;
        }
        if (expr->kind == OASP_EXPR_NEGATE && left.kind == OASP_VALUE_INT && left.as.integer != INT64_MIN) {
            *result = oasp_value_int(-left.as.integer);
            return true;
        }
        if (expr->kind == OASP_EXPR_NEGATE && left.kind == OASP_VALUE_FLOAT) {
            *result = oasp_value_float(-left.as.floating);
            return true;
        }
        if (expr->kind == OASP_EXPR_NOT && left.kind == OASP_VALUE_INT) {
            *result = oasp_value_int(~left.as.integer);
            return true;
        }
        return false;
    }
    if (!oasp_expr_is_binary(expr->kind))
        return false;
    oasp_value right;
    if (!oasp_fold_literal(compiler, expr->left, &left) || !oasp_fold_literal(compiler, expr->right, &right))
        return false;
    if (expr->kind >= OASP_EXPR_EQ && expr->kind <= OASP_EXPR_GE)
        return oasp_fold_compare(expr->kind, &left, &right, result);
    if (expr->kind == OASP_EXPR_CONCAT && left.kind == OASP_VALUE_STRING && right.kind == OASP_VALUE_STRING) {
        if (left.as.string.length > SIZE_MAX - right.as.string.length)
            return false;
        size_t length = left.as.string.length + right.as.string.length;
        char *data = oasp_arena_alloc(compiler->arena, length + 1, _Alignof(char));
        if (!data)
            return false;
        memcpy(data, left.as.string.data, left.as.string.length);
        memcpy(data + left.as.string.length, right.as.string.data, right.as.string.length);
        data[length] = '\0';
        *result = oasp_value_string(data, length);
        return true;
    }
    if (left.kind != OASP_VALUE_INT || right.kind != OASP_VALUE_INT)
        return false;
    int64_t value;
    switch (expr->kind) {
    case OASP_EXPR_ADD:
        if (__builtin_add_overflow(left.as.integer, right.as.integer, &value))
            return false;
        break;
    case OASP_EXPR_SUBTRACT:
        if (__builtin_sub_overflow(left.as.integer, right.as.integer, &value))
            return false;
        break;
    case OASP_EXPR_MULTIPLY:
        if (__builtin_mul_overflow(left.as.integer, right.as.integer, &value))
            return false;
        break;
    case OASP_EXPR_INT_DIVIDE:
        if (!right.as.integer || (left.as.integer == INT64_MIN && right.as.integer == -1))
            return false;
        value = left.as.integer / right.as.integer;
        break;
    case OASP_EXPR_MOD:
        if (!right.as.integer || (left.as.integer == INT64_MIN && right.as.integer == -1))
            return false;
        value = left.as.integer % right.as.integer;
        break;
    case OASP_EXPR_AND:
        value = left.as.integer & right.as.integer;
        break;
    case OASP_EXPR_OR:
        value = left.as.integer | right.as.integer;
        break;
    case OASP_EXPR_XOR:
        value = left.as.integer ^ right.as.integer;
        break;
    case OASP_EXPR_EQV:
        value = ~(left.as.integer ^ right.as.integer);
        break;
    case OASP_EXPR_IMP:
        value = ~left.as.integer | right.as.integer;
        break;
    default:
        return false;
    }
    *result = oasp_value_int(value);
    return true;
}

static void oasp_compile_expr(oasp_bytecode_compiler *compiler, const oasp_expr *expr);

static size_t oasp_compile_concat_terms(oasp_bytecode_compiler *compiler, const oasp_expr *expr) {
    if (expr && expr->kind == OASP_EXPR_CONCAT)
        return oasp_compile_concat_terms(compiler, expr->left) + oasp_compile_concat_terms(compiler, expr->right);
    oasp_compile_expr(compiler, expr);
    return 1;
}

static bool oasp_compile_recordset_field(oasp_bytecode_compiler *compiler, const oasp_expr *expr, oasp_opcode opcode) {
    if (!expr || expr->kind != OASP_EXPR_CALL || expr->arg_count != 1 || !expr->args[0] ||
        expr->args[0]->kind != OASP_EXPR_LITERAL ||
        expr->args[0]->literal.kind != OASP_VALUE_STRING || !expr->left)
        return false;
    oasp_slice member = {0};
    oasp_slice receiver = {0};
    bool member_call = false;
    if (expr->left->kind == OASP_EXPR_IDENTIFIER) {
        if (oasp_dotted_parts(expr->left->name, &receiver, &member)) {
            if (!oasp_ascii_equal(member, "fields") && !oasp_ascii_equal(member, "item"))
                return false;
            member_call = true;
        } else {
            receiver = expr->left->name;
        }
    } else if (expr->left->kind == OASP_EXPR_MEMBER && oasp_simple_identifier(expr->left->left)) {
        receiver = expr->left->left->name;
        member = expr->left->name;
        if (!oasp_ascii_equal(member, "fields") && !oasp_ascii_equal(member, "item"))
            return false;
        member_call = true;
    } else {
        return false;
    }
    if (!member_call && (oasp_ascii_equal(receiver, "request") || oasp_ascii_equal(receiver, "session") ||
                         oasp_ascii_equal(receiver, "application")))
        return false;
    size_t local = oasp_local_slot_index(compiler, receiver);
    size_t global = compiler->in_routine ? SIZE_MAX : oasp_global_slot_index(compiler, receiver);
    if ((local == SIZE_MAX && global == SIZE_MAX) ||
        (local != SIZE_MAX && local >= INT32_MAX) ||
        (global != SIZE_MAX && global >= INT32_MAX))
        return false;
    size_t pc = oasp_emit(compiler, opcode, expr->line);
    compiler->bytecode->instructions[pc].slot =
        local != SIZE_MAX ? (int32_t)local + 1 : -(int32_t)global - 1;
    compiler->bytecode->instructions[pc].operand =
        oasp_constant_pool(compiler, &expr->args[0]->literal);
    if (member_call) {
        compiler->bytecode->instructions[pc].flags = OASP_BC_FLAG_ROOT_SLOT;
        compiler->bytecode->instructions[pc].target = oasp_name_pool(compiler, member);
        compiler->bytecode->instructions[pc].reserved =
            (uint16_t)oasp_member_id_for_name(member);
    } else {
        compiler->bytecode->instructions[pc].target = OASP_BC_NONE;
    }
    return !compiler->failed;
}

#if !defined(OASP_DISABLE_RENDER_PLAN)
static size_t oasp_collect_concat_terms(const oasp_expr *expr, const oasp_expr **terms, size_t capacity) {
    if (!expr || !capacity)
        return 0;
    if (expr->kind != OASP_EXPR_CONCAT) {
        terms[0] = expr;
        return 1;
    }
    size_t left = oasp_collect_concat_terms(expr->left, terms, capacity);
    if (!left || left >= capacity)
        return 0;
    size_t right = oasp_collect_concat_terms(expr->right, terms + left, capacity - left);
    return right ? left + right : 0;
}

static const oasp_stmt *oasp_static_routine(const oasp_bytecode_compiler *compiler, oasp_slice name) {
    if (!compiler || !compiler->program || compiler->object_routine)
        return NULL;
    for (const oasp_stmt *stmt = compiler->program->first; stmt; stmt = stmt->next)
        if (stmt->kind == OASP_STMT_ROUTINE && oasp_name_equal(stmt->name, name))
            return stmt;
    return NULL;
}

static bool oasp_render_field_candidate(const oasp_bytecode_compiler *compiler, const oasp_expr *expr) {
    if (!expr || expr->kind != OASP_EXPR_CALL || expr->arg_count != 1 || !expr->args[0] ||
        expr->args[0]->kind != OASP_EXPR_LITERAL || expr->args[0]->literal.kind != OASP_VALUE_STRING || !expr->left)
        return false;
    oasp_slice receiver = {0};
    oasp_slice member = {0};
    bool member_call = false;
    if (expr->left->kind == OASP_EXPR_IDENTIFIER) {
        if (oasp_dotted_parts(expr->left->name, &receiver, &member)) {
            if (!oasp_ascii_equal(member, "fields") && !oasp_ascii_equal(member, "item"))
                return false;
            member_call = true;
        } else {
            receiver = expr->left->name;
        }
    } else if (expr->left->kind == OASP_EXPR_MEMBER && oasp_simple_identifier(expr->left->left)) {
        receiver = expr->left->left->name;
        member_call = oasp_ascii_equal(expr->left->name, "fields") || oasp_ascii_equal(expr->left->name, "item");
    } else {
        return false;
    }
    if (!member_call && (oasp_ascii_equal(receiver, "request") || oasp_ascii_equal(receiver, "session") ||
                         oasp_ascii_equal(receiver, "application")))
        return false;
    return oasp_local_slot_index(compiler, receiver) != SIZE_MAX ||
           (!compiler->in_routine && oasp_global_slot_index(compiler, receiver) != SIZE_MAX);
}

static bool oasp_render_transform(const oasp_bytecode_compiler *compiler, const oasp_expr *expr, const oasp_expr **field, uint8_t *flags);

static bool oasp_render_wrapper_transform(const oasp_bytecode_compiler *compiler, const oasp_expr *expr, const oasp_expr **field, uint8_t *flags) {
    if (!expr || expr->kind != OASP_EXPR_CALL || expr->arg_count != 1 || !expr->left ||
        expr->left->kind != OASP_EXPR_IDENTIFIER)
        return false;
    const oasp_stmt *routine = oasp_static_routine(compiler, expr->left->name);
    if (!routine || routine->routine_kind != OASP_ROUTINE_FUNCTION || routine->parameter_count != 1 ||
        !routine->body.first || routine->body.first != routine->body.last ||
        routine->body.first->kind != OASP_STMT_ASSIGN || !routine->body.first->target ||
        routine->body.first->target->kind != OASP_EXPR_IDENTIFIER ||
        !oasp_name_equal(routine->body.first->target->name, routine->name))
        return false;
    const oasp_expr *encoded = routine->body.first->value;
    if (!encoded || encoded->kind != OASP_EXPR_CALL || encoded->arg_count != 1 || !encoded->left ||
        encoded->left->kind != OASP_EXPR_IDENTIFIER || !oasp_ascii_equal(encoded->left->name, "server.htmlencode"))
        return false;
    const oasp_expr *converted = encoded->args[0];
    if (!converted || converted->kind != OASP_EXPR_CALL || converted->arg_count != 1 || !converted->left ||
        converted->left->kind != OASP_EXPR_IDENTIFIER || !oasp_ascii_equal(converted->left->name, "cstr") ||
        !converted->args[0] || converted->args[0]->kind != OASP_EXPR_IDENTIFIER ||
        !oasp_name_equal(converted->args[0]->name, routine->parameters[0].name))
        return false;
    if (!oasp_render_transform(compiler, expr->args[0], field, flags))
        return false;
    *flags |= OASP_BC_FLAG_RENDER_CSTR | OASP_BC_FLAG_RENDER_HTMLENCODE;
    return true;
}

static bool oasp_render_transform(const oasp_bytecode_compiler *compiler, const oasp_expr *expr, const oasp_expr **field, uint8_t *flags) {
    if (!expr || !field || !flags)
        return false;
    if (expr->kind == OASP_EXPR_CALL && expr->left && expr->left->kind == OASP_EXPR_IDENTIFIER &&
        expr->arg_count == 1 && oasp_ascii_equal(expr->left->name, "cstr")) {
        if (!oasp_render_transform(compiler, expr->args[0], field, flags))
            return false;
        *flags |= OASP_BC_FLAG_RENDER_CSTR;
        return true;
    }
    if (expr->kind == OASP_EXPR_CALL && expr->left && expr->left->kind == OASP_EXPR_IDENTIFIER &&
        expr->arg_count == 1 && oasp_ascii_equal(expr->left->name, "server.htmlencode")) {
        if (!oasp_render_transform(compiler, expr->args[0], field, flags))
            return false;
        *flags |= OASP_BC_FLAG_RENDER_HTMLENCODE;
        return true;
    }
    if (oasp_render_wrapper_transform(compiler, expr, field, flags))
        return true;
    if (!oasp_render_field_candidate(compiler, expr))
        return false;
    *field = expr;
    return true;
}

static bool oasp_compile_render_response(oasp_bytecode_compiler *compiler, const oasp_stmt *stmt) {
    const oasp_expr *call = stmt ? stmt->value : NULL;
    const oasp_expr *terms[3] = {0};
    if (!call || call->kind != OASP_EXPR_CALL || call->arg_count != 1 || !call->left ||
        call->left->kind != OASP_EXPR_IDENTIFIER || !oasp_ascii_equal(call->left->name, "response.write") ||
        oasp_collect_concat_terms(call->args[0], terms, OASP_ARRAY_COUNT(terms)) != OASP_ARRAY_COUNT(terms) ||
        terms[0]->kind != OASP_EXPR_LITERAL || terms[0]->literal.kind != OASP_VALUE_STRING ||
        terms[2]->kind != OASP_EXPR_LITERAL || terms[2]->literal.kind != OASP_VALUE_STRING)
        return false;
    const oasp_expr *field = NULL;
    uint8_t flags = 0;
    if (!oasp_render_transform(compiler, terms[1], &field, &flags))
        return false;
    size_t before = compiler->bytecode->length;
    if (!oasp_compile_recordset_field(compiler, field, OASP_BC_WRITE_RENDER_RS_FIELD))
        return false;
    oasp_instruction *instruction = &compiler->bytecode->instructions[before];
    instruction->target = oasp_constant_pool(compiler, &terms[0]->literal);
    instruction->error_target = oasp_constant_pool(compiler, &terms[2]->literal);
    instruction->flags |= flags;
    return !compiler->failed;
}
#endif

static void oasp_compile_expr(oasp_bytecode_compiler *compiler, const oasp_expr *expr) {
    if (!expr) {
        (void)oasp_emit(compiler, OASP_BC_PUSH_EMPTY, 0);
        return;
    }
    oasp_value folded;
    if (expr->kind != OASP_EXPR_LITERAL && oasp_fold_literal(compiler, expr, &folded)) {
        size_t folded_pc = oasp_emit(compiler, OASP_BC_PUSH_LITERAL, expr->line);
        compiler->bytecode->instructions[folded_pc].operand = oasp_constant_pool(compiler, &folded);
        return;
    }
    if (oasp_expr_is_unary(expr->kind)) {
        oasp_compile_expr(compiler, expr->left);
        (void)oasp_emit(compiler, oasp_expr_opcode(expr->kind), expr->line);
        return;
    }
    if (expr->kind == OASP_EXPR_CONCAT) {
        if (oasp_expr_static_type(compiler, expr->left) == OASP_VALUE_STRING &&
            oasp_expr_static_type(compiler, expr->right) == OASP_VALUE_STRING) {
            oasp_compile_expr(compiler, expr->left);
            oasp_compile_expr(compiler, expr->right);
            (void)oasp_emit(compiler, OASP_BC_CONCAT_STRING, expr->line);
        } else {
            size_t count = oasp_compile_concat_terms(compiler, expr);
            size_t pc = oasp_emit(compiler, OASP_BC_CONCAT_N, expr->line);
            compiler->bytecode->instructions[pc].operand = count > UINT32_MAX ? OASP_BC_NONE : (uint32_t)count;
        }
        return;
    }
    if (oasp_expr_is_binary(expr->kind)) {
        oasp_compile_expr(compiler, expr->left);
        oasp_compile_expr(compiler, expr->right);
        oasp_opcode opcode = oasp_expr_opcode(expr->kind);
        if (expr->kind == OASP_EXPR_ADD && oasp_expr_static_type(compiler, expr->left) == OASP_VALUE_INT &&
            oasp_expr_static_type(compiler, expr->right) == OASP_VALUE_INT)
            opcode = OASP_BC_ADD_I64;
        else if (expr->kind >= OASP_EXPR_EQ && expr->kind <= OASP_EXPR_GE &&
                 oasp_expr_static_type(compiler, expr->left) == OASP_VALUE_STRING &&
                 oasp_expr_static_type(compiler, expr->right) == OASP_VALUE_STRING)
            opcode = OASP_BC_COMPARE_STRING;
        size_t operation_pc = oasp_emit(compiler, opcode, expr->line);
        if (opcode == OASP_BC_COMPARE_STRING)
            compiler->bytecode->instructions[operation_pc].flags = (uint8_t)expr->kind;
        return;
    }
    size_t pc;
    switch (expr->kind) {
    case OASP_EXPR_EMPTY:
        (void)oasp_emit(compiler, OASP_BC_PUSH_EMPTY, expr->line);
        break;
    case OASP_EXPR_LITERAL:
        pc = oasp_emit(compiler, OASP_BC_PUSH_LITERAL, expr->line);
        compiler->bytecode->instructions[pc].operand = oasp_constant_pool(compiler, &expr->literal);
        break;
    case OASP_EXPR_IDENTIFIER: {
        oasp_slice direct_member;
        int32_t direct_slot;
        if (oasp_dotted_root_slot(compiler, expr->name, &direct_slot, &direct_member) &&
            oasp_ascii_equal(direct_member, "eof")) {
            pc = oasp_emit(compiler, OASP_BC_RECORDSET_EOF, expr->line);
            compiler->bytecode->instructions[pc].slot = direct_slot;
            compiler->bytecode->instructions[pc].operand = oasp_name_pool(compiler, expr->name);
            compiler->bytecode->instructions[pc].reserved = OASP_MEMBER_EOF;
            break;
        }
        size_t slot = oasp_local_slot_index(compiler, expr->name);
        if (slot != SIZE_MAX) {
            pc = oasp_emit(compiler, OASP_BC_PUSH_LOCAL_SLOT, expr->line);
            compiler->bytecode->instructions[pc].operand = (uint32_t)slot;
            break;
        }
        slot = compiler->in_routine ? SIZE_MAX : oasp_global_slot_index(compiler, expr->name);
        if (slot != SIZE_MAX) {
            pc = oasp_emit(compiler, OASP_BC_PUSH_GLOBAL_SLOT, expr->line);
            compiler->bytecode->instructions[pc].operand = (uint32_t)slot;
            break;
        }
        pc = oasp_emit(compiler, OASP_BC_PUSH_NAME, expr->line);
        compiler->bytecode->instructions[pc].operand = oasp_name_pool(compiler, expr->name);
        oasp_slice member;
        int32_t root_slot;
        if (oasp_dotted_root_slot(compiler, expr->name, &root_slot, &member)) {
            compiler->bytecode->instructions[pc].flags |= OASP_BC_FLAG_ROOT_SLOT;
            compiler->bytecode->instructions[pc].slot = root_slot;
            compiler->bytecode->instructions[pc].reserved = (uint16_t)oasp_member_id_for_name(member);
        } else {
            compiler->bytecode->instructions[pc].reserved = (uint16_t)oasp_dotted_member_id(expr->name);
        }
        break;
    }
    case OASP_EXPR_NEW:
        pc = oasp_emit(compiler, OASP_BC_NEW, expr->line);
        compiler->bytecode->instructions[pc].operand = oasp_name_pool(compiler, expr->name);
        break;
    case OASP_EXPR_MEMBER:
        oasp_compile_expr(compiler, expr->left);
        pc = oasp_emit(compiler, OASP_BC_GET_MEMBER, expr->line);
        compiler->bytecode->instructions[pc].operand = oasp_name_pool(compiler, expr->name);
        compiler->bytecode->instructions[pc].reserved = (uint16_t)oasp_member_id_for_name(expr->name);
        break;
    case OASP_EXPR_CALL:
        if (oasp_compile_recordset_field(compiler, expr, OASP_BC_RECORDSET_FIELD))
            break;
        if (expr->left && expr->left->kind == OASP_EXPR_CALL && expr->left->left &&
            expr->left->left->kind == OASP_EXPR_IDENTIFIER &&
            oasp_ascii_equal(expr->left->left->name, "request.cookies") &&
            expr->left->arg_count == 1 && expr->arg_count == 1) {
            oasp_compile_expr(compiler, expr->left->args[0]);
            oasp_compile_expr(compiler, expr->args[0]);
            pc = oasp_emit(compiler, OASP_BC_CALL_NAME, expr->line);
            compiler->bytecode->instructions[pc].operand =
                oasp_name_pool(compiler, (oasp_slice){"request.cookies.subkey", 22});
            compiler->bytecode->instructions[pc].target = 2;
        } else if (expr->left && expr->left->kind == OASP_EXPR_MEMBER) {
            oasp_compile_expr(compiler, expr->left->left);
            for (size_t i = 0; i < expr->arg_count; i++)
                oasp_compile_expr(compiler, expr->args[i]);
            pc = oasp_emit(compiler, OASP_BC_LOAD_MEMBER_CALL_MEMBER, expr->line);
            compiler->bytecode->instructions[pc].operand = oasp_name_pool(compiler, expr->left->name);
            compiler->bytecode->instructions[pc].reserved =
                (uint16_t)oasp_member_id_for_name(expr->left->name);
        } else if (expr->left && expr->left->kind == OASP_EXPR_IDENTIFIER) {
            oasp_symbol symbol;
            bool resolved = oasp_resolved_symbol(compiler, expr->left, &symbol);
            size_t local = oasp_local_slot_index(compiler, expr->left->name);
            size_t global = compiler->in_routine ? SIZE_MAX : oasp_global_slot_index(compiler, expr->left->name);
            size_t routine_index = oasp_static_routine_index(compiler, expr->left->name);
            if (resolved && symbol.kind == OASP_SYMBOL_BUILTIN && symbol.slot != OASP_BUILTIN_DYNAMIC &&
                expr->arg_count == 1 && expr->args[0] && expr->args[0]->kind == OASP_EXPR_IDENTIFIER) {
                size_t argument_local = oasp_local_slot_index(compiler, expr->args[0]->name);
                if (argument_local != SIZE_MAX) {
                    pc = oasp_emit(compiler, OASP_BC_LOAD_LOCAL_CALL_BUILTIN, expr->line);
                    compiler->bytecode->instructions[pc].operand = (uint32_t)argument_local;
                    compiler->bytecode->instructions[pc].reserved = (uint16_t)symbol.slot;
                    compiler->bytecode->instructions[pc].error_target = oasp_name_pool(compiler, expr->left->name);
                    compiler->bytecode->instructions[pc].target = 1;
                    break;
                }
            }
            if (routine_index == SIZE_MAX &&
                (!resolved || (symbol.kind != OASP_SYMBOL_BUILTIN &&
                               symbol.kind != OASP_SYMBOL_ROUTINE &&
                               symbol.kind != OASP_SYMBOL_CLASS_METHOD)) &&
                (local != SIZE_MAX || global != SIZE_MAX)) {
                pc = oasp_emit(compiler, local != SIZE_MAX ? OASP_BC_PUSH_LOCAL_SLOT : OASP_BC_PUSH_GLOBAL_SLOT, expr->line);
                compiler->bytecode->instructions[pc].operand = (uint32_t)(local != SIZE_MAX ? local : global);
                for (size_t i = 0; i < expr->arg_count; i++)
                    oasp_compile_expr(compiler, expr->args[i]);
                pc = oasp_emit(compiler, OASP_BC_CALL_DEFAULT, expr->line);
                compiler->bytecode->instructions[pc].target = (uint32_t)expr->arg_count;
                break;
            }
            for (size_t i = 0; i < expr->arg_count; i++)
                oasp_compile_expr(compiler, expr->args[i]);
            pc = oasp_emit(compiler, OASP_BC_CALL_NAME, expr->line);
            compiler->bytecode->instructions[pc].operand = oasp_name_pool(compiler, expr->left->name);
            if (routine_index < INT32_MAX) {
                compiler->bytecode->instructions[pc].slot = (int32_t)routine_index + 1;
            } else if (resolved && symbol.kind == OASP_SYMBOL_BUILTIN && symbol.slot != OASP_BUILTIN_DYNAMIC) {
                compiler->bytecode->instructions[pc].flags |= OASP_BC_FLAG_STATIC_BUILTIN;
                compiler->bytecode->instructions[pc].reserved = (uint16_t)symbol.slot;
            } else {
                oasp_slice member;
                int32_t root_slot;
                if (oasp_dotted_root_slot(compiler, expr->left->name, &root_slot, &member)) {
                    oasp_member_id member_id = oasp_member_id_for_name(member);
                    compiler->bytecode->instructions[pc].flags |= OASP_BC_FLAG_ROOT_SLOT;
                    if (member_id != OASP_MEMBER_DYNAMIC)
                        compiler->bytecode->instructions[pc].flags |= OASP_BC_FLAG_STATIC_MEMBER;
                    compiler->bytecode->instructions[pc].slot = root_slot;
                    compiler->bytecode->instructions[pc].reserved = (uint16_t)member_id;
                } else if (oasp_dotted_member_id(expr->left->name) != OASP_MEMBER_DYNAMIC) {
                    compiler->bytecode->instructions[pc].flags |= OASP_BC_FLAG_STATIC_MEMBER;
                    compiler->bytecode->instructions[pc].reserved =
                        (uint16_t)oasp_dotted_member_id(expr->left->name);
                } else if (!memchr(expr->left->name.data, '.', expr->left->name.length) &&
                           !oasp_ascii_equal(expr->left->name, "execute") &&
                           !oasp_ascii_equal(expr->left->name, "executeglobal") &&
                           !oasp_ascii_equal(expr->left->name, "eval")) {
                    compiler->bytecode->instructions[pc].flags |= OASP_BC_FLAG_SYMBOL_HINT;
                    compiler->bytecode->instructions[pc].error_target = oasp_symbol_id(expr->left->name);
                }
            }
        } else {
            oasp_compile_expr(compiler, expr->left);
            for (size_t i = 0; i < expr->arg_count; i++)
                oasp_compile_expr(compiler, expr->args[i]);
            pc = oasp_emit(compiler, OASP_BC_CALL_DEFAULT, expr->line);
        }
        size_t argument_count = expr->left && expr->left->kind == OASP_EXPR_CALL && expr->left->left &&
                                        expr->left->left->kind == OASP_EXPR_IDENTIFIER &&
                                        oasp_ascii_equal(expr->left->left->name, "request.cookies") &&
                                        expr->left->arg_count == 1 && expr->arg_count == 1
                                    ? 2
                                    : expr->arg_count;
        compiler->bytecode->instructions[pc].target =
            argument_count > UINT32_MAX ? OASP_BC_NONE : (uint32_t)argument_count;
        break;
    default:
        compiler->failed = true;
        break;
    }
}

static bool oasp_compile_assign(oasp_bytecode_compiler *compiler, const oasp_expr *target, size_t line) {
    size_t pc;
    if (!target)
        return false;
    if (target->kind == OASP_EXPR_IDENTIFIER) {
        size_t local = oasp_local_slot_index(compiler, target->name);
        size_t global = compiler->in_routine ? SIZE_MAX : oasp_global_slot_index(compiler, target->name);
        pc = oasp_emit(compiler, local != SIZE_MAX ? OASP_BC_ASSIGN_LOCAL_SLOT
                                                   : global != SIZE_MAX ? OASP_BC_ASSIGN_GLOBAL_SLOT : OASP_BC_ASSIGN_NAME,
                       line);
        compiler->bytecode->instructions[pc].operand =
            (uint32_t)(local != SIZE_MAX ? local : global != SIZE_MAX ? global : oasp_name_pool(compiler, target->name));
        return !compiler->failed;
    }
    if (target->kind == OASP_EXPR_MEMBER) {
        if (target->left && target->left->kind == OASP_EXPR_CALL && target->left->left &&
            target->left->left->kind == OASP_EXPR_IDENTIFIER &&
            oasp_ascii_equal(target->left->left->name, "response.cookies")) {
            for (size_t i = 0; i < target->left->arg_count; i++)
                oasp_compile_expr(compiler, target->left->args[i]);
            pc = oasp_emit(compiler, OASP_BC_ASSIGN_COOKIE_ATTRIBUTE, line);
            compiler->bytecode->instructions[pc].operand = oasp_name_pool(compiler, target->name);
            compiler->bytecode->instructions[pc].target = (uint32_t)target->left->arg_count;
            return !compiler->failed;
        }
        oasp_compile_expr(compiler, target->left);
        pc = oasp_emit(compiler, OASP_BC_ASSIGN_MEMBER, line);
        compiler->bytecode->instructions[pc].operand = oasp_name_pool(compiler, target->name);
        return !compiler->failed;
    }
    if (target->kind == OASP_EXPR_CALL && target->left && target->left->kind == OASP_EXPR_IDENTIFIER) {
        oasp_slice member;
        int32_t root_slot;
        if (oasp_dotted_root_slot(compiler, target->left->name, &root_slot, &member)) {
            pc = oasp_emit(compiler, root_slot > 0 ? OASP_BC_PUSH_LOCAL_SLOT : OASP_BC_PUSH_GLOBAL_SLOT, line);
            compiler->bytecode->instructions[pc].operand =
                (uint32_t)(root_slot > 0 ? root_slot - 1 : -root_slot - 1);
            for (size_t i = 0; i < target->arg_count; i++)
                oasp_compile_expr(compiler, target->args[i]);
            pc = oasp_emit(compiler, OASP_BC_ASSIGN_INDEX_MEMBER, line);
            compiler->bytecode->instructions[pc].operand = oasp_name_pool(compiler, member);
            compiler->bytecode->instructions[pc].target = (uint32_t)target->arg_count;
            return !compiler->failed;
        }
        size_t local = oasp_local_slot_index(compiler, target->left->name);
        size_t global = compiler->in_routine ? SIZE_MAX : oasp_global_slot_index(compiler, target->left->name);
        if (local != SIZE_MAX || global != SIZE_MAX) {
            pc = oasp_emit(compiler, local != SIZE_MAX ? OASP_BC_PUSH_LOCAL_SLOT : OASP_BC_PUSH_GLOBAL_SLOT, line);
            compiler->bytecode->instructions[pc].operand = (uint32_t)(local != SIZE_MAX ? local : global);
            for (size_t i = 0; i < target->arg_count; i++)
                oasp_compile_expr(compiler, target->args[i]);
            pc = oasp_emit(compiler, OASP_BC_ASSIGN_INDEX_DEFAULT, line);
            compiler->bytecode->instructions[pc].target = (uint32_t)target->arg_count;
            return !compiler->failed;
        }
        for (size_t i = 0; i < target->arg_count; i++)
            oasp_compile_expr(compiler, target->args[i]);
        pc = oasp_emit(compiler, OASP_BC_ASSIGN_INDEX_NAME, line);
        compiler->bytecode->instructions[pc].operand = oasp_name_pool(compiler, target->left->name);
        compiler->bytecode->instructions[pc].target = (uint32_t)target->arg_count;
        return !compiler->failed;
    }
    if (target->kind == OASP_EXPR_CALL && target->left && target->left->kind == OASP_EXPR_CALL &&
        target->left->left && target->left->left->kind == OASP_EXPR_IDENTIFIER &&
        oasp_ascii_equal(target->left->left->name, "response.cookies") &&
        target->left->arg_count == 1 && target->arg_count == 1) {
        oasp_compile_expr(compiler, target->left->args[0]);
        oasp_compile_expr(compiler, target->args[0]);
        pc = oasp_emit(compiler, OASP_BC_ASSIGN_INDEX_NAME, line);
        compiler->bytecode->instructions[pc].operand =
            oasp_name_pool(compiler, (oasp_slice){"response.cookies.subkey", 23});
        compiler->bytecode->instructions[pc].target = 2;
        return !compiler->failed;
    }
    if (target->kind == OASP_EXPR_CALL && target->left && target->left->kind == OASP_EXPR_MEMBER) {
        oasp_compile_expr(compiler, target->left->left);
        for (size_t i = 0; i < target->arg_count; i++)
            oasp_compile_expr(compiler, target->args[i]);
        pc = oasp_emit(compiler, OASP_BC_ASSIGN_INDEX_MEMBER, line);
        compiler->bytecode->instructions[pc].operand = oasp_name_pool(compiler, target->left->name);
        compiler->bytecode->instructions[pc].target = (uint32_t)target->arg_count;
        return !compiler->failed;
    }
    if (target->kind == OASP_EXPR_CALL && target->left) {
        oasp_compile_expr(compiler, target->left);
        for (size_t i = 0; i < target->arg_count; i++)
            oasp_compile_expr(compiler, target->args[i]);
        pc = oasp_emit(compiler, OASP_BC_ASSIGN_INDEX_DEFAULT, line);
        compiler->bytecode->instructions[pc].target =
            target->arg_count > UINT32_MAX ? OASP_BC_NONE : (uint32_t)target->arg_count;
        return !compiler->failed;
    }
    compiler->failed = true;
    return false;
}

static bool oasp_literal_condition(oasp_bytecode_compiler *compiler, const oasp_expr *expr, bool *truth) {
    oasp_value value;
    if (!oasp_fold_literal(compiler, expr, &value))
        return false;
    *truth = oasp_value_truthy(&value);
    return true;
}

static size_t oasp_compile_condition_jump(oasp_bytecode_compiler *compiler, const oasp_expr *condition, bool jump_when_true, size_t line) {
    if (condition && condition->kind == OASP_EXPR_NOT) {
        condition = condition->left;
        jump_when_true = !jump_when_true;
    }
    if (condition && condition->kind == OASP_EXPR_CALL && condition->left &&
        condition->left->kind == OASP_EXPR_IDENTIFIER && condition->arg_count == 1 &&
        oasp_ascii_equal(condition->left->name, "isempty")) {
        oasp_compile_expr(compiler, condition->args[0]);
        size_t pc = oasp_emit(compiler, OASP_BC_JUMP_IF_EMPTY, line);
        compiler->bytecode->instructions[pc].flags = jump_when_true ? 0 : 1;
        return pc;
    }
    if (condition && condition->kind >= OASP_EXPR_EQ && condition->kind <= OASP_EXPR_GE &&
        condition->left && condition->left->kind == OASP_EXPR_IDENTIFIER &&
        condition->right && condition->right->kind == OASP_EXPR_LITERAL) {
        size_t local = oasp_local_slot_index(compiler, condition->left->name);
        uint32_t constant = oasp_constant_pool(compiler, &condition->right->literal);
        if (local <= UINT32_MAX && constant <= UINT16_MAX) {
            size_t pc = oasp_emit(compiler, OASP_BC_LOCAL_CONST_COMPARE_JUMP, line);
            compiler->bytecode->instructions[pc].operand = (uint32_t)local;
            compiler->bytecode->instructions[pc].reserved = (uint16_t)constant;
            compiler->bytecode->instructions[pc].slot = (int32_t)condition->kind;
            compiler->bytecode->instructions[pc].flags = jump_when_true ? 0 : 1;
            return pc;
        }
    }
    oasp_compile_expr(compiler, condition);
    return oasp_emit(compiler, jump_when_true ? OASP_BC_JUMP_IF_TRUE : OASP_BC_JUMP_IF_FALSE, line);
}

static bool oasp_compile_local_add_store(oasp_bytecode_compiler *compiler, const oasp_stmt *stmt) {
    if (!stmt->target || stmt->target->kind != OASP_EXPR_IDENTIFIER || !stmt->value ||
        stmt->value->kind != OASP_EXPR_ADD || !stmt->value->left || !stmt->value->right ||
        stmt->value->left->kind != OASP_EXPR_IDENTIFIER || stmt->value->right->kind != OASP_EXPR_LITERAL ||
        stmt->value->right->literal.kind != OASP_VALUE_INT ||
        !oasp_name_equal(stmt->target->name, stmt->value->left->name))
        return false;
    size_t slot = oasp_local_slot_index(compiler, stmt->target->name);
    uint32_t constant = oasp_constant_pool(compiler, &stmt->value->right->literal);
    if (slot > UINT32_MAX || constant > UINT16_MAX)
        return false;
    size_t pc = oasp_emit(compiler, OASP_BC_LOCAL_ADD_CONST_STORE, stmt->line);
    compiler->bytecode->instructions[pc].operand = (uint32_t)slot;
    compiler->bytecode->instructions[pc].target = (uint32_t)slot;
    compiler->bytecode->instructions[pc].reserved = (uint16_t)constant;
    return true;
}

static size_t oasp_compile_concat_suffix_terms(oasp_bytecode_compiler *compiler, const oasp_expr *expr, bool *skipped_first) {
    if (expr && expr->kind == OASP_EXPR_CONCAT)
        return oasp_compile_concat_suffix_terms(compiler, expr->left, skipped_first) +
               oasp_compile_concat_suffix_terms(compiler, expr->right, skipped_first);
    if (!*skipped_first) {
        *skipped_first = true;
        return 0;
    }
    oasp_compile_expr(compiler, expr);
    return 1;
}

static bool oasp_count_concat_suffix_terms(const oasp_expr *expr, bool *skipped_first, uint32_t *count) {
    if (expr && expr->kind == OASP_EXPR_CONCAT)
        return oasp_count_concat_suffix_terms(expr->left, skipped_first, count) &&
               oasp_count_concat_suffix_terms(expr->right, skipped_first, count);
    if (!*skipped_first) {
        *skipped_first = true;
        return true;
    }
    if (*count == UINT32_MAX)
        return false;
    (*count)++;
    return true;
}

static bool oasp_compile_self_concat(oasp_bytecode_compiler *compiler, const oasp_stmt *stmt) {
    if (!stmt->target || stmt->target->kind != OASP_EXPR_IDENTIFIER || !stmt->value ||
        stmt->value->kind != OASP_EXPR_CONCAT)
        return false;
    const oasp_expr *first = stmt->value;
    while (first && first->kind == OASP_EXPR_CONCAT)
        first = first->left;
    if (!first || first->kind != OASP_EXPR_IDENTIFIER ||
        !oasp_name_equal(stmt->target->name, first->name))
        return false;
    size_t local = oasp_local_slot_index(compiler, stmt->target->name);
    size_t global = compiler->in_routine
                        ? SIZE_MAX
                        : oasp_global_slot_index(compiler, stmt->target->name);
    if (local == SIZE_MAX && global == SIZE_MAX)
        return false;
    bool skipped_first = false;
    uint32_t count = 0;
    if (!oasp_count_concat_suffix_terms(stmt->value, &skipped_first, &count) || !count)
        return false;
    skipped_first = false;
    if (oasp_compile_concat_suffix_terms(compiler, stmt->value, &skipped_first) != count)
        return false;
    oasp_opcode opcode = local != SIZE_MAX
                             ? (count == 1 ? OASP_BC_APPEND_LOCAL : OASP_BC_APPEND_LOCAL_N)
                             : (count == 1 ? OASP_BC_APPEND_GLOBAL : OASP_BC_APPEND_GLOBAL_N);
    size_t pc = oasp_emit(compiler, opcode, stmt->line);
    compiler->bytecode->instructions[pc].operand =
        (uint32_t)(local != SIZE_MAX ? local : global);
    compiler->bytecode->instructions[pc].target = count;
    return !compiler->failed;
}

static bool oasp_compile_response_write(oasp_bytecode_compiler *compiler, const oasp_stmt *stmt) {
    const oasp_expr *call = stmt ? stmt->value : NULL;
    if (!call || call->kind != OASP_EXPR_CALL || call->arg_count != 1 || !call->left ||
        call->left->kind != OASP_EXPR_IDENTIFIER ||
        !oasp_ascii_equal(call->left->name, "response.write"))
        return false;
    const oasp_expr *value = call->args[0];
    size_t pc;
    if (oasp_compile_recordset_field(compiler, value, OASP_BC_WRITE_RS_FIELD))
        return true;
    if (value->kind == OASP_EXPR_CALL && value->left &&
        value->left->kind == OASP_EXPR_IDENTIFIER && value->arg_count == 1) {
        oasp_opcode opcode = oasp_ascii_equal(value->left->name, "server.htmlencode")
                                 ? OASP_BC_WRITE_HTMLENCODE_RS_FIELD
                             : oasp_ascii_equal(value->left->name, "cint") ||
                                   oasp_ascii_equal(value->left->name, "clng")
                                 ? OASP_BC_WRITE_INT_RS_FIELD
                                 : OASP_BC_COUNT;
        if (opcode != OASP_BC_COUNT && oasp_compile_recordset_field(compiler, value->args[0], opcode))
            return true;
    }
    if (value->kind == OASP_EXPR_LITERAL) {
        pc = oasp_emit(compiler, OASP_BC_WRITE_CONST, stmt->line);
        compiler->bytecode->instructions[pc].operand =
            oasp_constant_pool(compiler, &value->literal);
        return !compiler->failed;
    }
    if (value->kind == OASP_EXPR_IDENTIFIER) {
        size_t local = oasp_local_slot_index(compiler, value->name);
        size_t global = compiler->in_routine
                            ? SIZE_MAX
                            : oasp_global_slot_index(compiler, value->name);
        if (local != SIZE_MAX || global != SIZE_MAX) {
            pc = oasp_emit(compiler, OASP_BC_WRITE_LOCAL, stmt->line);
            compiler->bytecode->instructions[pc].slot =
                local != SIZE_MAX ? (int32_t)local + 1 : -(int32_t)global - 1;
            return true;
        }
        oasp_slice member;
        int32_t root_slot;
        if (oasp_dotted_root_slot(compiler, value->name, &root_slot, &member)) {
            pc = oasp_emit(compiler, OASP_BC_WRITE_MEMBER, stmt->line);
            compiler->bytecode->instructions[pc].flags = OASP_BC_FLAG_ROOT_SLOT;
            compiler->bytecode->instructions[pc].slot = root_slot;
            compiler->bytecode->instructions[pc].operand =
                oasp_name_pool(compiler, value->name);
            compiler->bytecode->instructions[pc].reserved =
                (uint16_t)oasp_member_id_for_name(member);
            return !compiler->failed;
        }
    }
    if (value->kind == OASP_EXPR_MEMBER) {
        oasp_compile_expr(compiler, value->left);
        pc = oasp_emit(compiler, OASP_BC_WRITE_MEMBER, stmt->line);
        compiler->bytecode->instructions[pc].operand =
            oasp_name_pool(compiler, value->name);
        compiler->bytecode->instructions[pc].reserved =
            (uint16_t)oasp_member_id_for_name(value->name);
        return !compiler->failed;
    }
    if (value->kind == OASP_EXPR_CONCAT) {
        size_t count = oasp_compile_concat_terms(compiler, value);
        if (count < 2 || count > UINT32_MAX)
            return false;
        pc = oasp_emit(compiler, OASP_BC_WRITE_N, stmt->line);
        compiler->bytecode->instructions[pc].target = (uint32_t)count;
        return !compiler->failed;
    }
    oasp_compile_expr(compiler, value);
    (void)oasp_emit(compiler, OASP_BC_WRITE_VALUE, stmt->line);
    return !compiler->failed;
}

static bool oasp_compile_recordset_move_next(oasp_bytecode_compiler *compiler, const oasp_expr *expr) {
    if (!expr || expr->kind != OASP_EXPR_CALL || expr->arg_count || !expr->left ||
        expr->left->kind != OASP_EXPR_IDENTIFIER)
        return false;
    oasp_slice receiver;
    oasp_slice member;
    int32_t slot;
    if (!oasp_dotted_parts(expr->left->name, &receiver, &member) ||
        !oasp_ascii_equal(member, "movenext") ||
        !oasp_dotted_root_slot(compiler, expr->left->name, &slot, &member))
        return false;
    size_t pc = oasp_emit(compiler, OASP_BC_RECORDSET_MOVE_NEXT, expr->line);
    compiler->bytecode->instructions[pc].slot = slot;
    compiler->bytecode->instructions[pc].operand = oasp_name_pool(compiler, expr->left->name);
    return !compiler->failed;
}

static bool oasp_compile_loop_exit(oasp_bytecode_compiler *compiler, const oasp_stmt *stmt) {
    int kind = oasp_ascii_equal(stmt->name, "for") ? 1 : oasp_ascii_equal(stmt->name, "do") ? 2 : 0;
    oasp_loop_context *loop = compiler->loop;
    while (loop && loop->exit_kind != kind)
        loop = loop->parent;
    if (!kind || !loop)
        return false;
    size_t pc = oasp_emit(compiler, OASP_BC_JUMP, stmt->line);
    oasp_loop_patch *patch = oasp_arena_alloc(compiler->scratch, sizeof(*patch), _Alignof(oasp_loop_patch));
    if (!patch)
        return false;
    patch->pc = pc;
    patch->next = loop->exits;
    loop->exits = patch;
    return true;
}

static void oasp_patch_loop(oasp_bytecode_compiler *compiler, oasp_loop_context *loop, size_t target) {
    for (oasp_loop_patch *patch = loop->exits; patch; patch = patch->next)
        compiler->bytecode->instructions[patch->pc].target = (uint32_t)target;
}

static void oasp_compile_block(oasp_bytecode_compiler *compiler, const oasp_block *block) {
    oasp_bytecode *bytecode = compiler->bytecode;
    for (const oasp_stmt *stmt = block->first; stmt && !compiler->failed; stmt = stmt->next) {
        size_t pc;
        if (stmt->kind == OASP_STMT_OPTION_EXPLICIT) {
            (void)oasp_emit(compiler, OASP_BC_OPTION_EXPLICIT, stmt->line);
        } else if (stmt->kind == OASP_STMT_ON_ERROR_RESUME_NEXT) {
            (void)oasp_emit(compiler, OASP_BC_ON_ERROR_RESUME_NEXT, stmt->line);
        } else if (stmt->kind == OASP_STMT_ON_ERROR_GOTO_0) {
            (void)oasp_emit(compiler, OASP_BC_ON_ERROR_GOTO_0, stmt->line);
        } else if (stmt->kind == OASP_STMT_DIM || stmt->kind == OASP_STMT_REDIM) {
            for (size_t i = 0; i < stmt->declaration_count; i++) {
                for (size_t j = 0; j < stmt->declarations[i].dimension_count; j++)
                    oasp_compile_expr(compiler, stmt->declarations[i].dimensions[j]);
                pc = oasp_emit(compiler, stmt->kind == OASP_STMT_DIM ? OASP_BC_DECLARE : OASP_BC_REDIM, stmt->line);
                bytecode->instructions[pc].operand = oasp_name_pool(compiler, stmt->declarations[i].name);
                bytecode->instructions[pc].target = (uint32_t)stmt->declarations[i].dimension_count;
                bytecode->instructions[pc].flags = stmt->preserve ? 1u : 0u;
            }
        } else if (stmt->kind == OASP_STMT_CONST) {
            for (size_t i = 0; i < stmt->constant_count; i++) {
                oasp_compile_expr(compiler, stmt->constants[i].value);
                pc = oasp_emit(compiler, OASP_BC_DECLARE_CONST, stmt->line);
                bytecode->instructions[pc].operand = oasp_name_pool(compiler, stmt->constants[i].name);
            }
        } else if (stmt->kind == OASP_STMT_ERASE) {
            for (size_t i = 0; i < stmt->expression_count; i++) {
                (void)oasp_emit(compiler, OASP_BC_PUSH_EMPTY, stmt->line);
                (void)oasp_compile_assign(compiler, stmt->expressions[i], stmt->line);
            }
        } else if (stmt->kind == OASP_STMT_ASSIGN || stmt->kind == OASP_STMT_SET_ASSIGN) {
            if (stmt->kind == OASP_STMT_SET_ASSIGN ||
                (!oasp_compile_self_concat(compiler, stmt) &&
                 !oasp_compile_local_add_store(compiler, stmt))) {
                oasp_compile_expr(compiler, stmt->value);
                (void)oasp_compile_assign(compiler, stmt->target, stmt->line);
            }
        } else if (stmt->kind == OASP_STMT_CALL) {
            const oasp_expr *path = oasp_include_path(stmt);
            if (path) {
                pc = oasp_emit(compiler, OASP_BC_INCLUDE, stmt->line);
                bytecode->instructions[pc].operand = oasp_constant_pool(compiler, &path->literal);
#if !defined(OASP_DISABLE_RENDER_PLAN)
            } else if (oasp_compile_render_response(compiler, stmt)) {
                continue;
#endif
            } else if (oasp_compile_response_write(compiler, stmt)) {
                continue;
            } else if (oasp_compile_recordset_move_next(compiler, stmt->value)) {
                continue;
            } else {
                size_t call_start = bytecode->length;
                oasp_compile_expr(compiler, stmt->value);
                if (bytecode->length > call_start)
                    bytecode->instructions[bytecode->length - 1].flags |= OASP_BC_FLAG_CALL_STATEMENT;
                (void)oasp_emit(compiler, OASP_BC_DROP, stmt->line);
            }
        } else if (stmt->kind == OASP_STMT_EXIT && oasp_compile_loop_exit(compiler, stmt)) {
            break;
        } else if (stmt->kind == OASP_STMT_WHILE || stmt->kind == OASP_STMT_DO) {
            bool is_do = stmt->kind == OASP_STMT_DO;
            bool pre = stmt->condition && !stmt->post_condition;
            bool constant_truth = false;
            if (pre && oasp_literal_condition(compiler, stmt->condition, &constant_truth)) {
                bool enters = stmt->until ? !constant_truth : constant_truth;
                if (!enters)
                    continue;
                pre = false;
            }
            size_t start = bytecode->length;
            size_t condition = SIZE_MAX;
            if (pre)
                condition = oasp_compile_condition_jump(compiler, stmt->condition, is_do && stmt->until, stmt->line);
            oasp_loop_context loop = {compiler->loop, NULL, is_do ? 2 : 0};
            compiler->loop = &loop;
            oasp_compile_block(compiler, &stmt->body);
            compiler->loop = loop.parent;
            size_t post = SIZE_MAX;
            if (stmt->condition && stmt->post_condition) {
                post = oasp_compile_condition_jump(compiler, stmt->condition, stmt->until, stmt->line);
            }
            pc = oasp_emit(compiler, OASP_BC_LOOP_BACK, stmt->line);
            bytecode->instructions[pc].target = (uint32_t)start;
            bytecode->instructions[pc].operand = (uint32_t)bytecode->loop_count++;
            size_t end = bytecode->length;
            if (condition != SIZE_MAX)
                bytecode->instructions[condition].target = bytecode->instructions[condition].error_target = (uint32_t)end;
            if (post != SIZE_MAX)
                bytecode->instructions[post].target = bytecode->instructions[post].error_target = (uint32_t)end;
            oasp_patch_loop(compiler, &loop, end);
        } else if (stmt->kind == OASP_STMT_FOR || stmt->kind == OASP_STMT_FOR_EACH) {
            bool each = stmt->kind == OASP_STMT_FOR_EACH;
            oasp_compile_expr(compiler, stmt->value);
            if (!each) {
                oasp_compile_expr(compiler, stmt->limit);
                if (stmt->step)
                    oasp_compile_expr(compiler, stmt->step);
            }
            size_t loop_id = bytecode->loop_count++;
            pc = oasp_emit(compiler, each ? OASP_BC_FOR_EACH_INIT : OASP_BC_FOR_INIT, stmt->line);
            bytecode->instructions[pc].operand = (uint32_t)loop_id;
            bytecode->instructions[pc].error_target = each ? 1u : stmt->step ? 3u : 2u;
            size_t local = oasp_local_slot_index(compiler, stmt->name);
            size_t global = compiler->in_routine ? SIZE_MAX : oasp_global_slot_index(compiler, stmt->name);
            bytecode->instructions[pc].slot = local != SIZE_MAX ? (int32_t)local + 1 : global != SIZE_MAX ? -(int32_t)global - 1 : 0;
            if (!bytecode->instructions[pc].slot) {
                uint32_t name = oasp_name_pool(compiler, stmt->name);
                if (name > UINT32_C(0x00ffffff)) {
                    compiler->failed = true;
                    continue;
                }
                bytecode->instructions[pc].slot = INT32_MIN;
                bytecode->instructions[pc].flags = (uint8_t)name;
                bytecode->instructions[pc].reserved = (uint16_t)(name >> 8);
            }
            size_t init = pc;
            size_t body = bytecode->length;
            oasp_loop_context loop = {compiler->loop, NULL, 1};
            compiler->loop = &loop;
            oasp_compile_block(compiler, &stmt->body);
            compiler->loop = loop.parent;
            pc = oasp_emit(compiler, each ? OASP_BC_FOR_EACH_NEXT : OASP_BC_FOR_NEXT, stmt->line);
            bytecode->instructions[pc].operand = (uint32_t)loop_id;
            bytecode->instructions[pc].target = (uint32_t)body;
            bytecode->instructions[pc].slot = bytecode->instructions[init].slot;
            bytecode->instructions[pc].flags = bytecode->instructions[init].flags;
            bytecode->instructions[init].target = (uint32_t)bytecode->length;
            oasp_patch_loop(compiler, &loop, bytecode->length);
        } else if (stmt->kind == OASP_STMT_SELECT) {
            size_t select_start = bytecode->length;
            oasp_compile_expr(compiler, stmt->value);
            bool has_else = false;
            for (const oasp_case_branch *branch = stmt->cases; branch; branch = branch->next) {
                if (branch->is_else) {
                    (void)oasp_emit(compiler, OASP_BC_DROP, stmt->line);
                    oasp_compile_block(compiler, &branch->body);
                    pc = oasp_emit(compiler, OASP_BC_JUMP, stmt->line);
                    bytecode->instructions[pc].operand = OASP_BC_NONE;
                    has_else = true;
                    break;
                }
                size_t comparisons = bytecode->length;
                for (size_t i = 0; i < branch->value_count; i++) {
                    (void)oasp_emit(compiler, OASP_BC_DUP, stmt->line);
                    oasp_compile_expr(compiler, branch->values[i]);
                    (void)oasp_emit(compiler, OASP_BC_EQ, branch->values[i]->line);
                    pc = oasp_emit(compiler, OASP_BC_JUMP_IF_TRUE, stmt->line);
                    bytecode->instructions[pc].target = bytecode->instructions[pc].error_target = OASP_BC_NONE;
                }
                size_t skip = oasp_emit(compiler, OASP_BC_JUMP, stmt->line);
                bytecode->instructions[skip].operand = 0;
                size_t body = bytecode->length;
                for (size_t scan = comparisons; scan < skip; scan++)
                    if (bytecode->instructions[scan].opcode == OASP_BC_JUMP_IF_TRUE && bytecode->instructions[scan].target == OASP_BC_NONE)
                        bytecode->instructions[scan].target = (uint32_t)body;
                (void)oasp_emit(compiler, OASP_BC_DROP, stmt->line);
                oasp_compile_block(compiler, &branch->body);
                pc = oasp_emit(compiler, OASP_BC_JUMP, stmt->line);
                bytecode->instructions[pc].operand = OASP_BC_NONE;
                bytecode->instructions[skip].target = (uint32_t)bytecode->length;
            }
            if (!has_else)
                (void)oasp_emit(compiler, OASP_BC_DROP, stmt->line);
            size_t normal_end = oasp_emit(compiler, OASP_BC_JUMP, stmt->line);
            size_t cleanup = bytecode->length;
            (void)oasp_emit(compiler, OASP_BC_DROP, stmt->line);
            size_t end = bytecode->length;
            bytecode->instructions[normal_end].target = (uint32_t)end;
            for (size_t scan = select_start; scan < cleanup; scan++) {
                if (bytecode->instructions[scan].error_target == OASP_BC_NONE)
                    bytecode->instructions[scan].error_target = (uint32_t)cleanup;
                if (bytecode->instructions[scan].opcode == OASP_BC_JUMP && bytecode->instructions[scan].operand == OASP_BC_NONE) {
                    bytecode->instructions[scan].target = (uint32_t)end;
                    bytecode->instructions[scan].operand = 0;
                }
            }
        } else if (stmt->kind == OASP_STMT_WITH) {
            oasp_compile_expr(compiler, stmt->value);
            (void)oasp_emit(compiler, OASP_BC_WITH_ENTER, stmt->line);
            oasp_compile_block(compiler, &stmt->body);
            (void)oasp_emit(compiler, OASP_BC_WITH_LEAVE, stmt->line);
        } else if (stmt->kind == OASP_STMT_EXIT && compiler->in_routine) {
            (void)oasp_emit(compiler, OASP_BC_RETURN, stmt->line);
            break;
        } else if (stmt->kind == OASP_STMT_IF) {
            bool truth;
            if (oasp_literal_condition(compiler, stmt->condition, &truth)) {
                oasp_compile_block(compiler, truth ? &stmt->body : &stmt->alternative);
                continue;
            }
            size_t branch = oasp_compile_condition_jump(compiler, stmt->condition, false, stmt->line);
            oasp_compile_block(compiler, &stmt->body);
            if (stmt->alternative.first) {
                size_t jump = oasp_emit(compiler, OASP_BC_JUMP, stmt->line);
                bytecode->instructions[branch].target = (uint32_t)bytecode->length;
                oasp_compile_block(compiler, &stmt->alternative);
                bytecode->instructions[branch].error_target = (uint32_t)bytecode->length;
                bytecode->instructions[jump].target = (uint32_t)bytecode->length;
                bytecode->instructions[jump].operand = 0;
            } else {
                bytecode->instructions[branch].target = bytecode->instructions[branch].error_target =
                    (uint32_t)bytecode->length;
            }
        } else if (stmt->kind != OASP_STMT_ROUTINE && stmt->kind != OASP_STMT_CLASS && stmt->kind != OASP_STMT_NOOP) {
            compiler->failed = true;
        }
    }
}

static void oasp_compile_routine(oasp_bytecode_compiler *compiler, const oasp_stmt *stmt, size_t class_index) {
    oasp_routine_bytecode *routine = &compiler->bytecode->routines[compiler->bytecode->routine_count++];
    memset(routine, 0, sizeof(*routine));
    routine->name = stmt->name;
    routine->kind = stmt->routine_kind;
    routine->is_default = stmt->is_default;
    routine->class_index = class_index;
    routine->parameter_count = stmt->parameter_count;
    routine->parameters = stmt->parameter_count
                              ? oasp_arena_alloc(compiler->arena, stmt->parameter_count * sizeof(*routine->parameters),
                                                 _Alignof(oasp_parameter_bytecode))
                              : NULL;
    size_t slot_capacity = oasp_count_slot_candidates(&stmt->body) + stmt->parameter_count + (stmt->routine_kind == OASP_ROUTINE_SUB ? 0 : 1);
    routine->slot_names =
        slot_capacity ? oasp_arena_alloc(compiler->arena, slot_capacity * sizeof(*routine->slot_names), _Alignof(oasp_slice)) : NULL;
    if ((stmt->parameter_count && !routine->parameters) || (slot_capacity && !routine->slot_names)) {
        compiler->failed = true;
        return;
    }
    compiler->local_slot_names = routine->slot_names;
    compiler->local_slot_count = 0;
    compiler->local_slot_capacity = slot_capacity;
    compiler->in_routine = true;
    compiler->object_routine = class_index != SIZE_MAX;
    if (stmt->routine_kind != OASP_ROUTINE_SUB)
        oasp_add_slot(routine->slot_names, &compiler->local_slot_count, slot_capacity, stmt->name);
    for (size_t i = 0; i < stmt->parameter_count; i++) {
        routine->parameters[i].name = stmt->parameters[i].name;
        routine->parameters[i].by_value = stmt->parameters[i].by_value;
        routine->parameters[i].array = stmt->parameters[i].array;
        oasp_add_slot(routine->slot_names, &compiler->local_slot_count, slot_capacity, stmt->parameters[i].name);
    }
    oasp_collect_local_slots(compiler, &stmt->body);
    routine->slot_count = compiler->local_slot_count;
    routine->entry = compiler->bytecode->length;
    oasp_compile_block(compiler, &stmt->body);
    (void)oasp_emit(compiler, OASP_BC_RETURN, stmt->line);
    routine->limit = compiler->bytecode->length;
}

static void oasp_compile_routine_definitions(oasp_bytecode_compiler *compiler, const oasp_block *block) {
    for (const oasp_stmt *stmt = block->first; stmt && !compiler->failed; stmt = stmt->next) {
        if (stmt->kind == OASP_STMT_ROUTINE) {
            oasp_compile_routine(compiler, stmt, SIZE_MAX);
            continue;
        }
        if (stmt->kind == OASP_STMT_CLASS)
            continue;
        oasp_compile_routine_definitions(compiler, &stmt->body);
        oasp_compile_routine_definitions(compiler, &stmt->alternative);
        for (const oasp_case_branch *branch = stmt->cases; branch && !compiler->failed; branch = branch->next)
            oasp_compile_routine_definitions(compiler, &branch->body);
    }
}

static void oasp_compile_class_definitions(oasp_bytecode_compiler *compiler, const oasp_block *block) {
    for (const oasp_stmt *stmt = block->first; stmt && !compiler->failed; stmt = stmt->next) {
        if (stmt->kind == OASP_STMT_CLASS) {
        size_t class_index = compiler->bytecode->class_count++;
        oasp_class_bytecode *class_def = &compiler->bytecode->classes[class_index];
        memset(class_def, 0, sizeof(*class_def));
        class_def->name = stmt->name;
        class_def->entry = compiler->bytecode->length;
        bool previous_object = compiler->object_routine;
        compiler->object_routine = true;
        oasp_compile_block(compiler, &stmt->body);
        (void)oasp_emit(compiler, OASP_BC_RETURN, stmt->line);
        compiler->object_routine = previous_object;
        class_def->limit = compiler->bytecode->length;
        class_def->routine_offset = compiler->bytecode->routine_count;
        for (const oasp_stmt *member = stmt->body.first; member; member = member->next)
            if (member->kind == OASP_STMT_ROUTINE)
                oasp_compile_routine(compiler, member, class_index);
        class_def->routine_count = compiler->bytecode->routine_count - class_def->routine_offset;
            continue;
        }
        if (stmt->kind == OASP_STMT_ROUTINE)
            continue;
        oasp_compile_class_definitions(compiler, &stmt->body);
        oasp_compile_class_definitions(compiler, &stmt->alternative);
        for (const oasp_case_branch *branch = stmt->cases; branch && !compiler->failed; branch = branch->next)
            oasp_compile_class_definitions(compiler, &branch->body);
    }
}

static void oasp_compile_definitions(oasp_bytecode_compiler *compiler, const oasp_block *block) {
    oasp_compile_routine_definitions(compiler, block);
    oasp_compile_class_definitions(compiler, block);
    compiler->in_routine = false;
    compiler->object_routine = false;
    compiler->local_slot_names = NULL;
    compiler->local_slot_count = 0;
    compiler->local_slot_capacity = 0;
}

static bool oasp_queue(size_t *depths, size_t *queue, size_t *tail, size_t start, size_t limit,
                       uint32_t target, size_t depth) {
    if (target < start || target >= limit)
        return false;
    size_t index = target - start;
    if (depths[index] == SIZE_MAX) {
        depths[index] = depth;
        queue[(*tail)++] = target;
        return true;
    }
    return depths[index] == depth;
}

static bool oasp_global_slot_valid(int32_t slot, size_t count) {
    if (slot >= 0)
        return false;
    uint64_t index = (uint64_t)(-(int64_t)slot - 1);
    return index < count;
}

static bool oasp_validate_range(oasp_bytecode *bytecode, size_t start, size_t limit, size_t local_slots) {
    if (start >= limit || limit > bytecode->length)
        return false;
    size_t length = limit - start;
    size_t *depths = malloc(length * sizeof(*depths));
    size_t *queue = malloc(length * sizeof(*queue));
    if (!depths || !queue) {
        free(depths);
        free(queue);
        return false;
    }
    for (size_t i = 0; i < length; i++)
        depths[i] = SIZE_MAX;
    size_t head = 0;
    size_t tail = 1;
    depths[0] = 0;
    queue[0] = start;
    bool valid = true;
    while (head < tail && valid) {
        size_t pc = queue[head++];
        const oasp_instruction *instruction = &bytecode->instructions[pc];
        size_t depth = depths[pc - start];
        size_t required = 0;
        ptrdiff_t delta = 0;
        bool fallthrough = true;
        bool terminal = false;
        switch ((oasp_opcode)instruction->opcode) {
        case OASP_BC_PUSH_EMPTY:
        case OASP_BC_PUSH_LITERAL:
        case OASP_BC_PUSH_NAME:
        case OASP_BC_PUSH_GLOBAL_SLOT:
        case OASP_BC_PUSH_LOCAL_SLOT:
        case OASP_BC_NEW:
        case OASP_BC_LOAD_LOCAL_CALL_BUILTIN:
        case OASP_BC_RECORDSET_FIELD:
        case OASP_BC_RECORDSET_EOF:
            delta = 1;
            break;
        case OASP_BC_GET_MEMBER:
        case OASP_BC_NEGATE:
        case OASP_BC_POSITIVE:
        case OASP_BC_NOT:
            required = 1;
            break;
        case OASP_BC_CALL_NAME:
            required = instruction->target;
            delta = 1 - (ptrdiff_t)required;
            break;
        case OASP_BC_CALL_MEMBER:
        case OASP_BC_CALL_DEFAULT:
        case OASP_BC_LOAD_MEMBER_CALL_MEMBER:
            required = (size_t)instruction->target + 1;
            delta = 1 - (ptrdiff_t)required;
            break;
        case OASP_BC_CONCAT_N:
            required = instruction->operand;
            delta = 1 - (ptrdiff_t)required;
            break;
        case OASP_BC_CONCAT:
        case OASP_BC_EQ:
        case OASP_BC_NE:
        case OASP_BC_LT:
        case OASP_BC_LE:
        case OASP_BC_GT:
        case OASP_BC_GE:
        case OASP_BC_ADD:
        case OASP_BC_SUBTRACT:
        case OASP_BC_MULTIPLY:
        case OASP_BC_DIVIDE:
        case OASP_BC_INT_DIVIDE:
        case OASP_BC_MOD:
        case OASP_BC_POWER:
        case OASP_BC_AND:
        case OASP_BC_OR:
        case OASP_BC_XOR:
        case OASP_BC_EQV:
        case OASP_BC_IMP:
        case OASP_BC_IS:
        case OASP_BC_IS_NOT:
        case OASP_BC_ADD_I64:
        case OASP_BC_COMPARE_STRING:
        case OASP_BC_CONCAT_STRING:
            required = 2;
            delta = -1;
            break;
        case OASP_BC_ASSIGN_NAME:
        case OASP_BC_ASSIGN_GLOBAL_SLOT:
        case OASP_BC_ASSIGN_LOCAL_SLOT:
        case OASP_BC_DECLARE_CONST:
        case OASP_BC_DROP:
        case OASP_BC_WITH_ENTER:
            required = 1;
            delta = -1;
            break;
        case OASP_BC_ASSIGN_MEMBER:
            required = 2;
            delta = -2;
            break;
        case OASP_BC_ASSIGN_INDEX_NAME:
            required = (size_t)instruction->target + 1;
            delta = -(ptrdiff_t)required;
            break;
        case OASP_BC_ASSIGN_INDEX_MEMBER:
        case OASP_BC_ASSIGN_INDEX_DEFAULT:
            required = (size_t)instruction->target + 2;
            delta = -(ptrdiff_t)required;
            break;
        case OASP_BC_ASSIGN_COOKIE_ATTRIBUTE:
            required = (size_t)instruction->target + 1;
            delta = -(ptrdiff_t)required;
            break;
        case OASP_BC_DECLARE:
        case OASP_BC_REDIM:
            required = instruction->target;
            delta = -(ptrdiff_t)required;
            break;
        case OASP_BC_DUP:
            required = 1;
            delta = 1;
            break;
        case OASP_BC_JUMP_IF_FALSE:
        case OASP_BC_JUMP_IF_TRUE:
        case OASP_BC_JUMP_IF_EMPTY:
            required = 1;
            delta = -1;
            break;
        case OASP_BC_LOCAL_CONST_COMPARE_JUMP:
        case OASP_BC_LOCAL_ADD_CONST_STORE:
        case OASP_BC_WRITE_CONST:
        case OASP_BC_WRITE_LOCAL:
        case OASP_BC_WRITE_RS_FIELD:
        case OASP_BC_WRITE_HTMLENCODE_RS_FIELD:
        case OASP_BC_WRITE_INT_RS_FIELD:
        case OASP_BC_WRITE_RENDER_RS_FIELD:
        case OASP_BC_RECORDSET_MOVE_NEXT:
            break;
        case OASP_BC_APPEND_LOCAL:
        case OASP_BC_APPEND_GLOBAL:
        case OASP_BC_WRITE_VALUE:
            required = 1;
            delta = -1;
            break;
        case OASP_BC_APPEND_LOCAL_N:
        case OASP_BC_APPEND_GLOBAL_N:
        case OASP_BC_WRITE_N:
            required = instruction->target;
            delta = -(ptrdiff_t)required;
            break;
        case OASP_BC_WRITE_MEMBER:
            required = instruction->flags & OASP_BC_FLAG_ROOT_SLOT ? 0 : 1;
            delta = -(ptrdiff_t)required;
            break;
        case OASP_BC_FOR_INIT:
            required = instruction->error_target;
            delta = -(ptrdiff_t)required;
            break;
        case OASP_BC_FOR_EACH_INIT:
            required = 1;
            delta = -1;
            break;
        case OASP_BC_RETURN:
        case OASP_BC_HALT:
            terminal = true;
            fallthrough = false;
            break;
        case OASP_BC_JUMP:
        case OASP_BC_LOOP_BACK:
            fallthrough = false;
            break;
        default:
            break;
        }
        if (instruction->opcode >= OASP_BC_COUNT || depth < required || (terminal && depth)) {
            valid = false;
            break;
        }
        size_t next_depth = (size_t)((ptrdiff_t)depth + delta);
        if (next_depth > bytecode->max_stack)
            bytecode->max_stack = next_depth;
        if ((instruction->opcode == OASP_BC_PUSH_LITERAL || instruction->opcode == OASP_BC_INCLUDE ||
             instruction->opcode == OASP_BC_WRITE_CONST || instruction->opcode == OASP_BC_RECORDSET_FIELD ||
             instruction->opcode == OASP_BC_WRITE_RS_FIELD ||
             instruction->opcode == OASP_BC_WRITE_HTMLENCODE_RS_FIELD ||
             instruction->opcode == OASP_BC_WRITE_INT_RS_FIELD ||
             instruction->opcode == OASP_BC_WRITE_RENDER_RS_FIELD) &&
            instruction->operand >= bytecode->constant_count)
            valid = false;
        if ((instruction->opcode == OASP_BC_PUSH_NAME || instruction->opcode == OASP_BC_NEW ||
             instruction->opcode == OASP_BC_GET_MEMBER || instruction->opcode == OASP_BC_CALL_NAME ||
             instruction->opcode == OASP_BC_CALL_MEMBER || instruction->opcode == OASP_BC_LOAD_MEMBER_CALL_MEMBER ||
             instruction->opcode == OASP_BC_ASSIGN_NAME ||
             instruction->opcode == OASP_BC_ASSIGN_MEMBER || instruction->opcode == OASP_BC_ASSIGN_INDEX_NAME ||
             instruction->opcode == OASP_BC_ASSIGN_INDEX_MEMBER || instruction->opcode == OASP_BC_ASSIGN_COOKIE_ATTRIBUTE ||
             instruction->opcode == OASP_BC_DECLARE || instruction->opcode == OASP_BC_WRITE_MEMBER ||
             instruction->opcode == OASP_BC_REDIM || instruction->opcode == OASP_BC_DECLARE_CONST) &&
            instruction->operand >= bytecode->name_count)
            valid = false;
        if (instruction->opcode == OASP_BC_LOAD_LOCAL_CALL_BUILTIN &&
            (instruction->operand >= local_slots || instruction->error_target >= bytecode->name_count ||
             !instruction->reserved || instruction->reserved >= OASP_BUILTIN_COUNT))
            valid = false;
        if ((instruction->opcode == OASP_BC_GET_MEMBER ||
             instruction->opcode == OASP_BC_LOAD_MEMBER_CALL_MEMBER) &&
            instruction->reserved >= OASP_MEMBER_COUNT)
            valid = false;
        if (instruction->opcode == OASP_BC_PUSH_NAME && instruction->reserved >= OASP_MEMBER_COUNT)
            valid = false;
        if (instruction->opcode == OASP_BC_CALL_NAME &&
            instruction->flags & OASP_BC_FLAG_STATIC_MEMBER &&
            (!instruction->reserved || instruction->reserved >= OASP_MEMBER_COUNT))
            valid = false;
        if ((instruction->opcode == OASP_BC_LOCAL_CONST_COMPARE_JUMP ||
             instruction->opcode == OASP_BC_LOCAL_ADD_CONST_STORE) &&
            (instruction->operand >= local_slots || instruction->reserved >= bytecode->constant_count))
            valid = false;
        if (instruction->opcode == OASP_BC_CALL_NAME) {
            if (instruction->flags & OASP_BC_FLAG_ROOT_SLOT) {
                if (!instruction->slot ||
                    (instruction->slot > 0 && (size_t)(instruction->slot - 1) >= local_slots) ||
                    (instruction->slot < 0 && !oasp_global_slot_valid(instruction->slot, bytecode->global_slot_count)))
                    valid = false;
            } else if (instruction->slot < 0 ||
                       (instruction->slot > 0 &&
                        (size_t)(instruction->slot - 1) >= bytecode->routine_count)) {
                valid = false;
            }
        }
        if ((instruction->opcode == OASP_BC_PUSH_GLOBAL_SLOT || instruction->opcode == OASP_BC_ASSIGN_GLOBAL_SLOT) &&
            instruction->operand >= bytecode->global_slot_count)
            valid = false;
        if ((instruction->opcode == OASP_BC_PUSH_LOCAL_SLOT || instruction->opcode == OASP_BC_ASSIGN_LOCAL_SLOT) &&
            instruction->operand >= local_slots)
            valid = false;
        if (instruction->opcode == OASP_BC_APPEND_LOCAL && instruction->operand >= local_slots)
            valid = false;
        if (instruction->opcode == OASP_BC_APPEND_LOCAL_N &&
            (!instruction->target || instruction->operand >= local_slots))
            valid = false;
        if (instruction->opcode == OASP_BC_APPEND_GLOBAL &&
            instruction->operand >= bytecode->global_slot_count)
            valid = false;
        if (instruction->opcode == OASP_BC_APPEND_GLOBAL_N &&
            (!instruction->target || instruction->operand >= bytecode->global_slot_count))
            valid = false;
        if (instruction->opcode == OASP_BC_WRITE_N && instruction->target < 2)
            valid = false;
        if ((instruction->opcode == OASP_BC_RECORDSET_FIELD ||
             instruction->opcode == OASP_BC_WRITE_RS_FIELD ||
             instruction->opcode == OASP_BC_WRITE_HTMLENCODE_RS_FIELD ||
             instruction->opcode == OASP_BC_WRITE_INT_RS_FIELD ||
             instruction->opcode == OASP_BC_WRITE_RENDER_RS_FIELD) &&
            (!instruction->slot ||
             (instruction->slot > 0 && (size_t)(instruction->slot - 1) >= local_slots) ||
             (instruction->slot < 0 && !oasp_global_slot_valid(instruction->slot, bytecode->global_slot_count)) ||
             ((instruction->flags & OASP_BC_FLAG_ROOT_SLOT) &&
              ((instruction->opcode != OASP_BC_WRITE_RENDER_RS_FIELD &&
                instruction->target >= bytecode->name_count) ||
               (instruction->reserved != OASP_MEMBER_FIELDS &&
                instruction->reserved != OASP_MEMBER_ITEM)))))
            valid = false;
        if (instruction->opcode == OASP_BC_WRITE_RENDER_RS_FIELD &&
            (instruction->target >= bytecode->constant_count ||
             instruction->error_target >= bytecode->constant_count))
            valid = false;
        if ((instruction->opcode == OASP_BC_RECORDSET_MOVE_NEXT ||
             instruction->opcode == OASP_BC_RECORDSET_EOF) &&
            (!instruction->slot || instruction->operand >= bytecode->name_count ||
             (instruction->slot > 0 && (size_t)(instruction->slot - 1) >= local_slots) ||
             (instruction->slot < 0 && !oasp_global_slot_valid(instruction->slot, bytecode->global_slot_count))))
            valid = false;
        if ((instruction->opcode == OASP_BC_PUSH_NAME ||
             instruction->opcode == OASP_BC_WRITE_MEMBER) &&
            instruction->flags & OASP_BC_FLAG_ROOT_SLOT &&
            (!instruction->slot ||
             (instruction->slot > 0 && (size_t)(instruction->slot - 1) >= local_slots) ||
             (instruction->slot < 0 && !oasp_global_slot_valid(instruction->slot, bytecode->global_slot_count))))
            valid = false;
        if (instruction->opcode == OASP_BC_WRITE_LOCAL &&
            (!instruction->slot ||
             (instruction->slot > 0 && (size_t)(instruction->slot - 1) >= local_slots) ||
             (instruction->slot < 0 && !oasp_global_slot_valid(instruction->slot, bytecode->global_slot_count))))
            valid = false;
        if (!valid)
            break;
        if (instruction->opcode == OASP_BC_JUMP || instruction->opcode == OASP_BC_LOOP_BACK ||
            instruction->opcode == OASP_BC_FOR_NEXT || instruction->opcode == OASP_BC_FOR_EACH_NEXT)
            valid = oasp_queue(depths, queue, &tail, start, limit, instruction->target, next_depth);
        else if (instruction->opcode == OASP_BC_JUMP_IF_FALSE || instruction->opcode == OASP_BC_JUMP_IF_TRUE)
            valid = oasp_queue(depths, queue, &tail, start, limit, instruction->target, next_depth) &&
                    oasp_queue(depths, queue, &tail, start, limit, instruction->error_target, next_depth);
        else if (instruction->opcode == OASP_BC_JUMP_IF_EMPTY)
            valid = oasp_queue(depths, queue, &tail, start, limit, instruction->target, next_depth);
        else if (instruction->opcode == OASP_BC_LOCAL_CONST_COMPARE_JUMP)
            valid = oasp_queue(depths, queue, &tail, start, limit, instruction->target, next_depth) &&
                    oasp_queue(depths, queue, &tail, start, limit, instruction->error_target, next_depth);
        else if (instruction->opcode == OASP_BC_FOR_INIT || instruction->opcode == OASP_BC_FOR_EACH_INIT)
            valid = oasp_queue(depths, queue, &tail, start, limit, instruction->target, next_depth);
        if (valid && fallthrough)
            valid = pc + 1 < limit && oasp_queue(depths, queue, &tail, start, limit, (uint32_t)(pc + 1), next_depth);
    }
    free(depths);
    free(queue);
    return valid;
}

static bool oasp_range_error_aware(const oasp_bytecode *bytecode, size_t start, size_t limit) {
    for (size_t i = start; i < limit; i++) {
        uint8_t opcode = bytecode->instructions[i].opcode;
        if (opcode == OASP_BC_ON_ERROR_RESUME_NEXT || opcode == OASP_BC_ON_ERROR_GOTO_0 ||
            opcode == OASP_BC_INCLUDE)
            return true;
    }
    return false;
}

bool oasp_bytecode_validate(oasp_bytecode *bytecode) {
    if (!bytecode || !bytecode->instructions || !bytecode->length)
        return false;
    bytecode->max_stack = 0;
    memset(bytecode->opcode_counts, 0, sizeof(bytecode->opcode_counts));
    for (size_t i = 0; i < bytecode->length; i++) {
        if (bytecode->instructions[i].opcode >= OASP_BC_COUNT)
            return false;
        bytecode->opcode_counts[bytecode->instructions[i].opcode]++;
    }
    size_t root_limit = bytecode->length;
    oasp_class_bytecode class_def;
    oasp_routine_bytecode routine;
    if (bytecode->class_count) {
        if (!oasp_bytecode_class_at(bytecode, 0, &class_def))
            return false;
        if (class_def.entry < root_limit)
            root_limit = class_def.entry;
    }
    if (bytecode->routine_count) {
        if (!oasp_bytecode_routine_at(bytecode, 0, &routine))
            return false;
        if (routine.entry < root_limit)
            root_limit = routine.entry;
    }
    if (!oasp_validate_range(bytecode, 0, root_limit, 0)) {
        if (oasp_process_options.trace_vm)
            fprintf(stderr, "OpenASP bytecode validation failed in root range 0..%zu\n", root_limit);
        return false;
    }
    bool root_error_aware = oasp_range_error_aware(bytecode, 0, root_limit);
    if (bytecode->image_base && bytecode->root_error_aware != root_error_aware)
        return false;
    bytecode->root_error_aware = root_error_aware;
    for (size_t i = 0; i < bytecode->class_count; i++) {
        if (!oasp_bytecode_class_at(bytecode, i, &class_def) ||
            !oasp_validate_range(bytecode, class_def.entry, class_def.limit, 0)) {
            if (oasp_process_options.trace_vm)
                fprintf(stderr, "OpenASP bytecode validation failed in class %zu range %zu..%zu\n", i,
                        class_def.entry, class_def.limit);
            return false;
        }
        bool error_aware = oasp_range_error_aware(bytecode, class_def.entry, class_def.limit);
        if (bytecode->image_base) {
            if (class_def.error_aware != error_aware)
                return false;
        } else {
            bytecode->classes[i].error_aware = error_aware;
        }
    }
    for (size_t i = 0; i < bytecode->routine_count; i++) {
        if (!oasp_bytecode_routine_at(bytecode, i, &routine) ||
            !oasp_validate_range(bytecode, routine.entry, routine.limit, routine.slot_count)) {
            if (oasp_process_options.trace_vm)
                fprintf(stderr, "OpenASP bytecode validation failed in routine %zu range %zu..%zu slots=%zu\n", i,
                        routine.entry, routine.limit, routine.slot_count);
            return false;
        }
        bool error_aware = oasp_range_error_aware(bytecode, routine.entry, routine.limit);
        if (bytecode->image_base) {
            if (routine.error_aware != error_aware)
                return false;
        } else {
            bytecode->routines[i].error_aware = error_aware;
        }
    }
    return true;
}

static uint32_t oasp_state_root_effect(oasp_slice name, bool write) {
    static const char session[] = "session";
    static const char application[] = "application";
    size_t root_length = 0;
    while (root_length < name.length && name.data[root_length] != '.')
        root_length++;
    if (oasp_ascii_equal_n(name.data, root_length, session))
        return write ? OASP_STATE_WRITES_SESSION : OASP_STATE_READS_SESSION;
    if (oasp_ascii_equal_n(name.data, root_length, application))
        return write ? OASP_STATE_WRITES_APPLICATION : OASP_STATE_READS_APPLICATION;
    return 0;
}

static uint32_t oasp_state_expr_effects(const oasp_expr *expr) {
    if (!expr)
        return 0;
    uint32_t effects = oasp_state_expr_effects(expr->left) | oasp_state_expr_effects(expr->right);
    if (expr->kind == OASP_EXPR_IDENTIFIER)
        effects |= oasp_state_root_effect(expr->name, false);
    if (expr->kind == OASP_EXPR_CALL && expr->left && expr->left->kind == OASP_EXPR_IDENTIFIER &&
        (oasp_ascii_equal(expr->left->name, "session.abandon") ||
         oasp_ascii_equal(expr->left->name, "session.contents.remove")))
        effects |= OASP_STATE_WRITES_SESSION;
    if (expr->kind == OASP_EXPR_CALL && expr->left && expr->left->kind == OASP_EXPR_IDENTIFIER &&
        oasp_ascii_equal(expr->left->name, "application.contents.remove"))
        effects |= OASP_STATE_WRITES_APPLICATION;
    if (expr->kind == OASP_EXPR_CALL && expr->left && expr->left->kind == OASP_EXPR_IDENTIFIER &&
        (oasp_ascii_equal(expr->left->name, "execute") ||
         oasp_ascii_equal(expr->left->name, "executeglobal") ||
         oasp_ascii_equal(expr->left->name, "eval")))
        effects |= OASP_STATE_READS_SESSION | OASP_STATE_WRITES_SESSION |
                   OASP_STATE_READS_APPLICATION | OASP_STATE_WRITES_APPLICATION;
    for (size_t i = 0; i < expr->arg_count; i++)
        effects |= oasp_state_expr_effects(expr->args[i]);
    return effects;
}

static uint32_t oasp_state_target_effects(const oasp_expr *expr) {
    if (!expr)
        return 0;
    if (expr->kind == OASP_EXPR_IDENTIFIER)
        return oasp_state_root_effect(expr->name, true);
    return oasp_state_target_effects(expr->left);
}

static uint32_t oasp_state_block_effects(const oasp_block *block) {
    uint32_t effects = 0;
    for (const oasp_stmt *stmt = block->first; stmt; stmt = stmt->next) {
        effects |= oasp_state_expr_effects(stmt->target);
        effects |= oasp_state_expr_effects(stmt->value);
        effects |= oasp_state_expr_effects(stmt->condition);
        effects |= oasp_state_expr_effects(stmt->limit);
        effects |= oasp_state_expr_effects(stmt->step);
        if (stmt->kind == OASP_STMT_ASSIGN || stmt->kind == OASP_STMT_SET_ASSIGN ||
            stmt->kind == OASP_STMT_ERASE)
            effects |= oasp_state_target_effects(stmt->target);
        for (size_t i = 0; i < stmt->expression_count; i++)
            effects |= oasp_state_expr_effects(stmt->expressions[i]);
        for (size_t i = 0; i < stmt->declaration_count; i++)
            for (size_t j = 0; j < stmt->declarations[i].dimension_count; j++)
                effects |= oasp_state_expr_effects(stmt->declarations[i].dimensions[j]);
        for (size_t i = 0; i < stmt->constant_count; i++)
            effects |= oasp_state_expr_effects(stmt->constants[i].value);
        effects |= oasp_state_block_effects(&stmt->body);
        effects |= oasp_state_block_effects(&stmt->alternative);
        for (const oasp_case_branch *branch = stmt->cases; branch; branch = branch->next) {
            for (size_t i = 0; i < branch->value_count; i++)
                effects |= oasp_state_expr_effects(branch->values[i]);
            effects |= oasp_state_block_effects(&branch->body);
        }
    }
    return effects;
}

static bool oasp_compile_internal(oasp_arena *arena, const oasp_block *block, oasp_bytecode *bytecode, bool unresolved_globals_dynamic) {
    if (!arena || !block || !bytecode)
        return false;
    oasp_arena *analysis_arena = oasp_arena_create(16384);
    oasp_symbol_resolution *symbols = NULL;
    if (!analysis_arena || !oasp_symbol_resolve_program(analysis_arena, block, unresolved_globals_dynamic, &symbols)) {
        oasp_arena_destroy(analysis_arena);
        return false;
    }
    memset(bytecode, 0, sizeof(*bytecode));
    bytecode->state_effects = oasp_state_block_effects(block);
    size_t name_capacity = 1, constant_capacity = 1, routine_capacity = 0, class_capacity = 0;
    oasp_count_metadata(block, &name_capacity, &constant_capacity, &routine_capacity, &class_capacity);
    size_t global_capacity = oasp_count_slot_candidates(block);
    size_t instruction_capacity = oasp_count_block(block);
    if (instruction_capacity == SIZE_MAX || !oasp_count_definition_instructions(block, &instruction_capacity) ||
        instruction_capacity == SIZE_MAX) {
        oasp_arena_destroy(analysis_arena);
        return false;
    }
    instruction_capacity++;
    bytecode->instructions =
        oasp_arena_alloc(arena, (instruction_capacity + 1) * sizeof(*bytecode->instructions), _Alignof(oasp_instruction));
    bytecode->names = oasp_arena_alloc(arena, name_capacity * sizeof(*bytecode->names), _Alignof(oasp_slice));
    bytecode->constants = oasp_arena_alloc(arena, constant_capacity * sizeof(*bytecode->constants), _Alignof(oasp_value));
    bytecode->global_slot_names =
        global_capacity ? oasp_arena_alloc(arena, global_capacity * sizeof(*bytecode->global_slot_names), _Alignof(oasp_slice)) : NULL;
    bytecode->routines =
        routine_capacity ? oasp_arena_alloc(arena, routine_capacity * sizeof(*bytecode->routines), _Alignof(oasp_routine_bytecode)) : NULL;
    bytecode->classes =
        class_capacity ? oasp_arena_alloc(arena, class_capacity * sizeof(*bytecode->classes), _Alignof(oasp_class_bytecode)) : NULL;
    if (!bytecode->instructions || !bytecode->names || !bytecode->constants || (global_capacity && !bytecode->global_slot_names) ||
        (routine_capacity && !bytecode->routines) || (class_capacity && !bytecode->classes)) {
        oasp_arena_destroy(analysis_arena);
        return false;
    }
    oasp_bytecode_compiler compiler;
    memset(&compiler, 0, sizeof(compiler));
    compiler.arena = arena;
    compiler.scratch = analysis_arena;
    compiler.bytecode = bytecode;
    compiler.program = block;
    compiler.symbols = symbols;
    compiler.name_capacity = name_capacity;
    compiler.constant_capacity = constant_capacity;
    compiler.instruction_capacity = instruction_capacity;
    compiler.unresolved_globals_dynamic = unresolved_globals_dynamic;
    oasp_collect_global_slots(&compiler, block, global_capacity);
    oasp_compile_block(&compiler, block);
    (void)oasp_emit(&compiler, OASP_BC_HALT, 0);
    oasp_compile_definitions(&compiler, block);
    bool valid = !compiler.failed && bytecode->length <= instruction_capacity && bytecode->routine_count == routine_capacity &&
                 bytecode->class_count == class_capacity && oasp_bytecode_validate(bytecode);
    if (!valid && oasp_process_options.trace_vm)
        fprintf(stderr,
                "OpenASP bytecode compile: failed=%d instructions=%zu/%zu routines=%zu/%zu classes=%zu/%zu names=%zu/%zu constants=%zu/%zu\n",
                compiler.failed, bytecode->length, instruction_capacity, bytecode->routine_count, routine_capacity,
                bytecode->class_count, class_capacity, bytecode->name_count, name_capacity, bytecode->constant_count,
                constant_capacity);
    oasp_arena_destroy(analysis_arena);
    return valid;
}

bool oasp_bytecode_compile(oasp_arena *arena, const oasp_block *block, oasp_bytecode *bytecode) {
    return oasp_compile_internal(arena, block, bytecode, false);
}

bool oasp_bytecode_compile_module(oasp_arena *arena, const oasp_block *block, oasp_bytecode *bytecode) {
    return oasp_compile_internal(arena, block, bytecode, true);
}

bool oasp_bytecode_find_routine(const oasp_bytecode *bytecode, oasp_slice name, size_t class_index, oasp_routine_kind first, oasp_routine_kind second,
                                bool default_only, size_t *result) {
    if (!bytecode || !result)
        return false;
    for (size_t i = 0; i < bytecode->routine_count; i++) {
        oasp_routine_bytecode routine;
        if (!oasp_bytecode_routine_at(bytecode, i, &routine))
            return false;
        if (routine.class_index != class_index || (default_only ? !routine.is_default : !oasp_name_equal(routine.name, name)))
            continue;
        if (routine.kind == first || routine.kind == second) {
            *result = i;
            return true;
        }
    }
    return false;
}
