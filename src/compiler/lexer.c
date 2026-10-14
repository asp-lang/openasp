// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Converts explicit-length ASP source into a token vector. Most token text is
 * a borrowed source slice; decoded quoted strings are copied into `arena`.
 * Line/column and source_width preserve diagnostics and parser lookahead across
 * CRLF, continuations, comments, bracketed identifiers, and date literals.
 */

#include "lexer.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/*
 * Ensures room for `capacity` tokens. Capacity doubles from 128 while every
 * byte multiplication is overflow-checked; realloc failure preserves the old
 * token vector so oasp_token_list_dispose() remains valid.
 */
static bool oasp_token_reserve(oasp_token_list *tokens, size_t capacity) {
    if (capacity <= tokens->capacity)
        return true;
    size_t next = tokens->capacity ? tokens->capacity * 2 : 128;
    if (next < capacity)
        next = capacity;
    if (next > SIZE_MAX / sizeof(*tokens->items))
        return false;
    oasp_token *items = realloc(tokens->items, next * sizeof(*items));
    if (!items)
        return false;
    tokens->items = items;
    tokens->capacity = next;
    return true;
}

/* Appends one borrowed token and records its physical source width. */
static bool oasp_token_push(oasp_token_list *tokens, oasp_token_kind kind, const char *text, size_t length, size_t line,
                            size_t column) {
    if (!oasp_token_reserve(tokens, tokens->length + 1))
        return false;
    tokens->items[tokens->length++] = (oasp_token){.kind = kind, .text = {text, length}, .line = line, .column = column, .source_width = length};
    return true;
}

/* Copies decoded token text into `arena` before appending the token metadata. */
static bool oasp_token_push_copy(oasp_arena *arena, oasp_token_list *tokens, oasp_token_kind kind, const char *text, size_t length,
                                 size_t line, size_t column) {
    char *copy = oasp_arena_copy(arena, text, length);
    return copy && oasp_token_push(tokens, kind, copy, length, line, column);
}

static bool oasp_identifier_start(unsigned char ch) { return isalpha(ch) || ch == '_'; }

static bool oasp_identifier_part(unsigned char ch) { return isalnum(ch) || ch == '_'; }

static bool oasp_hex_digit(unsigned char ch) { return isdigit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'); }

/*
 * Consumes a valid decimal exponent and updates `cursor` only after at least
 * one exponent digit is present. This permits the caller to fall back to an
 * integer/float token when a trailing E is actually separate source text.
 */
static bool oasp_scan_exponent(const char *source, size_t length, size_t *cursor) {
    size_t start = *cursor;
    if (start >= length || (source[start] != 'e' && source[start] != 'E'))
        return false;
    size_t i = start + 1;
    if (i < length && (source[i] == '+' || source[i] == '-'))
        i++;
    if (i >= length || !isdigit((unsigned char)source[i]))
        return false;
    while (i < length && isdigit((unsigned char)source[i]))
        i++;
    *cursor = i;
    return true;
}

/*
 * Decodes a VBScript string, where doubled quotes represent one quote. Embedded
 * newlines and EOF produce a location-aware lexical error; temporary heap
 * storage is disposed on every exit and final bytes move to the source arena.
 */
static bool oasp_lex_string(oasp_arena *arena, const char *source, size_t length, size_t *cursor, size_t line,
                            size_t column, oasp_token_list *tokens) {
    size_t i = *cursor + 1;
    oasp_buffer decoded;
    if (!oasp_buffer_init(&decoded, 32))
        return false;
    while (i < length) {
        if (source[i] == '"') {
            if (i + 1 < length && source[i + 1] == '"') {
                if (!oasp_buffer_append_byte(&decoded, '"'))
                    goto oom;
                i += 2;
                continue;
            }
            i++;
            bool ok = oasp_token_push_copy(arena, tokens, OASP_TOKEN_STRING, decoded.data, decoded.length, line, column);
            oasp_buffer_dispose(&decoded);
            *cursor = i;
            return ok;
        }
        if (source[i] == '\r' || source[i] == '\n') {
            tokens->error = "unterminated string literal";
            tokens->error_line = line;
            tokens->error_column = column;
            oasp_buffer_dispose(&decoded);
            return false;
        }
        if (!oasp_buffer_append_byte(&decoded, source[i++]))
            goto oom;
    }
    tokens->error = "unterminated string literal";
    tokens->error_line = line;
    tokens->error_column = column;
    oasp_buffer_dispose(&decoded);
    return false;
oom:
    oasp_buffer_dispose(&decoded);
    return false;
}

/*
 * Scans `source[0..length)` once, updating `i`, `line`, and `column` together.
 * `statement_start` is required because `Rem` is a comment keyword only at a
 * statement boundary. On success an explicit EOF token terminates lookahead.
 */
bool oasp_lex(oasp_arena *arena, const char *source, size_t length, oasp_token_list *tokens) {
    if (!arena || !source || !tokens)
        return false;
    memset(tokens, 0, sizeof(*tokens));
    size_t i = 0;
    size_t line = 1;
    size_t column = 1;
    bool statement_start = true;
    while (i < length) {
        unsigned char ch = (unsigned char)source[i];
        if (ch == ' ' || ch == '\t' || ch == '\f') {
            i++;
            column++;
            continue;
        }
        if (ch == '_') {
            size_t j = i + 1;
            while (j < length && (source[j] == ' ' || source[j] == '\t'))
                j++;
            if (j < length && (source[j] == '\r' || source[j] == '\n')) {
                if (source[j] == '\r' && j + 1 < length && source[j + 1] == '\n')
                    j++;
                i = j + 1;
                line++;
                column = 1;
                for (;;) {
                    while (i < length && (source[i] == ' ' || source[i] == '\t' || source[i] == '\f')) {
                        i++;
                        column++;
                    }
                    if (i >= length || (source[i] != '\r' && source[i] != '\n'))
                        break;
                    if (source[i] == '\r' && i + 1 < length && source[i + 1] == '\n')
                        i++;
                    i++;
                    line++;
                    column = 1;
                }
                continue;
            }
        }
        if (ch == '\r' || ch == '\n') {
            if (!oasp_token_push(tokens, OASP_TOKEN_NEWLINE, "\n", 1, line, column))
                return false;
            if (ch == '\r' && i + 1 < length && source[i + 1] == '\n')
                i++;
            i++;
            line++;
            column = 1;
            statement_start = true;
            continue;
        }
        if (ch == '\'') {
            while (i < length && source[i] != '\r' && source[i] != '\n') {
                i++;
                column++;
            }
            continue;
        }
        if (statement_start && (ch == 'r' || ch == 'R') && i + 2 < length && (source[i + 1] == 'e' || source[i + 1] == 'E') &&
            (source[i + 2] == 'm' || source[i + 2] == 'M') &&
            (i + 3 == length || (!oasp_identifier_part((unsigned char)source[i + 3]) && source[i + 3] != '.'))) {
            while (i < length && source[i] != '\r' && source[i] != '\n') {
                i++;
                column++;
            }
            continue;
        }
        statement_start = false;
        size_t token_column = column;
        if (ch == '"') {
            size_t old = i;
            if (!oasp_lex_string(arena, source, length, &i, line, column, tokens))
                return false;
            column += i - old;
            continue;
        }
        if (ch == '[') {
            size_t start = ++i;
            column++;
            while (i < length && source[i] != ']' && source[i] != '\r' && source[i] != '\n') {
                i++;
                column++;
            }
            if (i >= length || source[i] != ']') {
                tokens->error = "unterminated bracketed identifier";
                tokens->error_line = line;
                tokens->error_column = token_column;
                return false;
            }
            if (!oasp_token_push(tokens, OASP_TOKEN_IDENTIFIER, source + start, i - start, line, token_column))
                return false;
            tokens->items[tokens->length - 1].source_width = i - start + 2;
            i++;
            column++;
            continue;
        }
        if (ch == '#') {
            size_t start = ++i;
            column++;
            while (i < length && source[i] != '#' && source[i] != '\r' && source[i] != '\n') {
                i++;
                column++;
            }
            if (i >= length || source[i] != '#') {
                tokens->error = "unterminated date literal";
                tokens->error_line = line;
                tokens->error_column = token_column;
                return false;
            }
            if (!oasp_token_push(tokens, OASP_TOKEN_DATE, source + start, i - start, line, token_column))
                return false;
            i++;
            column++;
            continue;
        }
        if (ch == '&' && i + 2 < length && (source[i + 1] == 'h' || source[i + 1] == 'H') && oasp_hex_digit((unsigned char)source[i + 2])) {
            size_t start = i;
            i += 2;
            while (i < length && oasp_hex_digit((unsigned char)source[i]))
                i++;
            if (i < length && source[i] == '&')
                i++;
            if (!oasp_token_push(tokens, OASP_TOKEN_INTEGER, source + start, i - start, line, token_column))
                return false;
            column += i - start;
            continue;
        }
        if (ch == '&' && i + 1 < length && source[i + 1] >= '0' && source[i + 1] <= '7') {
            size_t start = i++;
            while (i < length && source[i] >= '0' && source[i] <= '7')
                i++;
            if (i < length && source[i] == '&')
                i++;
            if (!oasp_token_push(tokens, OASP_TOKEN_INTEGER, source + start, i - start, line, token_column))
                return false;
            column += i - start;
            continue;
        }
        if (isdigit(ch) || (ch == '.' && i + 1 < length && isdigit((unsigned char)source[i + 1]))) {
            size_t start = i;
            oasp_token_kind kind = OASP_TOKEN_INTEGER;
            if (ch == '.') {
                kind = OASP_TOKEN_FLOAT;
                i++;
            }
            while (i < length && isdigit((unsigned char)source[i]))
                i++;
            if (i < length && source[i] == '.' && i + 1 < length && isdigit((unsigned char)source[i + 1])) {
                kind = OASP_TOKEN_FLOAT;
                i++;
                while (i < length && isdigit((unsigned char)source[i]))
                    i++;
            }
            size_t exponent = i;
            if (oasp_scan_exponent(source, length, &i))
                kind = OASP_TOKEN_FLOAT;
            else
                i = exponent;
            bool pushed = kind == OASP_TOKEN_FLOAT
                              ? oasp_token_push_copy(arena, tokens, kind, source + start, i - start, line, token_column)
                              : oasp_token_push(tokens, kind, source + start, i - start, line, token_column);
            if (!pushed)
                return false;
            column += i - start;
            continue;
        }
        if (oasp_identifier_start(ch)) {
            size_t start = i++;
            while (i < length) {
                if (oasp_identifier_part((unsigned char)source[i])) {
                    i++;
                    continue;
                }
                if (source[i] == '.' && source[i - 1] != '.') {
                    i++;
                    continue;
                }
                break;
            }
            if (!oasp_token_push(tokens, OASP_TOKEN_IDENTIFIER, source + start, i - start, line, token_column))
                return false;
            column += i - start;
            continue;
        }
        oasp_token_kind kind;
        size_t take = 1;
        switch (ch) {
        case '(':
            kind = OASP_TOKEN_LPAREN;
            break;
        case ')':
            kind = OASP_TOKEN_RPAREN;
            break;
        case ',':
            kind = OASP_TOKEN_COMMA;
            break;
        case ':':
            kind = OASP_TOKEN_COLON;
            break;
        case '&':
            kind = OASP_TOKEN_AMP;
            break;
        case '+':
            kind = OASP_TOKEN_PLUS;
            break;
        case '-':
            kind = OASP_TOKEN_MINUS;
            break;
        case '*':
            kind = OASP_TOKEN_STAR;
            break;
        case '/':
            kind = OASP_TOKEN_SLASH;
            break;
        case '\\':
            kind = OASP_TOKEN_BACKSLASH;
            break;
        case '^':
            kind = OASP_TOKEN_CARET;
            break;
        case '.':
            kind = OASP_TOKEN_DOT;
            break;
        case '=':
            if (i + 1 < length && source[i + 1] == '<') {
                kind = OASP_TOKEN_LE;
                take = 2;
            } else if (i + 1 < length && source[i + 1] == '>') {
                kind = OASP_TOKEN_GE;
                take = 2;
            } else
                kind = OASP_TOKEN_EQ;
            break;
        case '<':
            if (i + 1 < length && source[i + 1] == '=') {
                kind = OASP_TOKEN_LE;
                take = 2;
            } else if (i + 1 < length && source[i + 1] == '>') {
                kind = OASP_TOKEN_NE;
                take = 2;
            } else
                kind = OASP_TOKEN_LESS;
            break;
        case '>':
            if (i + 1 < length && source[i + 1] == '=') {
                kind = OASP_TOKEN_GE;
                take = 2;
            } else
                kind = OASP_TOKEN_GREATER;
            break;
        default:
            tokens->error = "unexpected character";
            tokens->error_line = line;
            tokens->error_column = column;
            return false;
        }
        if (!oasp_token_push(tokens, kind, source + i, take, line, token_column))
            return false;
        i += take;
        column += take;
        if (kind == OASP_TOKEN_COLON)
            statement_start = true;
    }
    return oasp_token_push(tokens, OASP_TOKEN_EOF, "", 0, line, column);
}

void oasp_token_list_dispose(oasp_token_list *tokens) {
    if (!tokens)
        return;
    free(tokens->items);
    memset(tokens, 0, sizeof(*tokens));
}

const char *oasp_token_kind_name(oasp_token_kind kind) {
    static const char *names[] = {"eof", "identifier", "string", "integer", "(", ")",  ",",  "=",  "&", ":",     "newline", "+",   "-",
                                  "*",   "/",          "\\",     "<",       ">", "<=", ">=", "<>", ".", "float", "^",       "date"};
    return (size_t)kind < OASP_ARRAY_COUNT(names) ? names[kind] : "unknown";
}
