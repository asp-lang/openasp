// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Recursive-descent parser data model. Nodes and child arrays are allocated in
 * the caller's arena, while slices borrow source or lexer-owned text. The
 * explicit counts beside every pointer array are part of the safety contract
 * used later by AST serialization and bytecode compilation.
 */

#ifndef OPENASP_PARSER_H
#define OPENASP_PARSER_H

#include "lexer.h"
#include "value.h"

typedef enum {
    OASP_EXPR_EMPTY,
    OASP_EXPR_LITERAL,
    OASP_EXPR_IDENTIFIER,
    OASP_EXPR_MEMBER,
    OASP_EXPR_CALL,
    OASP_EXPR_NEW,
    OASP_EXPR_NEGATE,
    OASP_EXPR_POSITIVE,
    OASP_EXPR_NOT,
    OASP_EXPR_CONCAT,
    OASP_EXPR_EQ,
    OASP_EXPR_NE,
    OASP_EXPR_LT,
    OASP_EXPR_LE,
    OASP_EXPR_GT,
    OASP_EXPR_GE,
    OASP_EXPR_ADD,
    OASP_EXPR_SUBTRACT,
    OASP_EXPR_MULTIPLY,
    OASP_EXPR_DIVIDE,
    OASP_EXPR_INT_DIVIDE,
    OASP_EXPR_MOD,
    OASP_EXPR_POWER,
    OASP_EXPR_AND,
    OASP_EXPR_OR,
    OASP_EXPR_XOR,
    OASP_EXPR_EQV,
    OASP_EXPR_IMP,
    OASP_EXPR_IS,
    OASP_EXPR_IS_NOT
} oasp_expr_kind;

typedef struct oasp_expr oasp_expr;

struct oasp_expr {
    /* Operator/node discriminator and source line for diagnostics. */
    oasp_expr_kind kind;
    size_t line;
    /* Identifier/member name or literal payload, according to `kind`. */
    oasp_slice name;
    oasp_value literal;
    /* Unary/binary operands and call arguments; all are arena-owned. */
    oasp_expr *left;
    oasp_expr *right;
    oasp_expr **args;
    size_t arg_count;
};

typedef enum {
    OASP_STMT_NOOP,
    OASP_STMT_OPTION_EXPLICIT,
    OASP_STMT_ON_ERROR_RESUME_NEXT,
    OASP_STMT_ON_ERROR_GOTO_0,
    OASP_STMT_DIM,
    OASP_STMT_CONST,
    OASP_STMT_REDIM,
    OASP_STMT_ERASE,
    OASP_STMT_ASSIGN,
    OASP_STMT_SET_ASSIGN,
    OASP_STMT_CALL,
    OASP_STMT_IF,
    OASP_STMT_WHILE,
    OASP_STMT_DO,
    OASP_STMT_FOR,
    OASP_STMT_FOR_EACH,
    OASP_STMT_SELECT,
    OASP_STMT_EXIT,
    OASP_STMT_ROUTINE,
    OASP_STMT_CLASS,
    OASP_STMT_WITH
} oasp_stmt_kind;

typedef struct oasp_stmt oasp_stmt;

typedef struct {
    /* Intrusive statement list; `last` permits constant-time append. */
    oasp_stmt *first;
    oasp_stmt *last;
} oasp_block;

typedef struct oasp_case_branch {
    struct oasp_case_branch *next;
    oasp_expr **values;
    size_t value_count;
    oasp_block body;
    bool is_else;
} oasp_case_branch;

typedef struct {
    oasp_slice name;
    oasp_expr **dimensions;
    size_t dimension_count;
} oasp_declarator;

typedef struct {
    oasp_slice name;
    oasp_expr *value;
} oasp_const_declarator;

typedef struct {
    oasp_slice name;
    bool by_value;
    bool array;
} oasp_parameter;

typedef enum { OASP_ROUTINE_SUB, OASP_ROUTINE_FUNCTION, OASP_ROUTINE_PROPERTY_GET, OASP_ROUTINE_PROPERTY_LET, OASP_ROUTINE_PROPERTY_SET } oasp_routine_kind;

struct oasp_stmt {
    /* Common statement identity and source-order linkage. */
    oasp_stmt_kind kind;
    size_t line;
    oasp_stmt *next;
    oasp_slice name;
    /* Kind-specific scalar expressions and nested control-flow blocks. */
    oasp_expr *target;
    oasp_expr *value;
    oasp_expr *condition;
    oasp_expr *limit;
    oasp_expr *step;
    oasp_expr **expressions;
    size_t expression_count;
    oasp_block body;
    oasp_block alternative;
    oasp_case_branch *cases;
    /* Kind-specific variable-length declarations, constants, and parameters. */
    oasp_slice *names;
    size_t name_count;
    oasp_declarator *declarations;
    size_t declaration_count;
    oasp_const_declarator *constants;
    size_t constant_count;
    oasp_parameter *parameters;
    size_t parameter_count;
    /* Flags disambiguate routine and loop variants without extra node types. */
    oasp_routine_kind routine_kind;
    bool is_default;
    bool until;
    bool post_condition;
    bool preserve;
};

typedef struct {
    /* Borrowed arena, owned token vector, and current lookahead position. */
    oasp_arena *arena;
    oasp_token_list tokens;
    size_t cursor;
    /* First deterministic diagnostic; later parse failures preserve it. */
    const char *error;
    size_t error_line;
    size_t error_column;
} oasp_parser;

/*
 * Lexes `source[0..length)` and initializes parser lookahead. Source and arena
 * must outlive all parsed nodes; dispose releases only the temporary token
 * vector, not arena-owned AST data.
 */
bool oasp_parser_init(oasp_parser *parser, oasp_arena *arena, const char *source, size_t length);
void oasp_parser_dispose(oasp_parser *parser);

/*
 * Parses either one expression or an entire program from the current cursor.
 * NULL/false means `error`, `error_line`, and `error_column` describe the first
 * syntax failure. `program` receives an arena-owned source-order statement list.
 */
oasp_expr *oasp_parse_expression(oasp_parser *parser);
bool oasp_parse_program(oasp_parser *parser, oasp_block *program);

/* Returns borrowed lookahead; EOF remains represented by a real token. */
const oasp_token *oasp_parser_current(const oasp_parser *parser);

#endif
