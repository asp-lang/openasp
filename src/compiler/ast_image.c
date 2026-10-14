// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/compiler/ast_image.c`): Compiler pipeline. Arena-owned tokens, AST
 * nodes, symbols, bytecode, and immutable images move through this stage without per-node
 * frees. Length/count variables bound every indexed operation and malformed input must fail
 * before execution.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#include "ast_image.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OASP_AST_MAGIC UINT64_C(0x4f41535041535431)
#define OASP_AST_ABI 2u
#define OASP_AST_NONE UINT32_MAX

typedef struct {
    uint32_t offset;
    uint32_t length;
} oasp_ast_slice;

typedef struct {
    uint64_t magic;
    uint32_t abi;
    uint32_t header_size;
    uint64_t total_size;
    uint32_t program_first;
    uint32_t program_last;
    uint32_t expression_offset;
    uint32_t expression_count;
    uint32_t statement_offset;
    uint32_t statement_count;
    uint32_t case_offset;
    uint32_t case_count;
} oasp_ast_header;

typedef struct {
    uint32_t self_offset;
    uint32_t kind;
    uint32_t literal_kind;
    uint32_t reserved;
    uint64_t line;
    oasp_ast_slice name;
    oasp_ast_slice literal_slice;
    uint64_t literal_bits;
    uint32_t left;
    uint32_t right;
    uint32_t args_offset;
    uint32_t arg_count;
} oasp_ast_expression;

typedef struct {
    oasp_ast_slice name;
    uint32_t dimensions_offset;
    uint32_t dimension_count;
} oasp_ast_declarator;

typedef struct {
    oasp_ast_slice name;
    uint32_t value;
} oasp_ast_constant;

typedef struct {
    oasp_ast_slice name;
    uint32_t flags;
} oasp_ast_parameter;

typedef struct {
    uint32_t self_offset;
    uint32_t next;
    uint32_t values_offset;
    uint32_t value_count;
    uint32_t body_first;
    uint32_t body_last;
    uint32_t is_else;
    uint32_t reserved;
} oasp_ast_case;

typedef struct {
    uint32_t self_offset;
    uint32_t kind;
    uint32_t routine_kind;
    uint64_t line;
    uint32_t next;
    uint32_t flags;
    oasp_ast_slice name;
    uint32_t target;
    uint32_t value;
    uint32_t condition;
    uint32_t limit;
    uint32_t step;
    uint32_t expressions_offset;
    uint32_t expression_count;
    uint32_t body_first;
    uint32_t body_last;
    uint32_t alternative_first;
    uint32_t alternative_last;
    uint32_t first_case;
    uint32_t names_offset;
    uint32_t name_count;
    uint32_t declarations_offset;
    uint32_t declaration_count;
    uint32_t constants_offset;
    uint32_t constant_count;
    uint32_t parameters_offset;
    uint32_t parameter_count;
} oasp_ast_statement;

typedef struct {
    const oasp_stmt **statements;
    size_t statement_count;
    size_t statement_capacity;
    const oasp_expr **expressions;
    size_t expression_count;
    size_t expression_capacity;
    const oasp_case_branch **cases;
    size_t case_count;
    size_t case_capacity;
} oasp_ast_map;

static bool oasp_ast_u32(size_t value, uint32_t *result) {
    if (value > UINT32_MAX)
        return false;
    *result = (uint32_t)value;
    return true;
}

static bool oasp_ast_range(size_t size, uint32_t offset, size_t count, size_t item_size) {
    return offset <= size && count <= SIZE_MAX / item_size && count * item_size <= size - offset;
}

static bool oasp_ast_aligned(uint32_t offset, size_t count, size_t alignment) {
    return !count || offset % alignment == 0;
}

static bool oasp_ast_push(const void ***items, size_t *count, size_t *capacity, const void *item) {
    if (!item)
        return true;
    for (size_t i = 0; i < *count; i++)
        if ((*items)[i] == item)
            return true;
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 64;
        if (next < *capacity || next > SIZE_MAX / sizeof(**items))
            return false;
        const void **grown = realloc((void *)*items, next * sizeof(**items));
        if (!grown)
            return false;
        *items = grown;
        *capacity = next;
    }
    (*items)[(*count)++] = item;
    return true;
}

static bool oasp_ast_map_expression(oasp_ast_map *map, const oasp_expr *expression) {
    if (!expression)
        return true;
    size_t before = map->expression_count;
    if (!oasp_ast_push((const void ***)&map->expressions, &map->expression_count, &map->expression_capacity, expression))
        return false;
    if (before == map->expression_count)
        return true;
    if (!oasp_ast_map_expression(map, expression->left) || !oasp_ast_map_expression(map, expression->right))
        return false;
    for (size_t i = 0; i < expression->arg_count; i++)
        if (!oasp_ast_map_expression(map, expression->args[i]))
            return false;
    return true;
}

static bool oasp_ast_map_block(oasp_ast_map *map, const oasp_block *block);

static bool oasp_ast_map_statement(oasp_ast_map *map, const oasp_stmt *statement) {
    if (!oasp_ast_push((const void ***)&map->statements, &map->statement_count, &map->statement_capacity, statement))
        return false;
    if (!oasp_ast_map_expression(map, statement->target) || !oasp_ast_map_expression(map, statement->value) ||
        !oasp_ast_map_expression(map, statement->condition) || !oasp_ast_map_expression(map, statement->limit) ||
        !oasp_ast_map_expression(map, statement->step))
        return false;
    for (size_t i = 0; i < statement->expression_count; i++)
        if (!oasp_ast_map_expression(map, statement->expressions[i]))
            return false;
    for (size_t i = 0; i < statement->declaration_count; i++)
        for (size_t j = 0; j < statement->declarations[i].dimension_count; j++)
            if (!oasp_ast_map_expression(map, statement->declarations[i].dimensions[j]))
                return false;
    for (size_t i = 0; i < statement->constant_count; i++)
        if (!oasp_ast_map_expression(map, statement->constants[i].value))
            return false;
    if (!oasp_ast_map_block(map, &statement->body) || !oasp_ast_map_block(map, &statement->alternative))
        return false;
    for (const oasp_case_branch *branch = statement->cases; branch; branch = branch->next) {
        if (!oasp_ast_push((const void ***)&map->cases, &map->case_count, &map->case_capacity, branch))
            return false;
        for (size_t i = 0; i < branch->value_count; i++)
            if (!oasp_ast_map_expression(map, branch->values[i]))
                return false;
        if (!oasp_ast_map_block(map, &branch->body))
            return false;
    }
    return true;
}

static bool oasp_ast_map_block(oasp_ast_map *map, const oasp_block *block) {
    for (const oasp_stmt *statement = block->first; statement; statement = statement->next)
        if (!oasp_ast_map_statement(map, statement))
            return false;
    return true;
}

static uint32_t oasp_ast_pointer_index(const void *const *items, size_t count, const void *item) {
    if (!item)
        return OASP_AST_NONE;
    for (size_t i = 0; i < count; i++)
        if (items[i] == item)
            return (uint32_t)i;
    return OASP_AST_NONE;
}

static uint32_t oasp_ast_statement_index(const oasp_ast_map *map, const oasp_stmt *statement) {
    return oasp_ast_pointer_index((const void *const *)map->statements, map->statement_count, statement);
}

static uint32_t oasp_ast_expression_index(const oasp_ast_map *map, const oasp_expr *expression) {
    return oasp_ast_pointer_index((const void *const *)map->expressions, map->expression_count, expression);
}

static uint32_t oasp_ast_case_index(const oasp_ast_map *map, const oasp_case_branch *branch) {
    return oasp_ast_pointer_index((const void *const *)map->cases, map->case_count, branch);
}

static bool oasp_ast_align(oasp_buffer *output, size_t alignment) {
    static const unsigned char zeros[8] = {0};
    size_t padding = (alignment - output->length % alignment) % alignment;
    return !padding || oasp_buffer_append(output, zeros, padding);
}

static bool oasp_ast_append(oasp_buffer *output, const void *data, size_t count, size_t item_size, uint32_t *offset) {
    if (!oasp_ast_align(output, 8) || !oasp_ast_u32(output->length, offset) || count > SIZE_MAX / item_size)
        return false;
    return !count || oasp_buffer_append(output, data, count * item_size);
}

static bool oasp_ast_append_slice(oasp_buffer *output, oasp_slice source, oasp_ast_slice *destination) {
    if (!oasp_ast_u32(output->length, &destination->offset) || !oasp_ast_u32(source.length, &destination->length))
        return false;
    return !source.length || oasp_buffer_append(output, source.data, source.length);
}

static bool oasp_ast_append_expression_indices(oasp_buffer *output, const oasp_ast_map *map, oasp_expr *const *expressions, size_t count, uint32_t *offset) {
    uint32_t *indices = count ? malloc(count * sizeof(*indices)) : NULL;
    if (count && !indices)
        return false;
    for (size_t i = 0; i < count; i++)
        indices[i] = oasp_ast_expression_index(map, expressions[i]);
    bool ok = oasp_ast_append(output, indices, count, sizeof(*indices), offset);
    free(indices);
    return ok;
}

static bool oasp_ast_encode_literal(oasp_buffer *output, const oasp_value *literal, oasp_ast_expression *record) {
    record->literal_kind = (uint32_t)literal->kind;
    switch (literal->kind) {
    case OASP_VALUE_EMPTY:
    case OASP_VALUE_NULL:
    case OASP_VALUE_NOTHING:
        return true;
    case OASP_VALUE_BOOL:
        record->literal_bits = literal->as.boolean ? 1 : 0;
        return true;
    case OASP_VALUE_INT:
        memcpy(&record->literal_bits, &literal->as.integer, sizeof(literal->as.integer));
        return true;
    case OASP_VALUE_FLOAT:
        memcpy(&record->literal_bits, &literal->as.floating, sizeof(literal->as.floating));
        return true;
    case OASP_VALUE_STRING:
        return oasp_ast_append_slice(output, literal->as.string, &record->literal_slice);
    case OASP_VALUE_DATE:
        return oasp_ast_append_slice(output, literal->as.date, &record->literal_slice);
    case OASP_VALUE_OBJECT:
    case OASP_VALUE_ARRAY:
        return false;
    }
    return false;
}

static void oasp_ast_map_dispose(oasp_ast_map *map) {
    free(map->statements);
    free(map->expressions);
    free(map->cases);
}

bool oasp_ast_image_encode(const oasp_block *program, oasp_buffer *output, size_t *statement_count, size_t *expression_count) {
    if (!program || !output || !oasp_buffer_init(output, 4096))
        return false;
    oasp_ast_header header;
    memset(&header, 0, sizeof(header));
    if (!oasp_buffer_append(output, &header, sizeof(header)))
        goto fail;
    oasp_ast_map map = {0};
    if (!oasp_ast_map_block(&map, program))
        goto fail_map;
    oasp_ast_expression *expressions = map.expression_count ? calloc(map.expression_count, sizeof(*expressions)) : NULL;
    oasp_ast_statement *statements = map.statement_count ? calloc(map.statement_count, sizeof(*statements)) : NULL;
    oasp_ast_case *cases = map.case_count ? calloc(map.case_count, sizeof(*cases)) : NULL;
    if ((map.expression_count && !expressions) || (map.statement_count && !statements) || (map.case_count && !cases))
        goto fail_records;

    for (size_t i = 0; i < map.expression_count; i++) {
        const oasp_expr *source = map.expressions[i];
        oasp_ast_expression *destination = &expressions[i];
        destination->kind = (uint32_t)source->kind;
        destination->line = source->line;
        destination->left = oasp_ast_expression_index(&map, source->left);
        destination->right = oasp_ast_expression_index(&map, source->right);
        if (!oasp_ast_append_slice(output, source->name, &destination->name) ||
            !oasp_ast_encode_literal(output, &source->literal, destination) ||
            !oasp_ast_u32(source->arg_count, &destination->arg_count) ||
            !oasp_ast_append_expression_indices(output, &map, source->args, source->arg_count, &destination->args_offset))
            goto fail_records;
    }

    for (size_t i = 0; i < map.case_count; i++) {
        const oasp_case_branch *source = map.cases[i];
        oasp_ast_case *destination = &cases[i];
        destination->next = oasp_ast_case_index(&map, source->next);
        destination->body_first = oasp_ast_statement_index(&map, source->body.first);
        destination->body_last = oasp_ast_statement_index(&map, source->body.last);
        destination->is_else = source->is_else ? 1u : 0u;
        if (!oasp_ast_u32(source->value_count, &destination->value_count) ||
            !oasp_ast_append_expression_indices(output, &map, source->values, source->value_count, &destination->values_offset))
            goto fail_records;
    }

    for (size_t i = 0; i < map.statement_count; i++) {
        const oasp_stmt *source = map.statements[i];
        oasp_ast_statement *destination = &statements[i];
        destination->kind = (uint32_t)source->kind;
        destination->routine_kind = (uint32_t)source->routine_kind;
        destination->line = source->line;
        destination->next = oasp_ast_statement_index(&map, source->next);
        destination->flags = (source->is_default ? 1u : 0u) | (source->until ? 2u : 0u) | (source->post_condition ? 4u : 0u) |
                             (source->preserve ? 8u : 0u);
        destination->target = oasp_ast_expression_index(&map, source->target);
        destination->value = oasp_ast_expression_index(&map, source->value);
        destination->condition = oasp_ast_expression_index(&map, source->condition);
        destination->limit = oasp_ast_expression_index(&map, source->limit);
        destination->step = oasp_ast_expression_index(&map, source->step);
        destination->body_first = oasp_ast_statement_index(&map, source->body.first);
        destination->body_last = oasp_ast_statement_index(&map, source->body.last);
        destination->alternative_first = oasp_ast_statement_index(&map, source->alternative.first);
        destination->alternative_last = oasp_ast_statement_index(&map, source->alternative.last);
        destination->first_case = oasp_ast_case_index(&map, source->cases);
        if (!oasp_ast_append_slice(output, source->name, &destination->name) ||
            !oasp_ast_u32(source->expression_count, &destination->expression_count) ||
            !oasp_ast_append_expression_indices(output, &map, source->expressions, source->expression_count, &destination->expressions_offset))
            goto fail_records;

        oasp_ast_slice *names = source->name_count ? calloc(source->name_count, sizeof(*names)) : NULL;
        if (source->name_count && !names)
            goto fail_records;
        bool names_ok = true;
        for (size_t j = 0; j < source->name_count; j++)
            if (!oasp_ast_append_slice(output, source->names[j], &names[j])) {
                names_ok = false;
                break;
            }
        if (!names_ok || !oasp_ast_u32(source->name_count, &destination->name_count) ||
            !oasp_ast_append(output, names, source->name_count, sizeof(*names), &destination->names_offset)) {
            free(names);
            goto fail_records;
        }
        free(names);

        oasp_ast_declarator *declarations =
            source->declaration_count ? calloc(source->declaration_count, sizeof(*declarations)) : NULL;
        if (source->declaration_count && !declarations)
            goto fail_records;
        bool declarations_ok = true;
        for (size_t j = 0; j < source->declaration_count; j++) {
            if (!oasp_ast_append_slice(output, source->declarations[j].name, &declarations[j].name) ||
                !oasp_ast_u32(source->declarations[j].dimension_count, &declarations[j].dimension_count) ||
                !oasp_ast_append_expression_indices(output, &map, source->declarations[j].dimensions,
                                                    source->declarations[j].dimension_count, &declarations[j].dimensions_offset)) {
                declarations_ok = false;
                break;
            }
        }
        if (!declarations_ok || !oasp_ast_u32(source->declaration_count, &destination->declaration_count) ||
            !oasp_ast_append(output, declarations, source->declaration_count, sizeof(*declarations), &destination->declarations_offset)) {
            free(declarations);
            goto fail_records;
        }
        free(declarations);

        oasp_ast_constant *constants = source->constant_count ? calloc(source->constant_count, sizeof(*constants)) : NULL;
        if (source->constant_count && !constants)
            goto fail_records;
        bool constants_ok = true;
        for (size_t j = 0; j < source->constant_count; j++) {
            constants[j].value = oasp_ast_expression_index(&map, source->constants[j].value);
            if (!oasp_ast_append_slice(output, source->constants[j].name, &constants[j].name)) {
                constants_ok = false;
                break;
            }
        }
        if (!constants_ok || !oasp_ast_u32(source->constant_count, &destination->constant_count) ||
            !oasp_ast_append(output, constants, source->constant_count, sizeof(*constants), &destination->constants_offset)) {
            free(constants);
            goto fail_records;
        }
        free(constants);

        oasp_ast_parameter *parameters = source->parameter_count ? calloc(source->parameter_count, sizeof(*parameters)) : NULL;
        if (source->parameter_count && !parameters)
            goto fail_records;
        bool parameters_ok = true;
        for (size_t j = 0; j < source->parameter_count; j++) {
            parameters[j].flags = (source->parameters[j].by_value ? 1u : 0u) | (source->parameters[j].array ? 2u : 0u);
            if (!oasp_ast_append_slice(output, source->parameters[j].name, &parameters[j].name)) {
                parameters_ok = false;
                break;
            }
        }
        if (!parameters_ok || !oasp_ast_u32(source->parameter_count, &destination->parameter_count) ||
            !oasp_ast_append(output, parameters, source->parameter_count, sizeof(*parameters), &destination->parameters_offset)) {
            free(parameters);
            goto fail_records;
        }
        free(parameters);
    }

    if (!oasp_ast_u32(map.expression_count, &header.expression_count) || !oasp_ast_u32(map.statement_count, &header.statement_count) ||
        !oasp_ast_u32(map.case_count, &header.case_count) ||
        !oasp_ast_append(output, expressions, map.expression_count, sizeof(*expressions), &header.expression_offset) ||
        !oasp_ast_append(output, statements, map.statement_count, sizeof(*statements), &header.statement_offset) ||
        !oasp_ast_append(output, cases, map.case_count, sizeof(*cases), &header.case_offset))
        goto fail_records;
    oasp_ast_expression *encoded_expressions = (void *)(output->data + header.expression_offset);
    oasp_ast_statement *encoded_statements = (void *)(output->data + header.statement_offset);
    oasp_ast_case *encoded_cases = (void *)(output->data + header.case_offset);
    for (size_t i = 0; i < map.expression_count; i++)
        encoded_expressions[i].self_offset = header.expression_offset + (uint32_t)(i * sizeof(*encoded_expressions));
    for (size_t i = 0; i < map.statement_count; i++)
        encoded_statements[i].self_offset = header.statement_offset + (uint32_t)(i * sizeof(*encoded_statements));
    for (size_t i = 0; i < map.case_count; i++)
        encoded_cases[i].self_offset = header.case_offset + (uint32_t)(i * sizeof(*encoded_cases));
    header.magic = OASP_AST_MAGIC;
    header.abi = OASP_AST_ABI;
    header.header_size = sizeof(header);
    header.total_size = output->length;
    header.program_first = oasp_ast_statement_index(&map, program->first);
    header.program_last = oasp_ast_statement_index(&map, program->last);
    memcpy(output->data, &header, sizeof(header));
    if (statement_count)
        *statement_count = map.statement_count;
    if (expression_count)
        *expression_count = map.expression_count;
    free(expressions);
    free(statements);
    free(cases);
    oasp_ast_map_dispose(&map);
    return true;

fail_records:
    free(expressions);
    free(statements);
    free(cases);
fail_map:
    oasp_ast_map_dispose(&map);
fail:
    oasp_buffer_dispose(output);
    return false;
}

static bool oasp_ast_index_valid(uint32_t index, size_t count) { return index == OASP_AST_NONE || index < count; }

static bool oasp_ast_slice_valid(const oasp_ast_header *header, oasp_ast_slice slice) {
    return oasp_ast_range((size_t)header->total_size, slice.offset, slice.length, 1);
}

static bool oasp_ast_index_list_valid(const oasp_ast_header *header, uint32_t offset, uint32_t count, size_t limit) {
    if (!oasp_ast_aligned(offset, count, _Alignof(uint32_t)) ||
        !oasp_ast_range((size_t)header->total_size, offset, count, sizeof(uint32_t)))
        return false;
    const uint32_t *indices = (const void *)((const char *)header + offset);
    for (size_t i = 0; i < count; i++)
        if (!oasp_ast_index_valid(indices[i], limit))
            return false;
    return true;
}

bool oasp_ast_image_open(const void *data, size_t size, size_t statement_count, size_t expression_count, oasp_ast_image *image) {
    if (!data || !image || size < sizeof(oasp_ast_header) ||
        (uintptr_t)data % _Alignof(oasp_ast_header) != 0)
        return false;
    const oasp_ast_header *header = data;
    if (header->magic != OASP_AST_MAGIC || header->abi != OASP_AST_ABI || header->header_size != sizeof(*header) || header->total_size != size ||
        header->statement_count != statement_count || header->expression_count != expression_count ||
        !oasp_ast_aligned(header->expression_offset, header->expression_count, _Alignof(oasp_ast_expression)) ||
        !oasp_ast_range(size, header->expression_offset, header->expression_count, sizeof(oasp_ast_expression)) ||
        !oasp_ast_aligned(header->statement_offset, header->statement_count, _Alignof(oasp_ast_statement)) ||
        !oasp_ast_range(size, header->statement_offset, header->statement_count, sizeof(oasp_ast_statement)) ||
        !oasp_ast_aligned(header->case_offset, header->case_count, _Alignof(oasp_ast_case)) ||
        !oasp_ast_range(size, header->case_offset, header->case_count, sizeof(oasp_ast_case)) ||
        !oasp_ast_index_valid(header->program_first, header->statement_count) || !oasp_ast_index_valid(header->program_last, header->statement_count))
        return false;
    const oasp_ast_expression *expressions = (const void *)((const char *)data + header->expression_offset);
    const oasp_ast_statement *statements = (const void *)((const char *)data + header->statement_offset);
    const oasp_ast_case *cases = (const void *)((const char *)data + header->case_offset);
    for (size_t i = 0; i < header->expression_count; i++) {
        const oasp_ast_expression *record = &expressions[i];
        if (record->self_offset != header->expression_offset + i * sizeof(*record) || record->kind > OASP_EXPR_IS_NOT ||
            record->literal_kind > OASP_VALUE_DATE || record->literal_kind == OASP_VALUE_OBJECT ||
            record->literal_kind == OASP_VALUE_ARRAY || record->line > SIZE_MAX || !oasp_ast_slice_valid(header, record->name) ||
            !oasp_ast_slice_valid(header, record->literal_slice) || !oasp_ast_index_valid(record->left, header->expression_count) ||
            !oasp_ast_index_valid(record->right, header->expression_count) ||
            !oasp_ast_index_list_valid(header, record->args_offset, record->arg_count, header->expression_count))
            return false;
    }
    for (size_t i = 0; i < header->case_count; i++) {
        const oasp_ast_case *record = &cases[i];
        if (record->self_offset != header->case_offset + i * sizeof(*record) || !oasp_ast_index_valid(record->next, header->case_count) ||
            !oasp_ast_index_valid(record->body_first, header->statement_count) ||
            !oasp_ast_index_valid(record->body_last, header->statement_count) || record->is_else > 1 ||
            !oasp_ast_index_list_valid(header, record->values_offset, record->value_count, header->expression_count))
            return false;
    }
    for (size_t i = 0; i < header->statement_count; i++) {
        const oasp_ast_statement *record = &statements[i];
        if (record->self_offset != header->statement_offset + i * sizeof(*record) || record->kind > OASP_STMT_WITH ||
            record->routine_kind > OASP_ROUTINE_PROPERTY_SET || record->line > SIZE_MAX || (record->flags & ~15u) ||
            !oasp_ast_slice_valid(header, record->name) || !oasp_ast_index_valid(record->next, header->statement_count) ||
            !oasp_ast_index_valid(record->target, header->expression_count) || !oasp_ast_index_valid(record->value, header->expression_count) ||
            !oasp_ast_index_valid(record->condition, header->expression_count) || !oasp_ast_index_valid(record->limit, header->expression_count) ||
            !oasp_ast_index_valid(record->step, header->expression_count) || !oasp_ast_index_valid(record->body_first, header->statement_count) ||
            !oasp_ast_index_valid(record->body_last, header->statement_count) ||
            !oasp_ast_index_valid(record->alternative_first, header->statement_count) ||
            !oasp_ast_index_valid(record->alternative_last, header->statement_count) || !oasp_ast_index_valid(record->first_case, header->case_count) ||
            !oasp_ast_index_list_valid(header, record->expressions_offset, record->expression_count, header->expression_count) ||
            !oasp_ast_aligned(record->names_offset, record->name_count, _Alignof(oasp_ast_slice)) ||
            !oasp_ast_range(size, record->names_offset, record->name_count, sizeof(oasp_ast_slice)) ||
            !oasp_ast_aligned(record->declarations_offset, record->declaration_count, _Alignof(oasp_ast_declarator)) ||
            !oasp_ast_range(size, record->declarations_offset, record->declaration_count, sizeof(oasp_ast_declarator)) ||
            !oasp_ast_aligned(record->constants_offset, record->constant_count, _Alignof(oasp_ast_constant)) ||
            !oasp_ast_range(size, record->constants_offset, record->constant_count, sizeof(oasp_ast_constant)) ||
            !oasp_ast_aligned(record->parameters_offset, record->parameter_count, _Alignof(oasp_ast_parameter)) ||
            !oasp_ast_range(size, record->parameters_offset, record->parameter_count, sizeof(oasp_ast_parameter)))
            return false;
        const oasp_ast_slice *names = (const void *)((const char *)data + record->names_offset);
        const oasp_ast_declarator *declarations = (const void *)((const char *)data + record->declarations_offset);
        const oasp_ast_constant *constants = (const void *)((const char *)data + record->constants_offset);
        const oasp_ast_parameter *parameters = (const void *)((const char *)data + record->parameters_offset);
        for (size_t j = 0; j < record->name_count; j++)
            if (!oasp_ast_slice_valid(header, names[j]))
                return false;
        for (size_t j = 0; j < record->declaration_count; j++)
            if (!oasp_ast_slice_valid(header, declarations[j].name) ||
                !oasp_ast_index_list_valid(header, declarations[j].dimensions_offset, declarations[j].dimension_count, header->expression_count))
                return false;
        for (size_t j = 0; j < record->constant_count; j++)
            if (!oasp_ast_slice_valid(header, constants[j].name) || !oasp_ast_index_valid(constants[j].value, header->expression_count))
                return false;
        for (size_t j = 0; j < record->parameter_count; j++)
            if (!oasp_ast_slice_valid(header, parameters[j].name) || (parameters[j].flags & ~3u))
                return false;
    }
    image->data = data;
    image->size = size;
    image->statement_count = statement_count;
    image->expression_count = expression_count;
    return true;
}

static oasp_slice oasp_ast_materialize_slice(const char *bytes, oasp_ast_slice slice) {
    oasp_slice result = {bytes + slice.offset, slice.length};
    return result;
}

static oasp_expr *oasp_ast_expression_pointer(oasp_expr *items, size_t count, uint32_t index) {
    return index == OASP_AST_NONE || index >= count ? NULL : &items[index];
}

static oasp_stmt *oasp_ast_statement_pointer(oasp_stmt *items, size_t count, uint32_t index) {
    return index == OASP_AST_NONE || index >= count ? NULL : &items[index];
}

static oasp_case_branch *oasp_ast_case_pointer(oasp_case_branch *items, size_t count, uint32_t index) {
    return index == OASP_AST_NONE || index >= count ? NULL : &items[index];
}

static bool oasp_ast_materialize_expression_list(oasp_arena *arena, const char *bytes, uint32_t offset, size_t count, oasp_expr *items,
                                                 size_t item_count, oasp_expr ***result) {
    *result = count ? oasp_arena_alloc(arena, count * sizeof(**result), _Alignof(oasp_expr *)) : NULL;
    if (count && !*result)
        return false;
    const uint32_t *indices = (const void *)(bytes + offset);
    for (size_t i = 0; i < count; i++)
        (*result)[i] = oasp_ast_expression_pointer(items, item_count, indices[i]);
    return true;
}

bool oasp_ast_image_materialize(const oasp_ast_image *image, oasp_arena *arena, oasp_block *program, const oasp_stmt ***statement_map, const oasp_expr ***expression_map) {
    if (!image || !arena || !program)
        return false;
    oasp_ast_image checked;
    if (!oasp_ast_image_open(image->data, image->size, image->statement_count, image->expression_count, &checked))
        return false;
    const char *bytes = image->data;
    const oasp_ast_header *header = image->data;
    const oasp_ast_expression *expression_records = (const void *)(bytes + header->expression_offset);
    const oasp_ast_statement *statement_records = (const void *)(bytes + header->statement_offset);
    const oasp_ast_case *case_records = (const void *)(bytes + header->case_offset);
    oasp_expr *expressions =
        header->expression_count ? oasp_arena_alloc(arena, header->expression_count * sizeof(*expressions), _Alignof(oasp_expr)) : NULL;
    oasp_stmt *statements =
        header->statement_count ? oasp_arena_alloc(arena, header->statement_count * sizeof(*statements), _Alignof(oasp_stmt)) : NULL;
    oasp_case_branch *cases = header->case_count ? oasp_arena_alloc(arena, header->case_count * sizeof(*cases), _Alignof(oasp_case_branch)) : NULL;
    const oasp_stmt **statement_pointers =
        statement_map && header->statement_count
            ? oasp_arena_alloc(arena, header->statement_count * sizeof(*statement_pointers), _Alignof(const oasp_stmt *))
            : NULL;
    const oasp_expr **expression_pointers =
        expression_map && header->expression_count
            ? oasp_arena_alloc(arena, header->expression_count * sizeof(*expression_pointers), _Alignof(const oasp_expr *))
            : NULL;
    if ((header->expression_count && (!expressions || (expression_map && !expression_pointers))) ||
        (header->statement_count && (!statements || (statement_map && !statement_pointers))) ||
        (header->case_count && !cases))
        return false;
    if (expressions)
        memset(expressions, 0, header->expression_count * sizeof(*expressions));
    if (statements)
        memset(statements, 0, header->statement_count * sizeof(*statements));
    if (cases)
        memset(cases, 0, header->case_count * sizeof(*cases));

    for (size_t i = 0; i < header->expression_count; i++) {
        const oasp_ast_expression *source = &expression_records[i];
        oasp_expr *destination = &expressions[i];
        destination->kind = (oasp_expr_kind)source->kind;
        destination->line = (size_t)source->line;
        destination->name = oasp_ast_materialize_slice(bytes, source->name);
        destination->literal.kind = (oasp_value_kind)source->literal_kind;
        switch (destination->literal.kind) {
        case OASP_VALUE_BOOL:
            destination->literal.as.boolean = source->literal_bits != 0;
            break;
        case OASP_VALUE_INT:
            memcpy(&destination->literal.as.integer, &source->literal_bits, sizeof(destination->literal.as.integer));
            break;
        case OASP_VALUE_FLOAT:
            memcpy(&destination->literal.as.floating, &source->literal_bits, sizeof(destination->literal.as.floating));
            break;
        case OASP_VALUE_STRING:
            destination->literal.as.string = oasp_ast_materialize_slice(bytes, source->literal_slice);
            break;
        case OASP_VALUE_DATE:
            destination->literal.as.date = oasp_ast_materialize_slice(bytes, source->literal_slice);
            break;
        default:
            break;
        }
        destination->left = oasp_ast_expression_pointer(expressions, header->expression_count, source->left);
        destination->right = oasp_ast_expression_pointer(expressions, header->expression_count, source->right);
        destination->arg_count = source->arg_count;
        if (!oasp_ast_materialize_expression_list(arena, bytes, source->args_offset, source->arg_count, expressions, header->expression_count,
                                                  &destination->args))
            return false;
        if (expression_pointers)
            expression_pointers[i] = destination;
    }

    for (size_t i = 0; i < header->case_count; i++) {
        const oasp_ast_case *source = &case_records[i];
        oasp_case_branch *destination = &cases[i];
        destination->next = oasp_ast_case_pointer(cases, header->case_count, source->next);
        destination->value_count = source->value_count;
        destination->body.first = oasp_ast_statement_pointer(statements, header->statement_count, source->body_first);
        destination->body.last = oasp_ast_statement_pointer(statements, header->statement_count, source->body_last);
        destination->is_else = source->is_else != 0;
        if (!oasp_ast_materialize_expression_list(arena, bytes, source->values_offset, source->value_count, expressions, header->expression_count,
                                                  &destination->values))
            return false;
    }

    for (size_t i = 0; i < header->statement_count; i++) {
        const oasp_ast_statement *source = &statement_records[i];
        oasp_stmt *destination = &statements[i];
        destination->kind = (oasp_stmt_kind)source->kind;
        destination->routine_kind = (oasp_routine_kind)source->routine_kind;
        destination->line = (size_t)source->line;
        destination->next = oasp_ast_statement_pointer(statements, header->statement_count, source->next);
        destination->name = oasp_ast_materialize_slice(bytes, source->name);
        destination->target = oasp_ast_expression_pointer(expressions, header->expression_count, source->target);
        destination->value = oasp_ast_expression_pointer(expressions, header->expression_count, source->value);
        destination->condition = oasp_ast_expression_pointer(expressions, header->expression_count, source->condition);
        destination->limit = oasp_ast_expression_pointer(expressions, header->expression_count, source->limit);
        destination->step = oasp_ast_expression_pointer(expressions, header->expression_count, source->step);
        destination->expression_count = source->expression_count;
        destination->body.first = oasp_ast_statement_pointer(statements, header->statement_count, source->body_first);
        destination->body.last = oasp_ast_statement_pointer(statements, header->statement_count, source->body_last);
        destination->alternative.first = oasp_ast_statement_pointer(statements, header->statement_count, source->alternative_first);
        destination->alternative.last = oasp_ast_statement_pointer(statements, header->statement_count, source->alternative_last);
        destination->cases = oasp_ast_case_pointer(cases, header->case_count, source->first_case);
        destination->is_default = (source->flags & 1u) != 0;
        destination->until = (source->flags & 2u) != 0;
        destination->post_condition = (source->flags & 4u) != 0;
        destination->preserve = (source->flags & 8u) != 0;
        if (!oasp_ast_materialize_expression_list(arena, bytes, source->expressions_offset, source->expression_count, expressions,
                                                  header->expression_count, &destination->expressions))
            return false;

        destination->name_count = source->name_count;
        destination->names = source->name_count ? oasp_arena_alloc(arena, source->name_count * sizeof(*destination->names), _Alignof(oasp_slice)) : NULL;
        if (source->name_count && !destination->names)
            return false;
        const oasp_ast_slice *names = (const void *)(bytes + source->names_offset);
        for (size_t j = 0; j < source->name_count; j++)
            destination->names[j] = oasp_ast_materialize_slice(bytes, names[j]);

        destination->declaration_count = source->declaration_count;
        destination->declarations =
            source->declaration_count
                ? oasp_arena_alloc(arena, source->declaration_count * sizeof(*destination->declarations), _Alignof(oasp_declarator))
                : NULL;
        if (source->declaration_count && !destination->declarations)
            return false;
        const oasp_ast_declarator *declarations = (const void *)(bytes + source->declarations_offset);
        for (size_t j = 0; j < source->declaration_count; j++) {
            destination->declarations[j].name = oasp_ast_materialize_slice(bytes, declarations[j].name);
            destination->declarations[j].dimension_count = declarations[j].dimension_count;
            if (!oasp_ast_materialize_expression_list(arena, bytes, declarations[j].dimensions_offset, declarations[j].dimension_count, expressions,
                                                      header->expression_count, &destination->declarations[j].dimensions))
                return false;
        }

        destination->constant_count = source->constant_count;
        destination->constants =
            source->constant_count ? oasp_arena_alloc(arena, source->constant_count * sizeof(*destination->constants), _Alignof(oasp_const_declarator))
                                   : NULL;
        if (source->constant_count && !destination->constants)
            return false;
        const oasp_ast_constant *constants = (const void *)(bytes + source->constants_offset);
        for (size_t j = 0; j < source->constant_count; j++) {
            destination->constants[j].name = oasp_ast_materialize_slice(bytes, constants[j].name);
            destination->constants[j].value = oasp_ast_expression_pointer(expressions, header->expression_count, constants[j].value);
        }

        destination->parameter_count = source->parameter_count;
        destination->parameters =
            source->parameter_count ? oasp_arena_alloc(arena, source->parameter_count * sizeof(*destination->parameters), _Alignof(oasp_parameter)) : NULL;
        if (source->parameter_count && !destination->parameters)
            return false;
        const oasp_ast_parameter *parameters = (const void *)(bytes + source->parameters_offset);
        for (size_t j = 0; j < source->parameter_count; j++) {
            destination->parameters[j].name = oasp_ast_materialize_slice(bytes, parameters[j].name);
            destination->parameters[j].by_value = (parameters[j].flags & 1u) != 0;
            destination->parameters[j].array = (parameters[j].flags & 2u) != 0;
        }
        if (statement_pointers)
            statement_pointers[i] = destination;
    }
    program->first = oasp_ast_statement_pointer(statements, header->statement_count, header->program_first);
    program->last = oasp_ast_statement_pointer(statements, header->statement_count, header->program_last);
    if (statement_map)
        *statement_map = statement_pointers;
    if (expression_map)
        *expression_map = expression_pointers;
    return true;
}

static bool oasp_ast_tagged(const void *pointer) { return ((uintptr_t)pointer & (uintptr_t)1) != 0; }

static const void *oasp_ast_untag(const void *pointer) { return (const void *)((uintptr_t)pointer & ~(uintptr_t)1); }

static const void *oasp_ast_tag(const void *pointer) { return (const void *)((uintptr_t)pointer | (uintptr_t)1); }

static const oasp_ast_header *oasp_ast_expression_header(const oasp_expr *expression, const oasp_ast_expression **record) {
    *record = oasp_ast_untag(expression);
    return (const void *)((const char *)*record - (*record)->self_offset);
}

static const oasp_ast_header *oasp_ast_statement_header(const oasp_stmt *statement, const oasp_ast_statement **record) {
    *record = oasp_ast_untag(statement);
    return (const void *)((const char *)*record - (*record)->self_offset);
}

static const oasp_ast_header *oasp_ast_case_header(const oasp_case_branch *branch, const oasp_ast_case **record) {
    *record = oasp_ast_untag(branch);
    return (const void *)((const char *)*record - (*record)->self_offset);
}

static const oasp_expr *oasp_ast_expression_ref(const oasp_ast_header *header, uint32_t index) {
    if (index == OASP_AST_NONE)
        return NULL;
    const oasp_ast_expression *records = (const void *)((const char *)header + header->expression_offset);
    return oasp_ast_tag(&records[index]);
}

static const oasp_stmt *oasp_ast_statement_ref(const oasp_ast_header *header, uint32_t index) {
    if (index == OASP_AST_NONE)
        return NULL;
    const oasp_ast_statement *records = (const void *)((const char *)header + header->statement_offset);
    return oasp_ast_tag(&records[index]);
}

static const oasp_case_branch *oasp_ast_case_ref(const oasp_ast_header *header, uint32_t index) {
    if (index == OASP_AST_NONE)
        return NULL;
    const oasp_ast_case *records = (const void *)((const char *)header + header->case_offset);
    return oasp_ast_tag(&records[index]);
}

static oasp_slice oasp_ast_image_slice(const oasp_ast_header *header, oasp_ast_slice slice) {
    oasp_slice result = {(const char *)header + slice.offset, slice.length};
    return result;
}

bool oasp_ast_image_program(const oasp_ast_image *image, oasp_block *program) {
    if (!image || !program || !oasp_ast_image_open(image->data, image->size, image->statement_count, image->expression_count, &(oasp_ast_image){0}))
        return false;
    const oasp_ast_header *header = image->data;
    program->first = (oasp_stmt *)oasp_ast_statement_ref(header, header->program_first);
    program->last = (oasp_stmt *)oasp_ast_statement_ref(header, header->program_last);
    return true;
}

const oasp_expr *oasp_ast_image_expression(const oasp_ast_image *image, size_t index) {
    if (!image || index >= image->expression_count)
        return NULL;
    return oasp_ast_expression_ref(image->data, (uint32_t)index);
}

const oasp_stmt *oasp_ast_image_statement(const oasp_ast_image *image, size_t index) {
    if (!image || index >= image->statement_count)
        return NULL;
    return oasp_ast_statement_ref(image->data, (uint32_t)index);
}

bool oasp_ast_is_expression(const oasp_expr *expression) { return expression && oasp_ast_tagged(expression); }

bool oasp_ast_is_statement(const oasp_stmt *statement) { return statement && oasp_ast_tagged(statement); }

oasp_expr_kind oasp_ast_expression_kind(const oasp_expr *expression) {
    if (!oasp_ast_is_expression(expression))
        return expression ? expression->kind : OASP_EXPR_EMPTY;
    const oasp_ast_expression *record;
    (void)oasp_ast_expression_header(expression, &record);
    return (oasp_expr_kind)record->kind;
}

size_t oasp_ast_expression_line(const oasp_expr *expression) {
    if (!oasp_ast_is_expression(expression))
        return expression ? expression->line : 0;
    const oasp_ast_expression *record;
    (void)oasp_ast_expression_header(expression, &record);
    return (size_t)record->line;
}

oasp_slice oasp_ast_expression_name(const oasp_expr *expression) {
    if (!oasp_ast_is_expression(expression))
        return expression ? expression->name : (oasp_slice){0};
    const oasp_ast_expression *record;
    const oasp_ast_header *header = oasp_ast_expression_header(expression, &record);
    return oasp_ast_image_slice(header, record->name);
}

oasp_value oasp_ast_expression_literal(const oasp_expr *expression) {
    if (!oasp_ast_is_expression(expression))
        return expression ? expression->literal : oasp_value_empty();
    const oasp_ast_expression *record;
    const oasp_ast_header *header = oasp_ast_expression_header(expression, &record);
    oasp_value value = oasp_value_empty();
    value.kind = (oasp_value_kind)record->literal_kind;
    if (value.kind == OASP_VALUE_BOOL)
        value.as.boolean = record->literal_bits != 0;
    else if (value.kind == OASP_VALUE_INT)
        memcpy(&value.as.integer, &record->literal_bits, sizeof(value.as.integer));
    else if (value.kind == OASP_VALUE_FLOAT)
        memcpy(&value.as.floating, &record->literal_bits, sizeof(value.as.floating));
    else if (value.kind == OASP_VALUE_STRING)
        value.as.string = oasp_ast_image_slice(header, record->literal_slice);
    else if (value.kind == OASP_VALUE_DATE)
        value.as.date = oasp_ast_image_slice(header, record->literal_slice);
    return value;
}

const oasp_expr *oasp_ast_expression_left(const oasp_expr *expression) {
    if (!oasp_ast_is_expression(expression))
        return expression ? expression->left : NULL;
    const oasp_ast_expression *record;
    const oasp_ast_header *header = oasp_ast_expression_header(expression, &record);
    return oasp_ast_expression_ref(header, record->left);
}

const oasp_expr *oasp_ast_expression_right(const oasp_expr *expression) {
    if (!oasp_ast_is_expression(expression))
        return expression ? expression->right : NULL;
    const oasp_ast_expression *record;
    const oasp_ast_header *header = oasp_ast_expression_header(expression, &record);
    return oasp_ast_expression_ref(header, record->right);
}

size_t oasp_ast_expression_arg_count(const oasp_expr *expression) {
    if (!oasp_ast_is_expression(expression))
        return expression ? expression->arg_count : 0;
    const oasp_ast_expression *record;
    (void)oasp_ast_expression_header(expression, &record);
    return record->arg_count;
}

const oasp_expr *oasp_ast_expression_arg(const oasp_expr *expression, size_t index) {
    if (!oasp_ast_is_expression(expression))
        return expression && index < expression->arg_count ? expression->args[index] : NULL;
    const oasp_ast_expression *record;
    const oasp_ast_header *header = oasp_ast_expression_header(expression, &record);
    if (index >= record->arg_count)
        return NULL;
    const uint32_t *indices = (const void *)((const char *)header + record->args_offset);
    return oasp_ast_expression_ref(header, indices[index]);
}

oasp_expr **oasp_ast_expression_arguments(oasp_arena *arena, const oasp_expr *expression) {
    if (!oasp_ast_is_expression(expression))
        return expression ? expression->args : NULL;
    size_t count = oasp_ast_expression_arg_count(expression);
    oasp_expr **arguments = count ? oasp_arena_alloc(arena, count * sizeof(*arguments), _Alignof(oasp_expr *)) : NULL;
    if (count && !arguments)
        return NULL;
    for (size_t i = 0; i < count; i++)
        arguments[i] = (oasp_expr *)oasp_ast_expression_arg(expression, i);
    return arguments;
}

oasp_stmt_kind oasp_ast_statement_kind(const oasp_stmt *statement) {
    if (!oasp_ast_is_statement(statement))
        return statement ? statement->kind : OASP_STMT_NOOP;
    const oasp_ast_statement *record;
    (void)oasp_ast_statement_header(statement, &record);
    return (oasp_stmt_kind)record->kind;
}

size_t oasp_ast_statement_line(const oasp_stmt *statement) {
    if (!oasp_ast_is_statement(statement))
        return statement ? statement->line : 0;
    const oasp_ast_statement *record;
    (void)oasp_ast_statement_header(statement, &record);
    return (size_t)record->line;
}

const oasp_stmt *oasp_ast_statement_next(const oasp_stmt *statement) {
    if (!oasp_ast_is_statement(statement))
        return statement ? statement->next : NULL;
    const oasp_ast_statement *record;
    const oasp_ast_header *header = oasp_ast_statement_header(statement, &record);
    return oasp_ast_statement_ref(header, record->next);
}

oasp_slice oasp_ast_statement_name(const oasp_stmt *statement) {
    if (!oasp_ast_is_statement(statement))
        return statement ? statement->name : (oasp_slice){0};
    const oasp_ast_statement *record;
    const oasp_ast_header *header = oasp_ast_statement_header(statement, &record);
    return oasp_ast_image_slice(header, record->name);
}

#define OASP_AST_STATEMENT_EXPRESSION_ACCESSOR(function_name, field_name)                                                             \
    const oasp_expr *function_name(const oasp_stmt *statement) {                                                                      \
        if (!oasp_ast_is_statement(statement))                                                                                        \
            return statement ? statement->field_name : NULL;                                                                         \
        const oasp_ast_statement *record;                                                                                             \
        const oasp_ast_header *header = oasp_ast_statement_header(statement, &record);                                                \
        return oasp_ast_expression_ref(header, record->field_name);                                                                   \
    }

OASP_AST_STATEMENT_EXPRESSION_ACCESSOR(oasp_ast_statement_target, target)
OASP_AST_STATEMENT_EXPRESSION_ACCESSOR(oasp_ast_statement_value, value)
OASP_AST_STATEMENT_EXPRESSION_ACCESSOR(oasp_ast_statement_condition, condition)
OASP_AST_STATEMENT_EXPRESSION_ACCESSOR(oasp_ast_statement_limit, limit)
OASP_AST_STATEMENT_EXPRESSION_ACCESSOR(oasp_ast_statement_step, step)

#define OASP_AST_STATEMENT_REFERENCE_ACCESSOR(function_name, native_block, field_name)                                                \
    const oasp_stmt *function_name(const oasp_stmt *statement) {                                                                      \
        if (!oasp_ast_is_statement(statement))                                                                                        \
            return statement ? statement->native_block.field_name : NULL;                                                            \
        const oasp_ast_statement *record;                                                                                             \
        const oasp_ast_header *header = oasp_ast_statement_header(statement, &record);                                                \
        return oasp_ast_statement_ref(header, record->native_block##_##field_name);                                                    \
    }

OASP_AST_STATEMENT_REFERENCE_ACCESSOR(oasp_ast_statement_body_first, body, first)
OASP_AST_STATEMENT_REFERENCE_ACCESSOR(oasp_ast_statement_body_last, body, last)
OASP_AST_STATEMENT_REFERENCE_ACCESSOR(oasp_ast_statement_alternative_first, alternative, first)
OASP_AST_STATEMENT_REFERENCE_ACCESSOR(oasp_ast_statement_alternative_last, alternative, last)

const oasp_case_branch *oasp_ast_statement_cases(const oasp_stmt *statement) {
    if (!oasp_ast_is_statement(statement))
        return statement ? statement->cases : NULL;
    const oasp_ast_statement *record;
    const oasp_ast_header *header = oasp_ast_statement_header(statement, &record);
    return oasp_ast_case_ref(header, record->first_case);
}

size_t oasp_ast_statement_expression_count(const oasp_stmt *statement) {
    if (!oasp_ast_is_statement(statement))
        return statement ? statement->expression_count : 0;
    const oasp_ast_statement *record;
    (void)oasp_ast_statement_header(statement, &record);
    return record->expression_count;
}

const oasp_expr *oasp_ast_statement_expression(const oasp_stmt *statement, size_t index) {
    if (!oasp_ast_is_statement(statement))
        return statement && index < statement->expression_count ? statement->expressions[index] : NULL;
    const oasp_ast_statement *record;
    const oasp_ast_header *header = oasp_ast_statement_header(statement, &record);
    if (index >= record->expression_count)
        return NULL;
    const uint32_t *indices = (const void *)((const char *)header + record->expressions_offset);
    return oasp_ast_expression_ref(header, indices[index]);
}

size_t oasp_ast_statement_name_count(const oasp_stmt *statement) {
    if (!oasp_ast_is_statement(statement))
        return statement ? statement->name_count : 0;
    const oasp_ast_statement *record;
    (void)oasp_ast_statement_header(statement, &record);
    return record->name_count;
}

oasp_slice oasp_ast_statement_name_at(const oasp_stmt *statement, size_t index) {
    if (!oasp_ast_is_statement(statement))
        return statement && index < statement->name_count ? statement->names[index] : (oasp_slice){0};
    const oasp_ast_statement *record;
    const oasp_ast_header *header = oasp_ast_statement_header(statement, &record);
    if (index >= record->name_count)
        return (oasp_slice){0};
    const oasp_ast_slice *items = (const void *)((const char *)header + record->names_offset);
    return oasp_ast_image_slice(header, items[index]);
}

size_t oasp_ast_statement_declaration_count(const oasp_stmt *statement) {
    if (!oasp_ast_is_statement(statement))
        return statement ? statement->declaration_count : 0;
    const oasp_ast_statement *record;
    (void)oasp_ast_statement_header(statement, &record);
    return record->declaration_count;
}

static const oasp_ast_declarator *oasp_ast_statement_declaration_record(const oasp_stmt *statement, size_t index, const oasp_ast_header **header) {
    const oasp_ast_statement *record;
    *header = oasp_ast_statement_header(statement, &record);
    if (index >= record->declaration_count)
        return NULL;
    const oasp_ast_declarator *items = (const void *)((const char *)*header + record->declarations_offset);
    return &items[index];
}

oasp_slice oasp_ast_statement_declaration_name(const oasp_stmt *statement, size_t index) {
    if (!oasp_ast_is_statement(statement))
        return statement && index < statement->declaration_count ? statement->declarations[index].name : (oasp_slice){0};
    const oasp_ast_header *header;
    const oasp_ast_declarator *record = oasp_ast_statement_declaration_record(statement, index, &header);
    return record ? oasp_ast_image_slice(header, record->name) : (oasp_slice){0};
}

size_t oasp_ast_statement_declaration_dimension_count(const oasp_stmt *statement, size_t index) {
    if (!oasp_ast_is_statement(statement))
        return statement && index < statement->declaration_count ? statement->declarations[index].dimension_count : 0;
    const oasp_ast_header *header;
    const oasp_ast_declarator *record = oasp_ast_statement_declaration_record(statement, index, &header);
    return record ? record->dimension_count : 0;
}

const oasp_expr *oasp_ast_statement_declaration_dimension(const oasp_stmt *statement, size_t declaration, size_t dimension) {
    if (!oasp_ast_is_statement(statement)) {
        if (!statement || declaration >= statement->declaration_count || dimension >= statement->declarations[declaration].dimension_count)
            return NULL;
        return statement->declarations[declaration].dimensions[dimension];
    }
    const oasp_ast_header *header;
    const oasp_ast_declarator *record = oasp_ast_statement_declaration_record(statement, declaration, &header);
    if (!record || dimension >= record->dimension_count)
        return NULL;
    const uint32_t *indices = (const void *)((const char *)header + record->dimensions_offset);
    return oasp_ast_expression_ref(header, indices[dimension]);
}

size_t oasp_ast_statement_constant_count(const oasp_stmt *statement) {
    if (!oasp_ast_is_statement(statement))
        return statement ? statement->constant_count : 0;
    const oasp_ast_statement *record;
    (void)oasp_ast_statement_header(statement, &record);
    return record->constant_count;
}

static const oasp_ast_constant *oasp_ast_statement_constant_record(const oasp_stmt *statement, size_t index, const oasp_ast_header **header) {
    const oasp_ast_statement *record;
    *header = oasp_ast_statement_header(statement, &record);
    if (index >= record->constant_count)
        return NULL;
    const oasp_ast_constant *items = (const void *)((const char *)*header + record->constants_offset);
    return &items[index];
}

oasp_slice oasp_ast_statement_constant_name(const oasp_stmt *statement, size_t index) {
    if (!oasp_ast_is_statement(statement))
        return statement && index < statement->constant_count ? statement->constants[index].name : (oasp_slice){0};
    const oasp_ast_header *header;
    const oasp_ast_constant *record = oasp_ast_statement_constant_record(statement, index, &header);
    return record ? oasp_ast_image_slice(header, record->name) : (oasp_slice){0};
}

const oasp_expr *oasp_ast_statement_constant_value(const oasp_stmt *statement, size_t index) {
    if (!oasp_ast_is_statement(statement))
        return statement && index < statement->constant_count ? statement->constants[index].value : NULL;
    const oasp_ast_header *header;
    const oasp_ast_constant *record = oasp_ast_statement_constant_record(statement, index, &header);
    return record ? oasp_ast_expression_ref(header, record->value) : NULL;
}

size_t oasp_ast_statement_parameter_count(const oasp_stmt *statement) {
    if (!oasp_ast_is_statement(statement))
        return statement ? statement->parameter_count : 0;
    const oasp_ast_statement *record;
    (void)oasp_ast_statement_header(statement, &record);
    return record->parameter_count;
}

static const oasp_ast_parameter *oasp_ast_statement_parameter_record(const oasp_stmt *statement, size_t index, const oasp_ast_header **header) {
    const oasp_ast_statement *record;
    *header = oasp_ast_statement_header(statement, &record);
    if (index >= record->parameter_count)
        return NULL;
    const oasp_ast_parameter *items = (const void *)((const char *)*header + record->parameters_offset);
    return &items[index];
}

oasp_slice oasp_ast_statement_parameter_name(const oasp_stmt *statement, size_t index) {
    if (!oasp_ast_is_statement(statement))
        return statement && index < statement->parameter_count ? statement->parameters[index].name : (oasp_slice){0};
    const oasp_ast_header *header;
    const oasp_ast_parameter *record = oasp_ast_statement_parameter_record(statement, index, &header);
    return record ? oasp_ast_image_slice(header, record->name) : (oasp_slice){0};
}

bool oasp_ast_statement_parameter_by_value(const oasp_stmt *statement, size_t index) {
    if (!oasp_ast_is_statement(statement))
        return statement && index < statement->parameter_count && statement->parameters[index].by_value;
    const oasp_ast_header *header;
    const oasp_ast_parameter *record = oasp_ast_statement_parameter_record(statement, index, &header);
    return record && (record->flags & 1u);
}

bool oasp_ast_statement_parameter_array(const oasp_stmt *statement, size_t index) {
    if (!oasp_ast_is_statement(statement))
        return statement && index < statement->parameter_count && statement->parameters[index].array;
    const oasp_ast_header *header;
    const oasp_ast_parameter *record = oasp_ast_statement_parameter_record(statement, index, &header);
    return record && (record->flags & 2u);
}

oasp_routine_kind oasp_ast_statement_routine_kind(const oasp_stmt *statement) {
    if (!oasp_ast_is_statement(statement))
        return statement ? statement->routine_kind : OASP_ROUTINE_SUB;
    const oasp_ast_statement *record;
    (void)oasp_ast_statement_header(statement, &record);
    return (oasp_routine_kind)record->routine_kind;
}

static bool oasp_ast_statement_flag(const oasp_stmt *statement, uint32_t flag) {
    const oasp_ast_statement *record;
    (void)oasp_ast_statement_header(statement, &record);
    return (record->flags & flag) != 0;
}

bool oasp_ast_statement_is_default(const oasp_stmt *statement) {
    return oasp_ast_is_statement(statement) ? oasp_ast_statement_flag(statement, 1u) : statement && statement->is_default;
}

bool oasp_ast_statement_until(const oasp_stmt *statement) {
    return oasp_ast_is_statement(statement) ? oasp_ast_statement_flag(statement, 2u) : statement && statement->until;
}

bool oasp_ast_statement_post_condition(const oasp_stmt *statement) {
    return oasp_ast_is_statement(statement) ? oasp_ast_statement_flag(statement, 4u) : statement && statement->post_condition;
}

bool oasp_ast_statement_preserve(const oasp_stmt *statement) {
    return oasp_ast_is_statement(statement) ? oasp_ast_statement_flag(statement, 8u) : statement && statement->preserve;
}

const oasp_case_branch *oasp_ast_case_next(const oasp_case_branch *branch) {
    if (!branch || !oasp_ast_tagged(branch))
        return branch ? branch->next : NULL;
    const oasp_ast_case *record;
    const oasp_ast_header *header = oasp_ast_case_header(branch, &record);
    return oasp_ast_case_ref(header, record->next);
}

size_t oasp_ast_case_value_count(const oasp_case_branch *branch) {
    if (!branch || !oasp_ast_tagged(branch))
        return branch ? branch->value_count : 0;
    const oasp_ast_case *record;
    (void)oasp_ast_case_header(branch, &record);
    return record->value_count;
}

const oasp_expr *oasp_ast_case_value(const oasp_case_branch *branch, size_t index) {
    if (!branch || !oasp_ast_tagged(branch))
        return branch && index < branch->value_count ? branch->values[index] : NULL;
    const oasp_ast_case *record;
    const oasp_ast_header *header = oasp_ast_case_header(branch, &record);
    if (index >= record->value_count)
        return NULL;
    const uint32_t *indices = (const void *)((const char *)header + record->values_offset);
    return oasp_ast_expression_ref(header, indices[index]);
}

const oasp_stmt *oasp_ast_case_body_first(const oasp_case_branch *branch) {
    if (!branch || !oasp_ast_tagged(branch))
        return branch ? branch->body.first : NULL;
    const oasp_ast_case *record;
    const oasp_ast_header *header = oasp_ast_case_header(branch, &record);
    return oasp_ast_statement_ref(header, record->body_first);
}

const oasp_stmt *oasp_ast_case_body_last(const oasp_case_branch *branch) {
    if (!branch || !oasp_ast_tagged(branch))
        return branch ? branch->body.last : NULL;
    const oasp_ast_case *record;
    const oasp_ast_header *header = oasp_ast_case_header(branch, &record);
    return oasp_ast_statement_ref(header, record->body_last);
}

bool oasp_ast_case_is_else(const oasp_case_branch *branch) {
    if (!branch || !oasp_ast_tagged(branch))
        return branch && branch->is_else;
    const oasp_ast_case *record;
    (void)oasp_ast_case_header(branch, &record);
    return record->is_else != 0;
}
