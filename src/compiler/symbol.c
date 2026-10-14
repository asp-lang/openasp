// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/compiler/symbol.c`): Compiler pipeline. Arena-owned tokens, AST nodes,
 * symbols, bytecode, and immutable images move through this stage without per-node frees.
 * Length/count variables bound every indexed operation and malformed input must fail before
 * execution.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#include "symbol.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    const oasp_expr *expr;
    oasp_symbol symbol;
} oasp_symbol_entry;

struct oasp_symbol_resolution {
    oasp_symbol_entry *entries;
    size_t capacity;
};

typedef struct {
    oasp_arena *arena;
    oasp_symbol_resolution *resolution;
    const oasp_block *program;
    const oasp_stmt *routine;
    const oasp_stmt *class_stmt;
    oasp_slice *globals;
    size_t global_count;
    oasp_slice *locals;
    size_t local_count;
    oasp_slice *parameters;
    size_t parameter_count;
    bool module_dynamic;
    bool failed;
} oasp_symbol_pass;

static bool oasp_symbol_name_equal(oasp_slice left, oasp_slice right) {
    if (left.length != right.length)
        return false;
    for (size_t i = 0; i < left.length; i++)
        if (tolower((unsigned char)left.data[i]) != tolower((unsigned char)right.data[i]))
            return false;
    return true;
}

static bool oasp_symbol_name_is(oasp_slice name, const char *literal) {
    size_t length = strlen(literal);
    return name.length == length && oasp_symbol_name_equal(name, (oasp_slice){literal, length});
}

uint32_t oasp_symbol_id(oasp_slice name) {
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < name.length; i++) {
        hash ^= (uint8_t)tolower((unsigned char)name.data[i]);
        hash *= UINT32_C(16777619);
    }
    return hash ? hash : UINT32_C(1);
}

oasp_builtin_id oasp_builtin_id_for_name(oasp_slice name) {
    if (oasp_symbol_name_is(name, "response.write"))
        return OASP_BUILTIN_RESPONSE_WRITE;
    if (oasp_symbol_name_is(name, "cstr"))
        return OASP_BUILTIN_CSTR;
    if (oasp_symbol_name_is(name, "len"))
        return OASP_BUILTIN_LEN;
    if (oasp_symbol_name_is(name, "isempty"))
        return OASP_BUILTIN_ISEMPTY;
    if (oasp_symbol_name_is(name, "server.htmlencode"))
        return OASP_BUILTIN_SERVER_HTMLENCODE;
    if (oasp_symbol_name_is(name, "server.urlencode"))
        return OASP_BUILTIN_SERVER_URLENCODE;
    if (oasp_symbol_name_is(name, "trim"))
        return OASP_BUILTIN_TRIM;
    if (oasp_symbol_name_is(name, "lcase"))
        return OASP_BUILTIN_LCASE;
    if (oasp_symbol_name_is(name, "ucase"))
        return OASP_BUILTIN_UCASE;
    if (oasp_symbol_name_is(name, "replace"))
        return OASP_BUILTIN_REPLACE;
    return OASP_BUILTIN_DYNAMIC;
}

oasp_member_id oasp_member_id_for_name(oasp_slice name) {
    if (oasp_symbol_name_is(name, "fields"))
        return OASP_MEMBER_FIELDS;
    if (oasp_symbol_name_is(name, "item"))
        return OASP_MEMBER_ITEM;
    if (oasp_symbol_name_is(name, "movenext"))
        return OASP_MEMBER_MOVENEXT;
    if (oasp_symbol_name_is(name, "eof"))
        return OASP_MEMBER_EOF;
    if (oasp_symbol_name_is(name, "execute"))
        return OASP_MEMBER_EXECUTE;
    if (oasp_symbol_name_is(name, "open"))
        return OASP_MEMBER_OPEN;
    if (oasp_symbol_name_is(name, "close"))
        return OASP_MEMBER_CLOSE;
    return OASP_MEMBER_DYNAMIC;
}

static bool oasp_symbol_is_builtin(oasp_slice name) {
    static const char *const names[] = {
        "__openaspmapcharacters", "abs", "application", "application.contents.remove", "application.lock",
        "application.unlock", "array", "asc", "ascb", "ascw", "atn", "cbool", "cdate", "chr", "chrb",
        "chrw", "cint", "clng", "cos", "createobject", "cstr", "date", "dateadd", "datediff", "datepart",
        "dateserial", "day", "err.clear", "err.raise", "escape", "eval", "execute", "executeglobal", "exp",
        "fix", "formatdatetime", "formatnumber", "hex", "hour", "instr", "instrb", "instrrev", "int",
        "isarray", "isdate", "isempty", "isnull", "isnumeric", "isobject", "join", "lbound", "lcase",
        "left", "leftb", "len", "lenb", "log", "mid", "midb", "minute", "month", "now", "randomize",
        "replace", "request", "request.binaryread", "request.cookies", "request.form", "request.querystring",
        "request.servervariables", "response.addheader", "response.binarywrite", "response.clear",
        "response.cookies", "response.end", "response.flush", "response.redirect", "response.write", "right",
        "rightb", "rnd", "second", "server.createobject", "server.htmlencode", "server.mappath",
        "server.transfer", "server.urlencode", "session", "session.abandon", "session.contents.remove", "sgn",
        "sin", "space", "split", "strcomp", "string", "tan", "timer", "trim", "typename", "ubound",
        "ucase", "unescape", "vartype", "vbsescape", "vbsunescape", "weekday", "year",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (oasp_symbol_name_is(name, names[i]))
            return true;
    return false;
}

static size_t oasp_symbol_count_expr(const oasp_expr *expr) {
    if (!expr)
        return 0;
    size_t count = 1 + oasp_symbol_count_expr(expr->left) + oasp_symbol_count_expr(expr->right);
    for (size_t i = 0; i < expr->arg_count; i++)
        count += oasp_symbol_count_expr(expr->args[i]);
    return count;
}

static size_t oasp_symbol_count_block(const oasp_block *block) {
    size_t count = 0;
    for (const oasp_stmt *stmt = block->first; stmt; stmt = stmt->next) {
        count += oasp_symbol_count_expr(stmt->target);
        count += oasp_symbol_count_expr(stmt->value);
        count += oasp_symbol_count_expr(stmt->condition);
        count += oasp_symbol_count_expr(stmt->limit);
        count += oasp_symbol_count_expr(stmt->step);
        for (size_t i = 0; i < stmt->expression_count; i++)
            count += oasp_symbol_count_expr(stmt->expressions[i]);
        for (size_t i = 0; i < stmt->declaration_count; i++)
            for (size_t j = 0; j < stmt->declarations[i].dimension_count; j++)
                count += oasp_symbol_count_expr(stmt->declarations[i].dimensions[j]);
        for (size_t i = 0; i < stmt->constant_count; i++)
            count += oasp_symbol_count_expr(stmt->constants[i].value);
        count += oasp_symbol_count_block(&stmt->body);
        count += oasp_symbol_count_block(&stmt->alternative);
        for (const oasp_case_branch *branch = stmt->cases; branch; branch = branch->next) {
            for (size_t i = 0; i < branch->value_count; i++)
                count += oasp_symbol_count_expr(branch->values[i]);
            count += oasp_symbol_count_block(&branch->body);
        }
    }
    return count;
}

static size_t oasp_symbol_count_declarations(const oasp_block *block) {
    size_t count = 0;
    for (const oasp_stmt *stmt = block->first; stmt; stmt = stmt->next) {
        count += stmt->declaration_count + stmt->constant_count;
        if (stmt->kind == OASP_STMT_FOR || stmt->kind == OASP_STMT_FOR_EACH)
            count++;
        if (stmt->kind == OASP_STMT_ASSIGN || stmt->kind == OASP_STMT_SET_ASSIGN)
            count++;
        count += oasp_symbol_count_declarations(&stmt->body);
        count += oasp_symbol_count_declarations(&stmt->alternative);
        for (const oasp_case_branch *branch = stmt->cases; branch; branch = branch->next)
            count += oasp_symbol_count_declarations(&branch->body);
    }
    return count;
}

static size_t oasp_symbol_index(const oasp_slice *names, size_t count, oasp_slice name) {
    for (size_t i = 0; i < count; i++)
        if (oasp_symbol_name_equal(names[i], name))
            return i;
    return SIZE_MAX;
}

static bool oasp_symbol_simple_identifier(const oasp_expr *expr) {
    return expr && expr->kind == OASP_EXPR_IDENTIFIER && !memchr(expr->name.data, '.', expr->name.length);
}

static void oasp_symbol_add(oasp_slice *names, size_t *count, size_t capacity, oasp_slice name) {
    if (name.length && *count < capacity && oasp_symbol_index(names, *count, name) == SIZE_MAX)
        names[(*count)++] = name;
}

static void oasp_symbol_collect_names(const oasp_block *block, oasp_slice *names, size_t *count, size_t capacity, const oasp_slice *globals,
                                      size_t global_count, bool implicit) {
    for (const oasp_stmt *stmt = block->first; stmt; stmt = stmt->next) {
        for (size_t i = 0; i < stmt->declaration_count; i++)
            oasp_symbol_add(names, count, capacity, stmt->declarations[i].name);
        for (size_t i = 0; i < stmt->constant_count; i++)
            oasp_symbol_add(names, count, capacity, stmt->constants[i].name);
        if (implicit && (stmt->kind == OASP_STMT_FOR || stmt->kind == OASP_STMT_FOR_EACH) &&
            oasp_symbol_index(globals, global_count, stmt->name) == SIZE_MAX)
            oasp_symbol_add(names, count, capacity, stmt->name);
        if (implicit && (stmt->kind == OASP_STMT_ASSIGN || stmt->kind == OASP_STMT_SET_ASSIGN) &&
            oasp_symbol_simple_identifier(stmt->target) &&
            oasp_symbol_index(globals, global_count, stmt->target->name) == SIZE_MAX)
            oasp_symbol_add(names, count, capacity, stmt->target->name);
        if (stmt->kind != OASP_STMT_ROUTINE && stmt->kind != OASP_STMT_CLASS) {
            oasp_symbol_collect_names(&stmt->body, names, count, capacity, globals, global_count, implicit);
            oasp_symbol_collect_names(&stmt->alternative, names, count, capacity, globals, global_count, implicit);
            for (const oasp_case_branch *branch = stmt->cases; branch; branch = branch->next)
                oasp_symbol_collect_names(&branch->body, names, count, capacity, globals, global_count, implicit);
        }
    }
}

static const oasp_stmt *oasp_symbol_find_routine(const oasp_block *block, oasp_slice name) {
    for (const oasp_stmt *stmt = block->first; stmt; stmt = stmt->next)
        if (stmt->kind == OASP_STMT_ROUTINE && oasp_symbol_name_equal(stmt->name, name))
            return stmt;
    return NULL;
}

static bool oasp_symbol_record(oasp_symbol_pass *pass, const oasp_expr *expr, oasp_symbol symbol) {
    if (!expr)
        return true;
    size_t mask = pass->resolution->capacity - 1;
    size_t index = ((uintptr_t)expr >> 3) & mask;
    while (pass->resolution->entries[index].expr && pass->resolution->entries[index].expr != expr)
        index = (index + 1) & mask;
    pass->resolution->entries[index].expr = expr;
    pass->resolution->entries[index].symbol = symbol;
    return true;
}

static oasp_symbol oasp_symbol_classify(oasp_symbol_pass *pass, const oasp_expr *expr, bool callee) {
    oasp_symbol result = {OASP_SYMBOL_DYNAMIC, oasp_symbol_id(expr->name), UINT32_MAX, OASP_VALUE_EMPTY};
    size_t index = oasp_symbol_index(pass->parameters, pass->parameter_count, expr->name);
    if (index != SIZE_MAX) {
        result.kind = OASP_SYMBOL_PARAMETER;
        result.slot = (uint32_t)index;
        return result;
    }
    index = oasp_symbol_index(pass->locals, pass->local_count, expr->name);
    if (index != SIZE_MAX) {
        result.kind = OASP_SYMBOL_LOCAL;
        result.slot = (uint32_t)index;
        return result;
    }
    index = oasp_symbol_index(pass->globals, pass->global_count, expr->name);
    if (index != SIZE_MAX) {
        result.kind = OASP_SYMBOL_MODULE_GLOBAL;
        result.slot = (uint32_t)index;
        return result;
    }
    if (callee && pass->class_stmt && oasp_symbol_find_routine(&pass->class_stmt->body, expr->name)) {
        result.kind = OASP_SYMBOL_CLASS_METHOD;
        return result;
    }
    if (callee && oasp_symbol_find_routine(pass->program, expr->name)) {
        result.kind = OASP_SYMBOL_ROUTINE;
        return result;
    }
    if (callee && oasp_symbol_is_builtin(expr->name)) {
        result.kind = OASP_SYMBOL_BUILTIN;
        result.slot = (uint32_t)oasp_builtin_id_for_name(expr->name);
    }
    return result;
}

static void oasp_symbol_visit_expr(oasp_symbol_pass *pass, const oasp_expr *expr, bool callee) {
    if (!expr || pass->failed)
        return;
    if (expr->kind == OASP_EXPR_IDENTIFIER)
        pass->failed = !oasp_symbol_record(pass, expr, oasp_symbol_classify(pass, expr, callee));
    else if (expr->kind == OASP_EXPR_MEMBER) {
        oasp_symbol member = {OASP_SYMBOL_DYNAMIC, oasp_symbol_id(expr->name),
                              (uint32_t)oasp_member_id_for_name(expr->name), OASP_VALUE_EMPTY};
        pass->failed = !oasp_symbol_record(pass, expr, member);
    }
    oasp_symbol_visit_expr(pass, expr->left, expr->kind == OASP_EXPR_CALL);
    oasp_symbol_visit_expr(pass, expr->right, false);
    for (size_t i = 0; i < expr->arg_count; i++)
        oasp_symbol_visit_expr(pass, expr->args[i], false);
}

static void oasp_symbol_visit_block(oasp_symbol_pass *pass, const oasp_block *block) {
    for (const oasp_stmt *stmt = block->first; stmt && !pass->failed; stmt = stmt->next) {
        const oasp_stmt *saved_routine = pass->routine;
        const oasp_stmt *saved_class = pass->class_stmt;
        oasp_slice *saved_locals = pass->locals;
        oasp_slice *saved_parameters = pass->parameters;
        size_t saved_local_count = pass->local_count;
        size_t saved_parameter_count = pass->parameter_count;
        if (stmt->kind == OASP_STMT_ROUTINE) {
            size_t capacity = oasp_symbol_count_declarations(&stmt->body) + stmt->parameter_count + 1;
            pass->locals = capacity ? oasp_arena_alloc(pass->arena, capacity * sizeof(*pass->locals), _Alignof(oasp_slice)) : NULL;
            if (capacity && !pass->locals) {
                pass->failed = true;
                return;
            }
            pass->local_count = 0;
            pass->parameters = stmt->parameter_count
                                   ? oasp_arena_alloc(pass->arena, stmt->parameter_count * sizeof(*pass->parameters),
                                                      _Alignof(oasp_slice))
                                   : NULL;
            if (stmt->parameter_count && !pass->parameters) {
                pass->failed = true;
                return;
            }
            pass->parameter_count = stmt->parameter_count;
            pass->routine = stmt;
            for (size_t i = 0; i < stmt->parameter_count; i++) {
                pass->parameters[i] = stmt->parameters[i].name;
                oasp_symbol_add(pass->locals, &pass->local_count, capacity, stmt->parameters[i].name);
            }
            if (stmt->routine_kind != OASP_ROUTINE_SUB)
                oasp_symbol_add(pass->locals, &pass->local_count, capacity, stmt->name);
            oasp_symbol_collect_names(&stmt->body, pass->locals, &pass->local_count, capacity, pass->globals,
                                      pass->global_count, !pass->module_dynamic && !pass->class_stmt);
        } else if (stmt->kind == OASP_STMT_CLASS) {
            pass->class_stmt = stmt;
        }
        oasp_symbol_visit_expr(pass, stmt->target, false);
        oasp_symbol_visit_expr(pass, stmt->value, false);
        oasp_symbol_visit_expr(pass, stmt->condition, false);
        oasp_symbol_visit_expr(pass, stmt->limit, false);
        oasp_symbol_visit_expr(pass, stmt->step, false);
        for (size_t i = 0; i < stmt->expression_count; i++)
            oasp_symbol_visit_expr(pass, stmt->expressions[i], false);
        for (size_t i = 0; i < stmt->declaration_count; i++)
            for (size_t j = 0; j < stmt->declarations[i].dimension_count; j++)
                oasp_symbol_visit_expr(pass, stmt->declarations[i].dimensions[j], false);
        for (size_t i = 0; i < stmt->constant_count; i++)
            oasp_symbol_visit_expr(pass, stmt->constants[i].value, false);
        oasp_symbol_visit_block(pass, &stmt->body);
        oasp_symbol_visit_block(pass, &stmt->alternative);
        for (const oasp_case_branch *branch = stmt->cases; branch; branch = branch->next) {
            for (size_t i = 0; i < branch->value_count; i++)
                oasp_symbol_visit_expr(pass, branch->values[i], false);
            oasp_symbol_visit_block(pass, &branch->body);
        }
        pass->routine = saved_routine;
        pass->class_stmt = saved_class;
        pass->locals = saved_locals;
        pass->local_count = saved_local_count;
        pass->parameters = saved_parameters;
        pass->parameter_count = saved_parameter_count;
    }
}

bool oasp_symbol_resolve_program(oasp_arena *arena, const oasp_block *program, bool module_dynamic, oasp_symbol_resolution **result) {
    if (!arena || !program || !result)
        return false;
    size_t expression_count = oasp_symbol_count_block(program);
    size_t capacity = 16;
    while (capacity < expression_count * 2) {
        if (capacity > SIZE_MAX / 2)
            return false;
        capacity *= 2;
    }
    oasp_symbol_resolution *resolution = oasp_arena_alloc(arena, sizeof(*resolution), _Alignof(oasp_symbol_resolution));
    oasp_symbol_entry *entries = oasp_arena_alloc(arena, capacity * sizeof(*entries), _Alignof(oasp_symbol_entry));
    size_t global_capacity = oasp_symbol_count_declarations(program);
    oasp_slice *globals = global_capacity
                              ? oasp_arena_alloc(arena, global_capacity * sizeof(*globals), _Alignof(oasp_slice))
                              : NULL;
    if (!resolution || !entries || (global_capacity && !globals))
        return false;
    memset(entries, 0, capacity * sizeof(*entries));
    resolution->entries = entries;
    resolution->capacity = capacity;
    oasp_symbol_pass pass = {
        .arena = arena,
        .resolution = resolution,
        .program = program,
        .globals = globals,
        .module_dynamic = module_dynamic,
    };
    oasp_symbol_collect_names(program, globals, &pass.global_count, global_capacity, NULL, 0, !module_dynamic);
    oasp_symbol_visit_block(&pass, program);
    if (pass.failed)
        return false;
    *result = resolution;
    return true;
}

bool oasp_symbol_lookup(const oasp_symbol_resolution *resolution, const oasp_expr *expr, oasp_symbol *result) {
    if (!resolution || !expr || !result || !resolution->capacity)
        return false;
    size_t mask = resolution->capacity - 1;
    size_t index = ((uintptr_t)expr >> 3) & mask;
    while (resolution->entries[index].expr) {
        if (resolution->entries[index].expr == expr) {
            *result = resolution->entries[index].symbol;
            return true;
        }
        index = (index + 1) & mask;
    }
    return false;
}
