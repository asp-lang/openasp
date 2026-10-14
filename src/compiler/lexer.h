// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/compiler/lexer.h`): Compiler pipeline. Arena-owned tokens, AST nodes,
 * symbols, bytecode, and immutable images move through this stage without per-node frees.
 * Length/count variables bound every indexed operation and malformed input must fail before
 * execution.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#ifndef OPENASP_LEXER_H
#define OPENASP_LEXER_H

#include "internal.h"

typedef enum {
    OASP_TOKEN_EOF,
    OASP_TOKEN_IDENTIFIER,
    OASP_TOKEN_STRING,
    OASP_TOKEN_INTEGER,
    OASP_TOKEN_LPAREN,
    OASP_TOKEN_RPAREN,
    OASP_TOKEN_COMMA,
    OASP_TOKEN_EQ,
    OASP_TOKEN_AMP,
    OASP_TOKEN_COLON,
    OASP_TOKEN_NEWLINE,
    OASP_TOKEN_PLUS,
    OASP_TOKEN_MINUS,
    OASP_TOKEN_STAR,
    OASP_TOKEN_SLASH,
    OASP_TOKEN_BACKSLASH,
    OASP_TOKEN_LESS,
    OASP_TOKEN_GREATER,
    OASP_TOKEN_LE,
    OASP_TOKEN_GE,
    OASP_TOKEN_NE,
    OASP_TOKEN_DOT,
    OASP_TOKEN_FLOAT,
    OASP_TOKEN_CARET,
    OASP_TOKEN_DATE
} oasp_token_kind;

typedef struct {
    oasp_token_kind kind;
    oasp_slice text;
    size_t line;
    size_t column;
    size_t source_width;
} oasp_token;

typedef struct {
    oasp_token *items;
    size_t length;
    size_t capacity;
    const char *error;
    size_t error_line;
    size_t error_column;
} oasp_token_list;

/* Non-decoded token text references source, which must outlive the resulting AST. */
bool oasp_lex(oasp_arena *arena, const char *source, size_t length, oasp_token_list *tokens);
void oasp_token_list_dispose(oasp_token_list *tokens);
const char *oasp_token_kind_name(oasp_token_kind kind);

#endif
