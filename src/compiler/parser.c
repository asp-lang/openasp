// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/compiler/parser.c`): Compiler pipeline. Arena-owned tokens, AST nodes,
 * symbols, bytecode, and immutable images move through this stage without per-node frees.
 * Length/count variables bound every indexed operation and malformed input must fail before
 * execution.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#include "parser.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static oasp_expr *oasp_new_expr(oasp_parser *parser, oasp_expr_kind kind) {
    oasp_expr *expr = oasp_arena_alloc(parser->arena, sizeof(*expr), _Alignof(oasp_expr));
    if (!expr) {
        parser->error = "out of memory while parsing expression";
        return NULL;
    }
    memset(expr, 0, sizeof(*expr));
    expr->kind = kind;
    expr->line = oasp_parser_current(parser)->line;
    expr->literal = oasp_value_empty();
    return expr;
}

static oasp_stmt *oasp_new_stmt(oasp_parser *parser, oasp_stmt_kind kind) {
    oasp_stmt *stmt = oasp_arena_alloc(parser->arena, sizeof(*stmt), _Alignof(oasp_stmt));
    if (!stmt) {
        parser->error = "out of memory while parsing statement";
        return NULL;
    }
    memset(stmt, 0, sizeof(*stmt));
    stmt->kind = kind;
    stmt->line = oasp_parser_current(parser)->line;
    return stmt;
}

const oasp_token *oasp_parser_current(const oasp_parser *parser) {
    return &parser->tokens.items[parser->cursor < parser->tokens.length ? parser->cursor : parser->tokens.length - 1];
}

static const oasp_token *oasp_previous(const oasp_parser *parser) { return &parser->tokens.items[parser->cursor ? parser->cursor - 1 : 0]; }

static bool oasp_at_kind(const oasp_parser *parser, oasp_token_kind kind) { return oasp_parser_current(parser)->kind == kind; }

static bool oasp_at_word(const oasp_parser *parser, const char *word) {
    const oasp_token *token = oasp_parser_current(parser);
    return token->kind == OASP_TOKEN_IDENTIFIER && oasp_ascii_equal(token->text, word);
}

static bool oasp_match_kind(oasp_parser *parser, oasp_token_kind kind) {
    if (!oasp_at_kind(parser, kind))
        return false;
    parser->cursor++;
    return true;
}

static bool oasp_match_word(oasp_parser *parser, const char *word) {
    if (!oasp_at_word(parser, word))
        return false;
    parser->cursor++;
    return true;
}

static void oasp_set_error(oasp_parser *parser, const char *message) {
    if (parser->error)
        return;
    const oasp_token *token = oasp_parser_current(parser);
    parser->error = message;
    parser->error_line = token->line;
    parser->error_column = token->column;
}

static bool oasp_expect_kind(oasp_parser *parser, oasp_token_kind kind, const char *message) {
    if (oasp_match_kind(parser, kind))
        return true;
    oasp_set_error(parser, message);
    return false;
}

bool oasp_parser_init(oasp_parser *parser, oasp_arena *arena, const char *source, size_t length) {
    if (!parser || !arena || !source)
        return false;
    memset(parser, 0, sizeof(*parser));
    parser->arena = arena;
    if (!oasp_lex(arena, source, length, &parser->tokens)) {
        parser->error = parser->tokens.error;
        parser->error_line = parser->tokens.error_line;
        parser->error_column = parser->tokens.error_column;
        return false;
    }
    return true;
}

void oasp_parser_dispose(oasp_parser *parser) {
    if (!parser)
        return;
    oasp_token_list_dispose(&parser->tokens);
}

static int64_t oasp_parse_integer_token(oasp_slice token) {
    unsigned base = 10;
    size_t start = 0;
    size_t length = token.length;
    if (length > 1 && token.data[0] == '&') {
        if (token.data[1] == 'h' || token.data[1] == 'H') {
            base = 16;
            start = 2;
        } else {
            base = 8;
            start = 1;
        }
        if (length > start && token.data[length - 1] == '&')
            length--;
    }
    uint64_t parsed = 0;
    for (size_t i = start; i < length; i++) {
        unsigned char ch = (unsigned char)token.data[i];
        unsigned digit = isdigit(ch) ? (unsigned)(ch - '0') : (unsigned)(tolower(ch) - 'a' + 10);
        parsed = parsed * base + digit;
    }
    if (base == 16 && length - start == 8 && parsed > INT32_MAX)
        return (int64_t)(int32_t)parsed;
    int64_t result;
    memcpy(&result, &parsed, sizeof(result));
    return result;
}

static oasp_expr *oasp_parse_imp(oasp_parser *parser);

static bool oasp_append_arg(oasp_parser *parser, oasp_expr ***items, size_t *length, size_t *capacity, oasp_expr *expr) {
    if (*length == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 4;
        oasp_expr **grown = oasp_arena_alloc(parser->arena, next * sizeof(*grown), _Alignof(oasp_expr *));
        if (!grown)
            return false;
        if (*length)
            memcpy(grown, *items, *length * sizeof(*grown));
        *items = grown;
        *capacity = next;
    }
    (*items)[(*length)++] = expr;
    return true;
}

static oasp_expr *oasp_parse_postfix(oasp_parser *parser, oasp_expr *expr) {
    for (;;) {
        if (oasp_at_kind(parser, OASP_TOKEN_DOT) && parser->cursor > 0) {
            const oasp_token *previous = &parser->tokens.items[parser->cursor - 1];
            const oasp_token *dot = oasp_parser_current(parser);
            if (previous->line == dot->line && dot->column > previous->column + previous->source_width)
                break;
        }
        if (oasp_match_kind(parser, OASP_TOKEN_DOT)) {
            if (!oasp_at_kind(parser, OASP_TOKEN_IDENTIFIER)) {
                oasp_set_error(parser, "expected member name after '.'");
                return NULL;
            }
            oasp_slice name = oasp_parser_current(parser)->text;
            parser->cursor++;
            while (name.length) {
                const char *dot = memchr(name.data, '.', name.length);
                oasp_slice segment = {
                    name.data,
                    dot ? (size_t)(dot - name.data) : name.length,
                };
                if (!segment.length) {
                    oasp_set_error(parser, "expected member name after '.'");
                    return NULL;
                }
                oasp_expr *member = oasp_new_expr(parser, OASP_EXPR_MEMBER);
                if (!member)
                    return NULL;
                member->left = expr;
                member->name = segment;
                expr = member;
                if (!dot)
                    break;
                name.data = dot + 1;
                name.length -= segment.length + 1;
            }
            continue;
        }
        if (!oasp_match_kind(parser, OASP_TOKEN_LPAREN))
            break;
        oasp_expr *call = oasp_new_expr(parser, OASP_EXPR_CALL);
        if (!call)
            return NULL;
        call->left = expr;
        oasp_expr **args = NULL;
        size_t count = 0;
        size_t capacity = 0;
        if (!oasp_at_kind(parser, OASP_TOKEN_RPAREN)) {
            for (;;) {
                oasp_expr *arg = NULL;
                if (!oasp_at_kind(parser, OASP_TOKEN_COMMA) && !oasp_at_kind(parser, OASP_TOKEN_RPAREN))
                    arg = oasp_parse_imp(parser);
                if (parser->error || !oasp_append_arg(parser, &args, &count, &capacity, arg)) {
                    if (!parser->error)
                        oasp_set_error(parser, "out of memory");
                    return NULL;
                }
                if (!oasp_match_kind(parser, OASP_TOKEN_COMMA))
                    break;
            }
        }
        if (!oasp_expect_kind(parser, OASP_TOKEN_RPAREN, "expected ')' after arguments"))
            return NULL;
        call->args = args;
        call->arg_count = count;
        expr = call;
    }
    return expr;
}

static oasp_expr *oasp_parse_primary(oasp_parser *parser) {
    const oasp_token *token = oasp_parser_current(parser);
    if (oasp_match_kind(parser, OASP_TOKEN_STRING)) {
        oasp_expr *expr = oasp_new_expr(parser, OASP_EXPR_LITERAL);
        if (expr)
            expr->literal = oasp_value_string(token->text.data, token->text.length);
        return expr;
    }
    if (oasp_match_kind(parser, OASP_TOKEN_INTEGER)) {
        oasp_expr *expr = oasp_new_expr(parser, OASP_EXPR_LITERAL);
        if (expr)
            expr->literal = oasp_value_int(oasp_parse_integer_token(token->text));
        return expr;
    }
    if (oasp_match_kind(parser, OASP_TOKEN_FLOAT)) {
        oasp_expr *expr = oasp_new_expr(parser, OASP_EXPR_LITERAL);
        if (expr)
            expr->literal = oasp_value_float(strtod(token->text.data, NULL));
        return expr;
    }
    if (oasp_match_kind(parser, OASP_TOKEN_DATE)) {
        oasp_expr *expr = oasp_new_expr(parser, OASP_EXPR_LITERAL);
        if (expr) {
            expr->literal.kind = OASP_VALUE_DATE;
            expr->literal.as.date = token->text;
        }
        return expr;
    }
    if (oasp_match_kind(parser, OASP_TOKEN_LPAREN)) {
        oasp_expr *expr = oasp_parse_imp(parser);
        if (!oasp_expect_kind(parser, OASP_TOKEN_RPAREN, "expected ')' after expression"))
            return NULL;
        return oasp_parse_postfix(parser, expr);
    }
    if (oasp_match_word(parser, "true") || oasp_match_word(parser, "false")) {
        oasp_expr *expr = oasp_new_expr(parser, OASP_EXPR_LITERAL);
        if (expr)
            expr->literal = oasp_value_bool(oasp_ascii_equal(oasp_previous(parser)->text, "true"));
        return expr;
    }
    if (oasp_match_word(parser, "empty")) {
        oasp_expr *expr = oasp_new_expr(parser, OASP_EXPR_LITERAL);
        if (expr)
            expr->literal = oasp_value_empty();
        return expr;
    }
    if (oasp_match_word(parser, "null")) {
        oasp_expr *expr = oasp_new_expr(parser, OASP_EXPR_LITERAL);
        if (expr)
            expr->literal = oasp_value_null();
        return expr;
    }
    if (oasp_match_word(parser, "nothing")) {
        oasp_expr *expr = oasp_new_expr(parser, OASP_EXPR_LITERAL);
        if (expr)
            expr->literal = oasp_value_nothing();
        return expr;
    }
    if (oasp_match_word(parser, "new")) {
        if (!oasp_at_kind(parser, OASP_TOKEN_IDENTIFIER)) {
            oasp_set_error(parser, "expected class name after New");
            return NULL;
        }
        oasp_expr *expr = oasp_new_expr(parser, OASP_EXPR_NEW);
        if (expr)
            expr->name = oasp_parser_current(parser)->text;
        parser->cursor++;
        return oasp_parse_postfix(parser, expr);
    }
    oasp_expr *expr = NULL;
    if (oasp_match_kind(parser, OASP_TOKEN_DOT)) {
        if (!oasp_at_kind(parser, OASP_TOKEN_IDENTIFIER)) {
            oasp_set_error(parser, "expected member name after '.'");
            return NULL;
        }
        const oasp_token *member_token = oasp_parser_current(parser);
        char *name = oasp_arena_alloc(parser->arena, member_token->text.length + 2, _Alignof(char));
        if (!name) {
            oasp_set_error(parser, "out of memory");
            return NULL;
        }
        name[0] = '.';
        memcpy(name + 1, member_token->text.data, member_token->text.length);
        name[member_token->text.length + 1] = '\0';
        parser->cursor++;
        expr = oasp_new_expr(parser, OASP_EXPR_IDENTIFIER);
        if (!expr)
            return NULL;
        expr->name = (oasp_slice){name, member_token->text.length + 1};
    } else {
        if (!oasp_match_kind(parser, OASP_TOKEN_IDENTIFIER)) {
            oasp_set_error(parser, "expected expression");
            return NULL;
        }
        expr = oasp_new_expr(parser, OASP_EXPR_IDENTIFIER);
        if (!expr)
            return NULL;
        expr->name = token->text;
    }
    return oasp_parse_postfix(parser, expr);
}

static oasp_expr *oasp_make_unary(oasp_parser *parser, oasp_expr_kind kind, oasp_expr *operand) {
    oasp_expr *expr = oasp_new_expr(parser, kind);
    if (expr)
        expr->left = operand;
    return expr;
}

static oasp_expr *oasp_make_binary(oasp_parser *parser, oasp_expr_kind kind, oasp_expr *left, oasp_expr *right) {
    oasp_expr *expr = oasp_new_expr(parser, kind);
    if (expr) {
        expr->left = left;
        expr->right = right;
    }
    return expr;
}

static oasp_expr *oasp_parse_power(oasp_parser *parser) {
    oasp_expr *left = oasp_parse_primary(parser);
    if (!left || !oasp_match_kind(parser, OASP_TOKEN_CARET))
        return left;
    return oasp_make_binary(parser, OASP_EXPR_POWER, left, oasp_parse_power(parser));
}

static oasp_expr *oasp_parse_unary(oasp_parser *parser) {
    if (oasp_match_kind(parser, OASP_TOKEN_MINUS))
        return oasp_make_unary(parser, OASP_EXPR_NEGATE, oasp_parse_unary(parser));
    if (oasp_match_kind(parser, OASP_TOKEN_PLUS))
        return oasp_make_unary(parser, OASP_EXPR_POSITIVE, oasp_parse_unary(parser));
    return oasp_parse_power(parser);
}

static oasp_expr *oasp_parse_mul(oasp_parser *parser) {
    oasp_expr *expr = oasp_parse_unary(parser);
    while (!parser->error) {
        oasp_expr_kind kind;
        if (oasp_match_kind(parser, OASP_TOKEN_STAR))
            kind = OASP_EXPR_MULTIPLY;
        else if (oasp_match_kind(parser, OASP_TOKEN_SLASH))
            kind = OASP_EXPR_DIVIDE;
        else if (oasp_match_kind(parser, OASP_TOKEN_BACKSLASH))
            kind = OASP_EXPR_INT_DIVIDE;
        else if (oasp_match_word(parser, "mod"))
            kind = OASP_EXPR_MOD;
        else
            break;
        expr = oasp_make_binary(parser, kind, expr, oasp_parse_unary(parser));
    }
    return expr;
}

static oasp_expr *oasp_parse_add(oasp_parser *parser) {
    oasp_expr *expr = oasp_parse_mul(parser);
    while (!parser->error) {
        oasp_expr_kind kind;
        if (oasp_match_kind(parser, OASP_TOKEN_PLUS))
            kind = OASP_EXPR_ADD;
        else if (oasp_match_kind(parser, OASP_TOKEN_MINUS))
            kind = OASP_EXPR_SUBTRACT;
        else
            break;
        expr = oasp_make_binary(parser, kind, expr, oasp_parse_mul(parser));
    }
    return expr;
}

static oasp_expr *oasp_parse_concat(oasp_parser *parser) {
    oasp_expr *expr = oasp_parse_add(parser);
    while (!parser->error && oasp_match_kind(parser, OASP_TOKEN_AMP))
        expr = oasp_make_binary(parser, OASP_EXPR_CONCAT, expr, oasp_parse_add(parser));
    return expr;
}

static oasp_expr *oasp_parse_compare(oasp_parser *parser) {
    oasp_expr *expr = oasp_parse_concat(parser);
    while (!parser->error) {
        oasp_expr_kind kind;
        if (oasp_match_kind(parser, OASP_TOKEN_EQ))
            kind = OASP_EXPR_EQ;
        else if (oasp_match_kind(parser, OASP_TOKEN_NE))
            kind = OASP_EXPR_NE;
        else if (oasp_match_kind(parser, OASP_TOKEN_LESS))
            kind = OASP_EXPR_LT;
        else if (oasp_match_kind(parser, OASP_TOKEN_LE))
            kind = OASP_EXPR_LE;
        else if (oasp_match_kind(parser, OASP_TOKEN_GREATER))
            kind = OASP_EXPR_GT;
        else if (oasp_match_kind(parser, OASP_TOKEN_GE))
            kind = OASP_EXPR_GE;
        else if (oasp_match_word(parser, "is")) {
            kind = oasp_match_word(parser, "not") ? OASP_EXPR_IS_NOT : OASP_EXPR_IS;
        } else
            break;
        expr = oasp_make_binary(parser, kind, expr, oasp_parse_concat(parser));
    }
    return expr;
}

static oasp_expr *oasp_parse_not(oasp_parser *parser) {
    if (oasp_match_word(parser, "not"))
        return oasp_make_unary(parser, OASP_EXPR_NOT, oasp_parse_not(parser));
    return oasp_parse_compare(parser);
}

#define OASP_DEFINE_BINARY_PARSER(name, lower, keyword, expression_kind)                                                                                       \
    static oasp_expr *name(oasp_parser *parser) {                                                                                                              \
        oasp_expr *expr = lower(parser);                                                                                                                       \
        while (!parser->error && oasp_match_word(parser, keyword))                                                                                             \
            expr = oasp_make_binary(parser, expression_kind, expr, lower(parser));                                                                             \
        return expr;                                                                                                                                           \
    }

OASP_DEFINE_BINARY_PARSER(oasp_parse_and, oasp_parse_not, "and", OASP_EXPR_AND)
OASP_DEFINE_BINARY_PARSER(oasp_parse_xor, oasp_parse_and, "xor", OASP_EXPR_XOR)
OASP_DEFINE_BINARY_PARSER(oasp_parse_or, oasp_parse_xor, "or", OASP_EXPR_OR)
OASP_DEFINE_BINARY_PARSER(oasp_parse_eqv, oasp_parse_or, "eqv", OASP_EXPR_EQV)
OASP_DEFINE_BINARY_PARSER(oasp_parse_imp, oasp_parse_eqv, "imp", OASP_EXPR_IMP)

oasp_expr *oasp_parse_expression(oasp_parser *parser) {
    if (!parser || parser->error)
        return NULL;
    return oasp_parse_imp(parser);
}

static void oasp_block_append(oasp_block *block, oasp_stmt *stmt) {
    if (!block->first)
        block->first = stmt;
    else
        block->last->next = stmt;
    block->last = stmt;
}

static void oasp_skip_separators(oasp_parser *parser) {
    while (oasp_match_kind(parser, OASP_TOKEN_NEWLINE) || oasp_match_kind(parser, OASP_TOKEN_COLON)) {
    }
}

static bool oasp_append_declarator(oasp_parser *parser, oasp_stmt *stmt, size_t *capacity, oasp_declarator declarator) {
    if (stmt->declaration_count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 4;
        oasp_declarator *items = oasp_arena_alloc(parser->arena, next * sizeof(*items), _Alignof(oasp_declarator));
        if (!items) {
            oasp_set_error(parser, "out of memory");
            return false;
        }
        if (stmt->declaration_count)
            memcpy(items, stmt->declarations, stmt->declaration_count * sizeof(*items));
        stmt->declarations = items;
        *capacity = next;
    }
    stmt->declarations[stmt->declaration_count++] = declarator;
    return true;
}

static bool oasp_parse_declarator(oasp_parser *parser, oasp_declarator *declarator) {
    memset(declarator, 0, sizeof(*declarator));
    if (!oasp_at_kind(parser, OASP_TOKEN_IDENTIFIER)) {
        oasp_set_error(parser, "expected variable name");
        return false;
    }
    declarator->name = oasp_parser_current(parser)->text;
    parser->cursor++;
    if (!oasp_match_kind(parser, OASP_TOKEN_LPAREN))
        return true;
    size_t capacity = 0;
    if (!oasp_at_kind(parser, OASP_TOKEN_RPAREN)) {
        for (;;) {
            oasp_expr *dimension = oasp_parse_expression(parser);
            if (!dimension || !oasp_append_arg(parser, &declarator->dimensions, &declarator->dimension_count, &capacity, dimension))
                return false;
            if (!oasp_match_kind(parser, OASP_TOKEN_COMMA))
                break;
        }
    }
    return oasp_expect_kind(parser, OASP_TOKEN_RPAREN, "expected ')' after array dimensions");
}

static oasp_stmt *oasp_parse_declaration(oasp_parser *parser, oasp_stmt_kind kind) {
    oasp_stmt *stmt = oasp_new_stmt(parser, kind);
    if (!stmt)
        return NULL;
    size_t capacity = 0;
    do {
        oasp_declarator declarator;
        if (!oasp_parse_declarator(parser, &declarator) || !oasp_append_declarator(parser, stmt, &capacity, declarator))
            return NULL;
    } while (oasp_match_kind(parser, OASP_TOKEN_COMMA));
    return stmt;
}

static oasp_stmt *oasp_parse_const(oasp_parser *parser) {
    oasp_stmt *stmt = oasp_new_stmt(parser, OASP_STMT_CONST);
    if (!stmt)
        return NULL;
    size_t capacity = 0;
    do {
        if (!oasp_at_kind(parser, OASP_TOKEN_IDENTIFIER)) {
            oasp_set_error(parser, "expected constant name");
            return NULL;
        }
        oasp_const_declarator item;
        item.name = oasp_parser_current(parser)->text;
        parser->cursor++;
        if (!oasp_expect_kind(parser, OASP_TOKEN_EQ, "expected '=' after constant name"))
            return NULL;
        item.value = oasp_parse_expression(parser);
        if (!item.value)
            return NULL;
        if (stmt->constant_count == capacity) {
            size_t next = capacity ? capacity * 2 : 4;
            oasp_const_declarator *items = oasp_arena_alloc(parser->arena, next * sizeof(*items), _Alignof(oasp_const_declarator));
            if (!items) {
                oasp_set_error(parser, "out of memory");
                return NULL;
            }
            if (stmt->constant_count)
                memcpy(items, stmt->constants, stmt->constant_count * sizeof(*items));
            stmt->constants = items;
            capacity = next;
        }
        stmt->constants[stmt->constant_count++] = item;
    } while (oasp_match_kind(parser, OASP_TOKEN_COMMA));
    return stmt;
}

static bool oasp_statement_end(const oasp_parser *parser) {
    return oasp_at_kind(parser, OASP_TOKEN_NEWLINE) || oasp_at_kind(parser, OASP_TOKEN_COLON) || oasp_at_kind(parser, OASP_TOKEN_EOF);
}

static oasp_stmt *oasp_parse_statement(oasp_parser *parser);

static bool oasp_at_any_word(const oasp_parser *parser, const char *const *words, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (oasp_at_word(parser, words[i]))
            return true;
    return false;
}

static bool oasp_parse_nested_block(oasp_parser *parser, oasp_block *block, const char *const *stops, size_t stop_count) {
    memset(block, 0, sizeof(*block));
    oasp_skip_separators(parser);
    while (!oasp_at_kind(parser, OASP_TOKEN_EOF) && !parser->error && !oasp_at_any_word(parser, stops, stop_count)) {
        oasp_stmt *stmt = oasp_parse_statement(parser);
        if (!stmt)
            return false;
        oasp_block_append(block, stmt);
        if (!oasp_statement_end(parser)) {
            oasp_set_error(parser, "expected end of statement");
            return false;
        }
        oasp_skip_separators(parser);
    }
    return !parser->error;
}

static bool oasp_parse_block_until_end(oasp_parser *parser, oasp_block *block, const char *end_word) {
    memset(block, 0, sizeof(*block));
    oasp_skip_separators(parser);
    while (!oasp_at_kind(parser, OASP_TOKEN_EOF) && !parser->error) {
        if (oasp_at_word(parser, "end") && parser->cursor + 1 < parser->tokens.length &&
            parser->tokens.items[parser->cursor + 1].kind == OASP_TOKEN_IDENTIFIER &&
            oasp_ascii_equal(parser->tokens.items[parser->cursor + 1].text, end_word)) {
            parser->cursor += 2;
            return true;
        }
        oasp_stmt *stmt = oasp_parse_statement(parser);
        if (!stmt)
            return false;
        oasp_block_append(block, stmt);
        if (!oasp_statement_end(parser)) {
            oasp_set_error(parser, "expected end of statement");
            return false;
        }
        oasp_skip_separators(parser);
    }
    oasp_set_error(parser, "expected matching End statement");
    return false;
}

static bool oasp_append_parameter(oasp_parser *parser, oasp_stmt *stmt, size_t *capacity, oasp_parameter parameter) {
    if (stmt->parameter_count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 4;
        oasp_parameter *items = oasp_arena_alloc(parser->arena, next * sizeof(*items), _Alignof(oasp_parameter));
        if (!items) {
            oasp_set_error(parser, "out of memory");
            return false;
        }
        if (stmt->parameter_count)
            memcpy(items, stmt->parameters, stmt->parameter_count * sizeof(*items));
        stmt->parameters = items;
        *capacity = next;
    }
    stmt->parameters[stmt->parameter_count++] = parameter;
    return true;
}

static oasp_stmt *oasp_parse_routine(oasp_parser *parser, oasp_routine_kind kind, bool is_default) {
    oasp_stmt *stmt = oasp_new_stmt(parser, OASP_STMT_ROUTINE);
    if (!stmt)
        return NULL;
    stmt->routine_kind = kind;
    stmt->is_default = is_default;
    if (!oasp_at_kind(parser, OASP_TOKEN_IDENTIFIER)) {
        oasp_set_error(parser, "expected routine name");
        return NULL;
    }
    stmt->name = oasp_parser_current(parser)->text;
    parser->cursor++;
    size_t capacity = 0;
    if (oasp_match_kind(parser, OASP_TOKEN_LPAREN)) {
        if (!oasp_at_kind(parser, OASP_TOKEN_RPAREN)) {
            for (;;) {
                oasp_parameter parameter;
                memset(&parameter, 0, sizeof(parameter));
                parameter.by_value = oasp_match_word(parser, "byval");
                if (!parameter.by_value)
                    (void)oasp_match_word(parser, "byref");
                if (!oasp_at_kind(parser, OASP_TOKEN_IDENTIFIER)) {
                    oasp_set_error(parser, "expected parameter name");
                    return NULL;
                }
                parameter.name = oasp_parser_current(parser)->text;
                parser->cursor++;
                if (oasp_match_kind(parser, OASP_TOKEN_LPAREN)) {
                    parameter.array = true;
                    if (!oasp_expect_kind(parser, OASP_TOKEN_RPAREN, "expected ')' after array parameter"))
                        return NULL;
                }
                if (!oasp_append_parameter(parser, stmt, &capacity, parameter))
                    return NULL;
                if (!oasp_match_kind(parser, OASP_TOKEN_COMMA))
                    break;
            }
        }
        if (!oasp_expect_kind(parser, OASP_TOKEN_RPAREN, "expected ')' after parameter list"))
            return NULL;
    }
    if (!oasp_statement_end(parser)) {
        oasp_set_error(parser, "expected end of routine declaration");
        return NULL;
    }
    oasp_skip_separators(parser);
    const char *end_word = kind == OASP_ROUTINE_SUB ? "sub" : (kind == OASP_ROUTINE_FUNCTION ? "function" : "property");
    return oasp_parse_block_until_end(parser, &stmt->body, end_word) ? stmt : NULL;
}

static oasp_stmt *oasp_parse_class(oasp_parser *parser) {
    oasp_stmt *stmt = oasp_new_stmt(parser, OASP_STMT_CLASS);
    if (!stmt)
        return NULL;
    if (!oasp_at_kind(parser, OASP_TOKEN_IDENTIFIER)) {
        oasp_set_error(parser, "expected class name");
        return NULL;
    }
    stmt->name = oasp_parser_current(parser)->text;
    parser->cursor++;
    if (!oasp_statement_end(parser)) {
        oasp_set_error(parser, "expected end of class declaration");
        return NULL;
    }
    oasp_skip_separators(parser);
    return oasp_parse_block_until_end(parser, &stmt->body, "class") ? stmt : NULL;
}

static oasp_stmt *oasp_parse_if_after_keyword(oasp_parser *parser) {
    oasp_stmt *stmt = oasp_new_stmt(parser, OASP_STMT_IF);
    if (!stmt)
        return NULL;
    stmt->condition = oasp_parse_expression(parser);
    if (!stmt->condition || !oasp_match_word(parser, "then")) {
        oasp_set_error(parser, "expected Then after If condition");
        return NULL;
    }
    if (!oasp_statement_end(parser)) {
        for (;;) {
            oasp_stmt *inline_stmt = oasp_parse_statement(parser);
            if (!inline_stmt)
                return NULL;
            oasp_block_append(&stmt->body, inline_stmt);
            if (!oasp_match_kind(parser, OASP_TOKEN_COLON) || oasp_statement_end(parser) || oasp_at_word(parser, "else") || oasp_at_word(parser, "end"))
                break;
        }
        if (oasp_match_word(parser, "else")) {
            /*
             * VBScript permits a separator between Else and the alternative
             * statement in a single-line If: `If test Then a=1:Else:a=2`.
             */
            (void)oasp_match_kind(parser, OASP_TOKEN_COLON);
            for (;;) {
                oasp_stmt *alternative = oasp_parse_statement(parser);
                if (!alternative)
                    return NULL;
                oasp_block_append(&stmt->alternative, alternative);
                if (!oasp_match_kind(parser, OASP_TOKEN_COLON) || oasp_statement_end(parser) || oasp_at_word(parser, "end"))
                    break;
            }
        }
        if (oasp_match_word(parser, "end") && !oasp_match_word(parser, "if")) {
            oasp_set_error(parser, "expected If after End");
            return NULL;
        }
        return stmt;
    }
    static const char *const stops[] = {"elseif", "else", "end"};
    if (!oasp_parse_nested_block(parser, &stmt->body, stops, OASP_ARRAY_COUNT(stops)))
        return NULL;
    if (oasp_match_word(parser, "elseif")) {
        oasp_stmt *nested = oasp_parse_if_after_keyword(parser);
        if (!nested)
            return NULL;
        oasp_block_append(&stmt->alternative, nested);
        return stmt;
    }
    if (oasp_match_word(parser, "else")) {
        static const char *const end_stop[] = {"end"};
        if (!oasp_parse_nested_block(parser, &stmt->alternative, end_stop, 1))
            return NULL;
    }
    if (!oasp_match_word(parser, "end") || !oasp_match_word(parser, "if")) {
        oasp_set_error(parser, "expected End If");
        return NULL;
    }
    return stmt;
}

static oasp_stmt *oasp_parse_while(oasp_parser *parser) {
    oasp_stmt *stmt = oasp_new_stmt(parser, OASP_STMT_WHILE);
    if (!stmt)
        return NULL;
    stmt->condition = oasp_parse_expression(parser);
    if (!oasp_statement_end(parser)) {
        oasp_set_error(parser, "expected end of While statement");
        return NULL;
    }
    static const char *const stops[] = {"wend"};
    if (!oasp_parse_nested_block(parser, &stmt->body, stops, 1) || !oasp_match_word(parser, "wend")) {
        oasp_set_error(parser, "expected Wend");
        return NULL;
    }
    return stmt;
}

static oasp_stmt *oasp_parse_do(oasp_parser *parser) {
    oasp_stmt *stmt = oasp_new_stmt(parser, OASP_STMT_DO);
    if (!stmt)
        return NULL;
    if (oasp_match_word(parser, "while") || oasp_match_word(parser, "until")) {
        stmt->until = oasp_ascii_equal(oasp_previous(parser)->text, "until");
        stmt->condition = oasp_parse_expression(parser);
    }
    if (!oasp_statement_end(parser)) {
        oasp_set_error(parser, "expected end of Do statement");
        return NULL;
    }
    static const char *const stops[] = {"loop"};
    if (!oasp_parse_nested_block(parser, &stmt->body, stops, 1) || !oasp_match_word(parser, "loop")) {
        oasp_set_error(parser, "expected Loop");
        return NULL;
    }
    if (oasp_match_word(parser, "while") || oasp_match_word(parser, "until")) {
        stmt->post_condition = true;
        stmt->until = oasp_ascii_equal(oasp_previous(parser)->text, "until");
        stmt->condition = oasp_parse_expression(parser);
    }
    return stmt;
}

static oasp_stmt *oasp_parse_for(oasp_parser *parser) {
    bool each = oasp_match_word(parser, "each");
    oasp_stmt *stmt = oasp_new_stmt(parser, each ? OASP_STMT_FOR_EACH : OASP_STMT_FOR);
    if (!stmt || !oasp_at_kind(parser, OASP_TOKEN_IDENTIFIER)) {
        if (!parser->error)
            oasp_set_error(parser, "expected For variable");
        return NULL;
    }
    stmt->name = oasp_parser_current(parser)->text;
    parser->cursor++;
    if (each) {
        if (!oasp_match_word(parser, "in")) {
            oasp_set_error(parser, "expected In after For Each variable");
            return NULL;
        }
        stmt->value = oasp_parse_expression(parser);
    } else {
        if (!oasp_expect_kind(parser, OASP_TOKEN_EQ, "expected '=' after For variable"))
            return NULL;
        stmt->value = oasp_parse_expression(parser);
        if (!oasp_match_word(parser, "to")) {
            oasp_set_error(parser, "expected To in For statement");
            return NULL;
        }
        stmt->limit = oasp_parse_expression(parser);
        if (oasp_match_word(parser, "step"))
            stmt->step = oasp_parse_expression(parser);
    }
    if (!oasp_statement_end(parser)) {
        oasp_set_error(parser, "expected end of For statement");
        return NULL;
    }
    static const char *const stops[] = {"next"};
    if (!oasp_parse_nested_block(parser, &stmt->body, stops, 1) || !oasp_match_word(parser, "next")) {
        oasp_set_error(parser, "expected Next");
        return NULL;
    }
    if (oasp_at_kind(parser, OASP_TOKEN_IDENTIFIER))
        parser->cursor++;
    return stmt;
}

static oasp_stmt *oasp_parse_select(oasp_parser *parser) {
    oasp_stmt *stmt = oasp_new_stmt(parser, OASP_STMT_SELECT);
    if (!stmt)
        return NULL;
    if (!oasp_match_word(parser, "case")) {
        oasp_set_error(parser, "expected Case after Select");
        return NULL;
    }
    stmt->value = oasp_parse_expression(parser);
    if (!oasp_statement_end(parser)) {
        oasp_set_error(parser, "expected end of Select statement");
        return NULL;
    }
    oasp_case_branch **tail = &stmt->cases;
    oasp_skip_separators(parser);
    while (oasp_match_word(parser, "case")) {
        oasp_case_branch *branch = oasp_arena_alloc(parser->arena, sizeof(*branch), _Alignof(oasp_case_branch));
        if (!branch) {
            oasp_set_error(parser, "out of memory");
            return NULL;
        }
        memset(branch, 0, sizeof(*branch));
        branch->is_else = oasp_match_word(parser, "else");
        size_t capacity = 0;
        if (!branch->is_else) {
            do {
                oasp_expr *value = oasp_parse_expression(parser);
                if (!value || !oasp_append_arg(parser, &branch->values, &branch->value_count, &capacity, value))
                    return NULL;
            } while (oasp_match_kind(parser, OASP_TOKEN_COMMA));
        }
        if (!oasp_statement_end(parser)) {
            oasp_stmt *inline_stmt = oasp_parse_statement(parser);
            if (!inline_stmt)
                return NULL;
            oasp_block_append(&branch->body, inline_stmt);
            if (!oasp_statement_end(parser)) {
                oasp_set_error(parser, "expected end of inline Case statement");
                return NULL;
            }
        }
        static const char *const stops[] = {"case", "end"};
        oasp_block remainder;
        if (!oasp_parse_nested_block(parser, &remainder, stops, 2))
            return NULL;
        if (remainder.first) {
            if (branch->body.last)
                branch->body.last->next = remainder.first;
            else
                branch->body.first = remainder.first;
            branch->body.last = remainder.last;
        }
        *tail = branch;
        tail = &branch->next;
    }
    if (!oasp_match_word(parser, "end") || !oasp_match_word(parser, "select")) {
        oasp_set_error(parser, "expected End Select");
        return NULL;
    }
    return stmt;
}

static oasp_stmt *oasp_parse_with(oasp_parser *parser) {
    oasp_stmt *stmt = oasp_new_stmt(parser, OASP_STMT_WITH);
    if (!stmt)
        return NULL;
    stmt->value = oasp_parse_expression(parser);
    if (!oasp_statement_end(parser)) {
        oasp_set_error(parser, "expected end of With statement");
        return NULL;
    }
    static const char *const stops[] = {"end"};
    if (!oasp_parse_nested_block(parser, &stmt->body, stops, 1) || !oasp_match_word(parser, "end") || !oasp_match_word(parser, "with")) {
        oasp_set_error(parser, "expected End With");
        return NULL;
    }
    return stmt;
}

static oasp_stmt *oasp_parse_simple_statement(oasp_parser *parser) {
    bool set_assignment = oasp_match_word(parser, "set");
    bool explicit_call = oasp_match_word(parser, "call");
    if (oasp_match_word(parser, "option")) {
        if (!oasp_match_word(parser, "explicit"))
            oasp_set_error(parser, "expected Explicit after Option");
        return oasp_new_stmt(parser, OASP_STMT_OPTION_EXPLICIT);
    }
    if (oasp_match_word(parser, "on")) {
        oasp_stmt_kind kind = OASP_STMT_NOOP;
        if (!oasp_match_word(parser, "error")) {
            oasp_set_error(parser, "expected Error after On");
        } else if (oasp_match_word(parser, "resume") && oasp_match_word(parser, "next")) {
            kind = OASP_STMT_ON_ERROR_RESUME_NEXT;
        } else if (oasp_match_word(parser, "goto") && oasp_match_kind(parser, OASP_TOKEN_INTEGER) && oasp_ascii_equal(oasp_previous(parser)->text, "0")) {
            kind = OASP_STMT_ON_ERROR_GOTO_0;
        } else {
            oasp_set_error(parser, "invalid On Error statement");
        }
        return oasp_new_stmt(parser, kind);
    }
    if (oasp_match_word(parser, "dim"))
        return oasp_parse_declaration(parser, OASP_STMT_DIM);
    if (oasp_match_word(parser, "redim")) {
        bool preserve = oasp_match_word(parser, "preserve");
        oasp_stmt *stmt = oasp_parse_declaration(parser, OASP_STMT_REDIM);
        if (stmt)
            stmt->preserve = preserve;
        return stmt;
    }
    if (oasp_at_word(parser, "erase") && parser->cursor + 1 < parser->tokens.length && parser->tokens.items[parser->cursor + 1].kind != OASP_TOKEN_EQ) {
        parser->cursor++;
        oasp_stmt *stmt = oasp_new_stmt(parser, OASP_STMT_ERASE);
        if (!stmt)
            return NULL;
        size_t capacity = 0;
        do {
            oasp_expr *target = oasp_parse_primary(parser);
            if (!target || !oasp_append_arg(parser, &stmt->expressions, &stmt->expression_count, &capacity, target))
                return NULL;
        } while (oasp_match_kind(parser, OASP_TOKEN_COMMA));
        return stmt;
    }
    size_t expression_start = parser->cursor;
    oasp_expr *head = oasp_parse_primary(parser);
    if (head && oasp_match_kind(parser, OASP_TOKEN_EQ)) {
        oasp_stmt *stmt = oasp_new_stmt(parser, set_assignment ? OASP_STMT_SET_ASSIGN : OASP_STMT_ASSIGN);
        if (!stmt)
            return NULL;
        stmt->target = head;
        stmt->value = oasp_parse_expression(parser);
        return stmt;
    }
    parser->cursor = expression_start;
    parser->error = NULL;
    head = oasp_parse_expression(parser);
    if (!head)
        return NULL;
    if (explicit_call && head->kind != OASP_EXPR_CALL) {
        if (head->kind != OASP_EXPR_IDENTIFIER && head->kind != OASP_EXPR_MEMBER) {
            oasp_set_error(parser, "expected procedure call after Call");
            return NULL;
        }
        oasp_expr *call = oasp_new_expr(parser, OASP_EXPR_CALL);
        if (!call)
            return NULL;
        call->left = head;
        head = call;
    }
    if (!explicit_call && !oasp_at_kind(parser, OASP_TOKEN_NEWLINE) && !oasp_at_kind(parser, OASP_TOKEN_COLON) && !oasp_at_kind(parser, OASP_TOKEN_EOF)) {
        oasp_expr *call = oasp_new_expr(parser, OASP_EXPR_CALL);
        if (!call)
            return NULL;
        call->left = head;
        size_t capacity = 0;
        do {
            oasp_expr *arg = NULL;
            if (!oasp_at_kind(parser, OASP_TOKEN_COMMA) && !oasp_at_kind(parser, OASP_TOKEN_NEWLINE) && !oasp_at_kind(parser, OASP_TOKEN_COLON) &&
                !oasp_at_kind(parser, OASP_TOKEN_EOF))
                arg = oasp_parse_expression(parser);
            if (!oasp_append_arg(parser, &call->args, &call->arg_count, &capacity, arg)) {
                oasp_set_error(parser, "out of memory");
                return NULL;
            }
        } while (oasp_match_kind(parser, OASP_TOKEN_COMMA));
        head = call;
    }
    if (head->kind == OASP_EXPR_IDENTIFIER || head->kind == OASP_EXPR_MEMBER) {
        oasp_expr *call = oasp_new_expr(parser, OASP_EXPR_CALL);
        if (!call)
            return NULL;
        call->left = head;
        head = call;
    }
    oasp_stmt *stmt = oasp_new_stmt(parser, OASP_STMT_CALL);
    if (stmt)
        stmt->value = head;
    return stmt;
}

static oasp_stmt *oasp_parse_statement(oasp_parser *parser) {
    bool is_default = false;
    bool has_access_modifier = false;
    if (oasp_match_word(parser, "public") || oasp_match_word(parser, "private")) {
        has_access_modifier = true;
        is_default = oasp_match_word(parser, "default");
        if (oasp_statement_end(parser))
            return oasp_new_stmt(parser, OASP_STMT_NOOP);
    } else if (oasp_at_word(parser, "default") && parser->cursor + 1 < parser->tokens.length &&
               parser->tokens.items[parser->cursor + 1].kind != OASP_TOKEN_EQ && oasp_match_word(parser, "default")) {
        if (oasp_statement_end(parser))
            return oasp_new_stmt(parser, OASP_STMT_NOOP);
        is_default = true;
    }
    if (oasp_match_word(parser, "class"))
        return oasp_parse_class(parser);
    if (oasp_match_word(parser, "if"))
        return oasp_parse_if_after_keyword(parser);
    if (oasp_match_word(parser, "while"))
        return oasp_parse_while(parser);
    if (oasp_match_word(parser, "do"))
        return oasp_parse_do(parser);
    if (oasp_match_word(parser, "for"))
        return oasp_parse_for(parser);
    if (oasp_match_word(parser, "select"))
        return oasp_parse_select(parser);
    if (oasp_match_word(parser, "with"))
        return oasp_parse_with(parser);
    if (oasp_match_word(parser, "exit")) {
        oasp_stmt *stmt = oasp_new_stmt(parser, OASP_STMT_EXIT);
        if (!stmt)
            return NULL;
        if (!oasp_at_kind(parser, OASP_TOKEN_IDENTIFIER)) {
            oasp_set_error(parser, "expected statement kind after Exit");
            return NULL;
        }
        stmt->name = oasp_parser_current(parser)->text;
        parser->cursor++;
        return stmt;
    }
    if (oasp_match_word(parser, "sub"))
        return oasp_parse_routine(parser, OASP_ROUTINE_SUB, is_default);
    if (oasp_match_word(parser, "function"))
        return oasp_parse_routine(parser, OASP_ROUTINE_FUNCTION, is_default);
    if (oasp_match_word(parser, "property")) {
        oasp_routine_kind kind;
        if (oasp_match_word(parser, "get"))
            kind = OASP_ROUTINE_PROPERTY_GET;
        else if (oasp_match_word(parser, "let"))
            kind = OASP_ROUTINE_PROPERTY_LET;
        else if (oasp_match_word(parser, "set"))
            kind = OASP_ROUTINE_PROPERTY_SET;
        else {
            oasp_set_error(parser, "expected Get, Let, or Set after Property");
            return NULL;
        }
        return oasp_parse_routine(parser, kind, is_default);
    }
    if (oasp_match_word(parser, "const"))
        return oasp_parse_const(parser);
    if (has_access_modifier && oasp_at_kind(parser, OASP_TOKEN_IDENTIFIER))
        return oasp_parse_declaration(parser, OASP_STMT_DIM);
    return oasp_parse_simple_statement(parser);
}

bool oasp_parse_program(oasp_parser *parser, oasp_block *program) {
    if (!parser || !program || parser->error)
        return false;
    memset(program, 0, sizeof(*program));
    oasp_skip_separators(parser);
    while (!oasp_at_kind(parser, OASP_TOKEN_EOF) && !parser->error) {
        if (oasp_at_word(parser, "end") || oasp_at_word(parser, "else") || oasp_at_word(parser, "elseif") || oasp_at_word(parser, "next") ||
            oasp_at_word(parser, "loop") || oasp_at_word(parser, "wend")) {
            oasp_set_error(parser, "unexpected block terminator");
            break;
        }
        oasp_stmt *stmt = oasp_parse_statement(parser);
        if (!stmt)
            break;
        oasp_block_append(program, stmt);
        if (!oasp_at_kind(parser, OASP_TOKEN_EOF) && !oasp_at_kind(parser, OASP_TOKEN_NEWLINE) && !oasp_at_kind(parser, OASP_TOKEN_COLON))
            oasp_set_error(parser, "expected end of statement");
        oasp_skip_separators(parser);
    }
    return !parser->error;
}
