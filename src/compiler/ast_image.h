// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Portable AST image interface. Encoding replaces process-local pointers with
 * checked indexes; opening validates the immutable byte range; materializing
 * rebuilds arena-owned pointer graphs when mutable parser structures are
 * required. All returned views borrow `data`, so the encoded bytes must outlive
 * the image and every node obtained from it.
 */

#ifndef OPENASP_AST_IMAGE_H
#define OPENASP_AST_IMAGE_H

#include "parser.h"

typedef struct oasp_ast_image {
    /* Borrowed encoded bytes and their exact validated extent. */
    const void *data;
    size_t size;
    /* Validated table lengths used to reject every out-of-range node index. */
    size_t statement_count;
    size_t expression_count;
} oasp_ast_image;

/*
 * Serializes `program` into `output` and reports the two table counts needed
 * when reopening it. `output` owns its heap buffer and remains unchanged only
 * where the buffer helper can preserve it on allocation failure.
 */
bool oasp_ast_image_encode(const oasp_block *program, oasp_buffer *output, size_t *statement_count, size_t *expression_count);

/*
 * Validates `data[0..size)` against the caller-supplied table counts and writes
 * a borrowed immutable image to `image`. No AST allocation occurs here.
 */
bool oasp_ast_image_open(const void *data, size_t size, size_t statement_count, size_t expression_count, oasp_ast_image *image);

/*
 * Reconstructs an arena-owned AST and optional index tables. `statements` and
 * `expressions` may be NULL when the caller needs only `program`.
 */
bool oasp_ast_image_materialize(const oasp_ast_image *image, oasp_arena *arena, oasp_block *program, const oasp_stmt ***statements, const oasp_expr ***expressions);

/* Decodes only the root block as an immutable image-backed statement chain. */
bool oasp_ast_image_program(const oasp_ast_image *image, oasp_block *program);

/*
 * The accessors below decode one validated field without exposing the binary
 * layout. Invalid object kinds or indexes return NULL, zero, false, or the
 * corresponding empty enum/value. Slice results borrow the image bytes.
 */
const oasp_expr *oasp_ast_image_expression(const oasp_ast_image *image, size_t index);
const oasp_stmt *oasp_ast_image_statement(const oasp_ast_image *image, size_t index);
bool oasp_ast_is_expression(const oasp_expr *expression);
bool oasp_ast_is_statement(const oasp_stmt *statement);
oasp_expr_kind oasp_ast_expression_kind(const oasp_expr *expression);
size_t oasp_ast_expression_line(const oasp_expr *expression);
oasp_slice oasp_ast_expression_name(const oasp_expr *expression);
oasp_value oasp_ast_expression_literal(const oasp_expr *expression);
const oasp_expr *oasp_ast_expression_left(const oasp_expr *expression);
const oasp_expr *oasp_ast_expression_right(const oasp_expr *expression);
size_t oasp_ast_expression_arg_count(const oasp_expr *expression);
const oasp_expr *oasp_ast_expression_arg(const oasp_expr *expression, size_t index);
oasp_expr **oasp_ast_expression_arguments(oasp_arena *arena, const oasp_expr *expression);
oasp_stmt_kind oasp_ast_statement_kind(const oasp_stmt *statement);
size_t oasp_ast_statement_line(const oasp_stmt *statement);
const oasp_stmt *oasp_ast_statement_next(const oasp_stmt *statement);
oasp_slice oasp_ast_statement_name(const oasp_stmt *statement);
const oasp_expr *oasp_ast_statement_target(const oasp_stmt *statement);
const oasp_expr *oasp_ast_statement_value(const oasp_stmt *statement);
const oasp_expr *oasp_ast_statement_condition(const oasp_stmt *statement);
const oasp_expr *oasp_ast_statement_limit(const oasp_stmt *statement);
const oasp_expr *oasp_ast_statement_step(const oasp_stmt *statement);
const oasp_stmt *oasp_ast_statement_body_first(const oasp_stmt *statement);
const oasp_stmt *oasp_ast_statement_body_last(const oasp_stmt *statement);
const oasp_stmt *oasp_ast_statement_alternative_first(const oasp_stmt *statement);
const oasp_stmt *oasp_ast_statement_alternative_last(const oasp_stmt *statement);
const oasp_case_branch *oasp_ast_statement_cases(const oasp_stmt *statement);

/* Indexed collection accessors validate `index` before decoding a child. */
size_t oasp_ast_statement_expression_count(const oasp_stmt *statement);
const oasp_expr *oasp_ast_statement_expression(const oasp_stmt *statement, size_t index);
size_t oasp_ast_statement_name_count(const oasp_stmt *statement);
oasp_slice oasp_ast_statement_name_at(const oasp_stmt *statement, size_t index);
size_t oasp_ast_statement_declaration_count(const oasp_stmt *statement);
oasp_slice oasp_ast_statement_declaration_name(const oasp_stmt *statement, size_t index);
size_t oasp_ast_statement_declaration_dimension_count(const oasp_stmt *statement, size_t index);
const oasp_expr *oasp_ast_statement_declaration_dimension(const oasp_stmt *statement, size_t declaration, size_t dimension);
size_t oasp_ast_statement_constant_count(const oasp_stmt *statement);
oasp_slice oasp_ast_statement_constant_name(const oasp_stmt *statement, size_t index);
const oasp_expr *oasp_ast_statement_constant_value(const oasp_stmt *statement, size_t index);
size_t oasp_ast_statement_parameter_count(const oasp_stmt *statement);
oasp_slice oasp_ast_statement_parameter_name(const oasp_stmt *statement, size_t index);
bool oasp_ast_statement_parameter_by_value(const oasp_stmt *statement, size_t index);
bool oasp_ast_statement_parameter_array(const oasp_stmt *statement, size_t index);
oasp_routine_kind oasp_ast_statement_routine_kind(const oasp_stmt *statement);
bool oasp_ast_statement_is_default(const oasp_stmt *statement);
bool oasp_ast_statement_until(const oasp_stmt *statement);
bool oasp_ast_statement_post_condition(const oasp_stmt *statement);
bool oasp_ast_statement_preserve(const oasp_stmt *statement);

/* Case-branch views borrow the same image and preserve encoded source order. */
const oasp_case_branch *oasp_ast_case_next(const oasp_case_branch *branch);
size_t oasp_ast_case_value_count(const oasp_case_branch *branch);
const oasp_expr *oasp_ast_case_value(const oasp_case_branch *branch, size_t index);
const oasp_stmt *oasp_ast_case_body_first(const oasp_case_branch *branch);
const oasp_stmt *oasp_ast_case_body_last(const oasp_case_branch *branch);
bool oasp_ast_case_is_else(const oasp_case_branch *branch);

#endif
