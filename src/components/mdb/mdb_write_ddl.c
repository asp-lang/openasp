// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "mdb_write_ddl.h"
#include "mdb_write.h"
#include "mdb_write_lock.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <glib.h>
#include <mdbtools.h>

#define OASP_JET4_PAGE_SIZE 4096
#define OASP_JET4_MAX_COLUMNS 255
#define OASP_JET4_TDEF_HEADER_SIZE 63
#define OASP_JET4_COLUMN_SIZE 25
#define OASP_JET4_USAGE_ROW_SIZE 69
#define OASP_JET4_INDEX_ROW_COUNT_SIZE 12
#define OASP_JET4_INDEX_COLUMN_BLOCK_SIZE 52
#define OASP_JET4_INDEX_INFO_BLOCK_SIZE 28
#define OASP_JET4_INDEX_ENTRY_MASK_SIZE 453
#define OASP_JET4_MAX_INDEX_COLUMNS 10
#define OASP_JET4_TABLE_PARENT_ID 0x0f000001U
#define OASP_JET4_TABLE_MAGIC 1625U
#define OASP_JET4_INDEX_MAGIC 1923U

extern ssize_t mdb_write_pg(MdbHandle* mdb, unsigned long pg);

/*
 * This file implements the supported Jet 4 CREATE TABLE subset directly from
 * page-format structures. Parsing is intentionally strict: unsupported DDL is
 * rejected before an exclusive lock or journal is created.
 */
typedef struct EgDdlColumn {
    /* Parsed schema plus offsets assigned during the layout pass. */
    char* name;
    uint8_t type;
    uint16_t length;
    uint8_t flags;
    uint8_t ext_flags;
    uint16_t variable_index;
    uint16_t fixed_offset;
    int variable;
    int long_value;
    int required;
    int primary_key;
    char* default_literal;
    char* default_expression;
} EgDdlColumn;

typedef struct EgDdlTable {
    /* Fixed-size storage bounds the Jet format's 255-column maximum. */
    char* name;
    char* primary_key_name;
    EgDdlColumn columns[OASP_JET4_MAX_COLUMNS];
    size_t column_count;
    size_t variable_count;
    size_t long_value_count;
    int primary_key_column;
} EgDdlTable;

typedef struct EgDdlBuffer {
    /* Growable encoder used for variable-length Jet property records. */
    unsigned char* data;
    size_t length;
    size_t capacity;
} EgDdlBuffer;

typedef struct EgDdlRowSnapshot {
    guint32 page_number;
    guint16 row_number;
    MdbField fields[OASP_JET4_MAX_COLUMNS];
    unsigned char* values[OASP_JET4_MAX_COLUMNS];
    char* literal;
    char* converted_value;
} EgDdlRowSnapshot;

typedef struct EgDdlIndex {
    char* name;
    char* table_name;
    char* column_names[OASP_JET4_MAX_INDEX_COLUMNS];
    size_t column_count;
    int unique;
} EgDdlIndex;

static void oasp_ddl_index_free(EgDdlIndex* index) {
    if (!index) {
        return;
    }
    free(index->name);
    free(index->table_name);
    for (size_t i = 0; i < index->column_count; i++) {
        free(index->column_names[i]);
    }
    memset(index, 0, sizeof(*index));
}

static void oasp_ddl_error(char* error, size_t capacity, const char* message) {
    if (error && capacity) {
        snprintf(error, capacity, "%s", message ? message : "unknown error");
    }
}

static void oasp_ddl_table_free(EgDdlTable* table) {
    size_t i = 0;
    if (!table) {
        return;
    }
    free(table->name);
    free(table->primary_key_name);
    while (i < table->column_count) {
        free(table->columns[i].name);
        free(table->columns[i].default_literal);
        free(table->columns[i].default_expression);
        i++;
    }
    memset(table, 0, sizeof(*table));
}

static void oasp_ddl_rows_free(EgDdlRowSnapshot* rows, size_t count) {
    size_t row = 0;
    while (row < count) {
        size_t column = 0;
        while (column < OASP_JET4_MAX_COLUMNS) {
            free(rows[row].values[column]);
            column++;
        }
        free(rows[row].literal);
        free(rows[row].converted_value);
        row++;
    }
    free(rows);
}

static const char* oasp_ddl_skip_space(const char* cursor) {
    while (cursor && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    return cursor;
}

static int oasp_ddl_is_ident_char(unsigned char ch) {
    return isalnum(ch) || ch == '_' || ch == '$';
}

static int oasp_ddl_keyword(const char* cursor, const char* keyword, const char** after) {
    size_t length = strlen(keyword);
    cursor = oasp_ddl_skip_space(cursor);
    if (strncasecmp(cursor, keyword, length) != 0 || oasp_ddl_is_ident_char((unsigned char)cursor[length])) {
        return 0;
    }
    if (after) {
        *after = cursor + length;
    }
    return 1;
}

static char* oasp_ddl_parse_identifier(const char** cursor, char* error, size_t error_capacity) {
    const char* start;
    const char* end;
    char terminator = '\0';
    char* result;
    size_t length;
    *cursor = oasp_ddl_skip_space(*cursor);
    start = *cursor;
    if (*start == '[') {
        terminator = ']';
        start++;
    } else if (*start == '"' || *start == '`') {
        terminator = *start;
        start++;
    }
    end = start;
    if (terminator) {
        while (*end && *end != terminator) {
            end++;
        }
        if (*end != terminator) {
            oasp_ddl_error(error, error_capacity, "unterminated quoted identifier");
            return NULL;
        }
        *cursor = end + 1;
    } else {
        while (oasp_ddl_is_ident_char((unsigned char)*end)) {
            end++;
        }
        *cursor = end;
    }
    length = (size_t)(end - start);
    if (length == 0 || length > 64 || !g_utf8_validate(start, (gssize)length, NULL)) {
        oasp_ddl_error(error, error_capacity, "identifier must be valid UTF-8 and at most 64 characters");
        return NULL;
    }
    result = (char*)malloc(length + 1);
    if (!result) {
        oasp_ddl_error(error, error_capacity, "out of memory");
        return NULL;
    }
    memcpy(result, start, length);
    result[length] = '\0';
    return result;
}

static int oasp_ddl_parse_uint(const char** cursor, unsigned int* value) {
    unsigned long parsed;
    char* end;
    *cursor = oasp_ddl_skip_space(*cursor);
    errno = 0;
    parsed = strtoul(*cursor, &end, 10);
    if (errno || end == *cursor || parsed > UINT16_MAX) {
        return 0;
    }
    *value = (unsigned int)parsed;
    *cursor = end;
    return 1;
}

static char* oasp_ddl_copy_range(const char* start, const char* end) {
    size_t length = (size_t)(end - start);
    char* value = (char*)malloc(length + 1);
    if (!value) {
        return NULL;
    }
    memcpy(value, start, length);
    value[length] = '\0';
    return value;
}

static int oasp_ddl_parse_index(const char* sql, EgDdlIndex* index, char* error, size_t error_capacity) {
    const char* cursor;
    const char* after;
    if (!sql || !oasp_ddl_keyword(sql, "create", &cursor)) {
        oasp_ddl_error(error, error_capacity, "expected CREATE INDEX");
        return 0;
    }
    if (oasp_ddl_keyword(cursor, "unique", &after)) {
        index->unique = 1;
        cursor = after;
    }
    if (!oasp_ddl_keyword(cursor, "index", &cursor)) {
        oasp_ddl_error(error, error_capacity, "expected INDEX after CREATE");
        return 0;
    }
    index->name = oasp_ddl_parse_identifier(&cursor, error, error_capacity);
    if (!index->name || !oasp_ddl_keyword(cursor, "on", &cursor)) {
        oasp_ddl_error(error, error_capacity, "CREATE INDEX requires ON table (column)");
        return 0;
    }
    index->table_name = oasp_ddl_parse_identifier(&cursor, error, error_capacity);
    cursor = oasp_ddl_skip_space(cursor);
    if (!index->table_name || *cursor++ != '(') {
        oasp_ddl_error(error, error_capacity, "CREATE INDEX requires indexed columns");
        return 0;
    }
    for (;;) {
        if (index->column_count == OASP_JET4_MAX_INDEX_COLUMNS) {
            oasp_ddl_error(error, error_capacity, "Jet indexes support at most 10 columns");
            return 0;
        }
        index->column_names[index->column_count] = oasp_ddl_parse_identifier(&cursor, error, error_capacity);
        if (!index->column_names[index->column_count]) {
            return 0;
        }
        index->column_count++;
        cursor = oasp_ddl_skip_space(cursor);
        if (*cursor == ')') {
            cursor++;
            break;
        }
        if (*cursor++ != ',') {
            oasp_ddl_error(error, error_capacity, "CREATE INDEX columns must be comma-separated ascending identifiers");
            return 0;
        }
    }
    cursor = oasp_ddl_skip_space(cursor);
    if ((*cursor == ';' && *oasp_ddl_skip_space(cursor + 1)) || (*cursor && *cursor != ';')) {
        oasp_ddl_error(error, error_capacity, "CREATE INDEX supports ascending columns only");
        return 0;
    }
    return 1;
}

static int oasp_ddl_set_primary_key(EgDdlTable* table, const char* name, char* error, size_t error_capacity) {
    if (table->primary_key_name) {
        oasp_ddl_error(error, error_capacity, "CREATE TABLE supports only one primary key");
        return 0;
    }
    table->primary_key_name = strdup(name);
    if (!table->primary_key_name) {
        oasp_ddl_error(error, error_capacity, "out of memory");
        return 0;
    }
    return 1;
}

static int oasp_ddl_parse_default(const char** cursor, const char* end, EgDdlColumn* column, char* error, size_t error_capacity) {
    const char* start;
    const char* value_start;
    const char* value_end;
    char* value;
    char quote;
    size_t length;
    size_t out_length = 0;
    *cursor = oasp_ddl_skip_space(*cursor);
    start = *cursor;
    if (start >= end) {
        oasp_ddl_error(error, error_capacity, "DEFAULT requires a literal value");
        return 0;
    }
    quote = *start;
    if (quote == '\'' || quote == '"') {
        const char* scan = start + 1;
        value_start = scan;
        while (scan < end) {
            if (*scan == quote) {
                if (scan + 1 < end && scan[1] == quote) {
                    scan += 2;
                    continue;
                }
                break;
            }
            scan++;
        }
        if (scan >= end || *scan != quote) {
            oasp_ddl_error(error, error_capacity, "unterminated DEFAULT string");
            return 0;
        }
        value_end = scan;
        value = (char*)malloc((size_t)(value_end - value_start) + 2);
        if (!value) {
            oasp_ddl_error(error, error_capacity, "out of memory");
            return 0;
        }
        value[out_length++] = 's';
        scan = value_start;
        while (scan < value_end) {
            value[out_length++] = *scan;
            if (*scan == quote && scan + 1 < value_end && scan[1] == quote) {
                scan++;
            }
            scan++;
        }
        value[out_length] = '\0';
        *cursor = value_end + 1;
    } else {
        const char* scan = start;
        int depth = 0;
        while (scan < end) {
            if (*scan == '(') {
                depth++;
            } else if (*scan == ')' && depth > 0) {
                depth--;
            } else if (depth == 0 && isspace((unsigned char)*scan)) {
                break;
            }
            scan++;
        }
        value_end = scan;
        length = (size_t)(value_end - start);
        if (length == 0) {
            oasp_ddl_error(error, error_capacity, "DEFAULT requires a literal value");
            return 0;
        }
        value = (char*)malloc(length + 2);
        if (!value) {
            oasp_ddl_error(error, error_capacity, "out of memory");
            return 0;
        }
        if ((length == 5 && !strncasecmp(start, "now()", length))
            || (length == 6 && !strncasecmp(start, "date()", length))
            || (length == 9 && !strncasecmp(start, "getdate()", length))) {
            value[0] = 'd';
        } else if ((length == 4 && !strncasecmp(start, "true", length))
                   || (length == 5 && !strncasecmp(start, "false", length))
                   || (length == 4 && !strncasecmp(start, "null", length))
                   || *start == '+' || *start == '-' || isdigit((unsigned char)*start)) {
            value[0] = (length == 4 && !strncasecmp(start, "null", length)) ? 'n' : 'v';
        } else {
            free(value);
            oasp_ddl_error(error, error_capacity, "unsupported DEFAULT expression");
            return 0;
        }
        memcpy(value + 1, start, length);
        value[length + 1] = '\0';
        *cursor = value_end;
    }
    column->default_literal = value;
    column->default_expression = oasp_ddl_copy_range(start, *cursor);
    if (!column->default_expression) {
        free(column->default_literal);
        column->default_literal = NULL;
        oasp_ddl_error(error, error_capacity, "out of memory");
        return 0;
    }
    return 1;
}

static int oasp_ddl_set_type(EgDdlColumn* column, const char* type, unsigned int text_length, char* error, size_t error_capacity) {
    int is_text_type = !strcasecmp(type, "text") || !strcasecmp(type, "varchar") || !strcasecmp(type, "char");
    if (text_length != 0 && !is_text_type) {
        oasp_ddl_error(error, error_capacity, "column length is supported only for TEXT types");
        return 0;
    }
    column->flags = 0x02;
    if (!strcasecmp(type, "bool") || !strcasecmp(type, "boolean") || !strcasecmp(type, "bit") || !strcasecmp(type, "yesno")) {
        column->type = MDB_BOOL;
        column->length = 1;
    } else if (!strcasecmp(type, "byte") || !strcasecmp(type, "tinyint")) {
        column->type = MDB_BYTE;
        column->length = 1;
        column->flags |= 0x01;
    } else if (!strcasecmp(type, "short") || !strcasecmp(type, "smallint")) {
        column->type = MDB_INT;
        column->length = 2;
        column->flags |= 0x01;
    } else if (!strcasecmp(type, "int") || !strcasecmp(type, "integer") || !strcasecmp(type, "long")) {
        column->type = MDB_LONGINT;
        column->length = 4;
        column->flags |= 0x01;
    } else if (!strcasecmp(type, "counter") || !strcasecmp(type, "autoincrement")) {
        column->type = MDB_LONGINT;
        column->length = 4;
        column->flags |= 0x05;
    } else if (!strcasecmp(type, "single") || !strcasecmp(type, "real")) {
        column->type = MDB_FLOAT;
        column->length = 4;
        column->flags |= 0x01;
    } else if (!strcasecmp(type, "double") || !strcasecmp(type, "float")) {
        column->type = MDB_DOUBLE;
        column->length = 8;
        column->flags |= 0x01;
    } else if (!strcasecmp(type, "money") || !strcasecmp(type, "currency")) {
        column->type = MDB_MONEY;
        column->length = 8;
        column->flags |= 0x01;
    } else if (!strcasecmp(type, "date") || !strcasecmp(type, "time") || !strcasecmp(type, "datetime") || !strcasecmp(type, "timestamp")) {
        column->type = MDB_DATETIME;
        column->length = 8;
        column->flags |= 0x01;
    } else if (is_text_type) {
        if (text_length == 0) {
            text_length = 255;
        }
        if (text_length > 255) {
            oasp_ddl_error(error, error_capacity, "Jet 4 TEXT length must be between 1 and 255");
            return 0;
        }
        column->type = MDB_TEXT;
        column->length = (uint16_t)(text_length * 2);
        column->variable = 1;
        column->ext_flags = 0x01;
    } else if (!strcasecmp(type, "memo") || !strcasecmp(type, "longtext")) {
        column->type = MDB_MEMO;
        column->variable = 1;
        column->long_value = 1;
    } else {
        oasp_ddl_error(error, error_capacity, "unsupported Jet 4 column type");
        return 0;
    }
    return 1;
}

static int oasp_ddl_default_integer(const char* text, int64_t minimum, int64_t maximum) {
    char* end = NULL;
    long long value;
    errno = 0;
    value = strtoll(text, &end, 10);
    return errno == 0 && end != text && *end == '\0' && value >= minimum && value <= maximum;
}

static int oasp_ddl_default_number(const char* text) {
    char* end = NULL;
    double value;
    errno = 0;
    value = strtod(text, &end);
    return errno == 0 && end != text && *end == '\0' && isfinite(value);
}

static int oasp_ddl_validate_default(const EgDdlColumn* column, char* error, size_t error_capacity) {
    const char* value;
    int valid = 0;
    if (!column->default_literal || column->default_literal[0] == 'n') {
        return 1;
    }
    value = column->default_literal + 1;
    switch (column->type) {
        case MDB_BOOL:
            valid = column->default_literal[0] == 'v'
                && (!strcasecmp(value, "true") || !strcasecmp(value, "false")
                    || !strcmp(value, "0") || !strcmp(value, "1") || !strcmp(value, "-1"));
            break;
        case MDB_BYTE:
            valid = column->default_literal[0] == 'v' && oasp_ddl_default_integer(value, 0, UINT8_MAX);
            break;
        case MDB_INT:
            valid = column->default_literal[0] == 'v' && oasp_ddl_default_integer(value, INT16_MIN, INT16_MAX);
            break;
        case MDB_LONGINT:
            valid = column->default_literal[0] == 'v' && oasp_ddl_default_integer(value, INT32_MIN, INT32_MAX);
            break;
        case MDB_FLOAT:
        case MDB_DOUBLE:
        case MDB_MONEY:
            valid = column->default_literal[0] == 'v' && oasp_ddl_default_number(value);
            break;
        case MDB_DATETIME:
            valid = column->default_literal[0] == 'd';
            break;
        case MDB_TEXT:
        case MDB_MEMO:
            valid = column->default_literal[0] == 's' && g_utf8_validate(value, -1, NULL);
            break;
        default:
            break;
    }
    if (!valid) {
        oasp_ddl_error(error, error_capacity, "DEFAULT value is incompatible with the Jet column type");
    }
    return valid;
}

static int oasp_ddl_parse_column(const char* start, const char* end, EgDdlTable* table, char* error, size_t error_capacity) {
    EgDdlColumn* column;
    const char* cursor = start;
    const char* type_start;
    char type[32];
    size_t type_length;
    unsigned int text_length = 0;
    size_t i = 0;
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    if (table->column_count >= OASP_JET4_MAX_COLUMNS) {
        oasp_ddl_error(error, error_capacity, "Jet 4 tables support at most 255 columns");
        return 0;
    }
    column = &table->columns[table->column_count];
    column->name = oasp_ddl_parse_identifier(&cursor, error, error_capacity);
    if (!column->name || cursor > end) {
        return 0;
    }
    while (i < table->column_count) {
        if (!strcasecmp(table->columns[i].name, column->name)) {
            oasp_ddl_error(error, error_capacity, "duplicate CREATE TABLE column name");
            return 0;
        }
        i++;
    }
    cursor = oasp_ddl_skip_space(cursor);
    type_start = cursor;
    while (cursor < end && (isalpha((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    type_length = (size_t)(cursor - type_start);
    if (type_length == 0 || type_length >= sizeof(type)) {
        oasp_ddl_error(error, error_capacity, "missing or invalid CREATE TABLE column type");
        return 0;
    }
    memcpy(type, type_start, type_length);
    type[type_length] = '\0';
    cursor = oasp_ddl_skip_space(cursor);
    if (cursor < end && *cursor == '(') {
        cursor++;
        if (!oasp_ddl_parse_uint(&cursor, &text_length)) {
            oasp_ddl_error(error, error_capacity, "invalid CREATE TABLE column length");
            return 0;
        }
        cursor = oasp_ddl_skip_space(cursor);
        if (cursor >= end || *cursor != ')') {
            oasp_ddl_error(error, error_capacity, "invalid CREATE TABLE column length");
            return 0;
        }
        cursor++;
    }
    if (!oasp_ddl_set_type(column, type, text_length, error, error_capacity)) {
        return 0;
    }
    cursor = oasp_ddl_skip_space(cursor);
    while (cursor < end) {
        const char* after;
        if (oasp_ddl_keyword(cursor, "primary", &after) && oasp_ddl_keyword(after, "key", &after)) {
            if (!oasp_ddl_set_primary_key(table, column->name, error, error_capacity)) {
                return 0;
            }
            column->primary_key = 1;
            cursor = oasp_ddl_skip_space(after);
        } else if (oasp_ddl_keyword(cursor, "not", &after) && oasp_ddl_keyword(after, "null", &after)) {
            column->required = 1;
            cursor = oasp_ddl_skip_space(after);
        } else if (oasp_ddl_keyword(cursor, "default", &after)) {
            if (column->default_literal) {
                oasp_ddl_error(error, error_capacity, "duplicate DEFAULT clause");
                return 0;
            }
            cursor = after;
            if (!oasp_ddl_parse_default(&cursor, end, column, error, error_capacity)) {
                return 0;
            }
            cursor = oasp_ddl_skip_space(cursor);
        } else {
            oasp_ddl_error(error, error_capacity, "unsupported CREATE TABLE column constraint or property");
            return 0;
        }
    }
    if (column->primary_key && (column->type != MDB_LONGINT || !(column->flags & 0x04))) {
        oasp_ddl_error(error, error_capacity, "native Jet 4 primary keys currently require an AutoIncrement column");
        return 0;
    }
    if ((column->flags & 0x04) && column->default_literal) {
        oasp_ddl_error(error, error_capacity, "AutoIncrement columns cannot have a DEFAULT value");
        return 0;
    }
    if (column->required && column->default_literal && column->default_literal[0] == 'n') {
        oasp_ddl_error(error, error_capacity, "NOT NULL columns cannot default to NULL");
        return 0;
    }
    if (!oasp_ddl_validate_default(column, error, error_capacity)) {
        return 0;
    }
    if (column->long_value) {
        table->long_value_count++;
    }
    table->column_count++;
    return 1;
}

static int oasp_ddl_parse_table_constraint(const char* start, const char* end, EgDdlTable* table, char* error, size_t error_capacity) {
    const char* cursor = start;
    const char* after;
    char* column_name;
    if (oasp_ddl_keyword(cursor, "constraint", &after)) {
        char* constraint_name;
        cursor = after;
        constraint_name = oasp_ddl_parse_identifier(&cursor, error, error_capacity);
        if (!constraint_name || cursor > end) {
            free(constraint_name);
            return 0;
        }
        free(constraint_name);
    }
    if (!oasp_ddl_keyword(cursor, "primary", &after) || !oasp_ddl_keyword(after, "key", &after)) {
        oasp_ddl_error(error, error_capacity, "only a single-column PRIMARY KEY table constraint is supported");
        return 0;
    }
    cursor = oasp_ddl_skip_space(after);
    if (cursor >= end || *cursor++ != '(') {
        oasp_ddl_error(error, error_capacity, "PRIMARY KEY requires one column");
        return 0;
    }
    column_name = oasp_ddl_parse_identifier(&cursor, error, error_capacity);
    if (!column_name) {
        return 0;
    }
    cursor = oasp_ddl_skip_space(cursor);
    if (cursor >= end || *cursor++ != ')' || oasp_ddl_skip_space(cursor) != end) {
        free(column_name);
        oasp_ddl_error(error, error_capacity, "only a single-column PRIMARY KEY is supported");
        return 0;
    }
    if (!oasp_ddl_set_primary_key(table, column_name, error, error_capacity)) {
        free(column_name);
        return 0;
    }
    free(column_name);
    return 1;
}

static int oasp_ddl_parse_item(const char* start, const char* end, EgDdlTable* table, char* error, size_t error_capacity) {
    const char* cursor = oasp_ddl_skip_space(start);
    if (oasp_ddl_keyword(cursor, "constraint", NULL) || oasp_ddl_keyword(cursor, "primary", NULL)) {
        return oasp_ddl_parse_table_constraint(cursor, end, table, error, error_capacity);
    }
    if (oasp_ddl_keyword(cursor, "unique", NULL) || oasp_ddl_keyword(cursor, "foreign", NULL)) {
        oasp_ddl_error(error, error_capacity, "UNIQUE and FOREIGN KEY constraints are not supported");
        return 0;
    }
    return oasp_ddl_parse_column(start, end, table, error, error_capacity);
}

/*
 * Split the top-level column list while honoring quotes, brackets, and nested
 * type/default parentheses. A second pass assigns fixed and variable slots,
 * which must match the order encoded into the table-definition page.
 */
static int oasp_ddl_parse(const char* sql, EgDdlTable* table, char* error, size_t error_capacity) {
    const char* cursor;
    const char* item_start;
    const char* close;
    int depth = 0;
    char quote = '\0';
    if (!sql || !oasp_ddl_keyword(sql, "create", &cursor) || !oasp_ddl_keyword(cursor, "table", &cursor)) {
        oasp_ddl_error(error, error_capacity, "expected CREATE TABLE");
        return 0;
    }
    table->name = oasp_ddl_parse_identifier(&cursor, error, error_capacity);
    if (!table->name) {
        return 0;
    }
    cursor = oasp_ddl_skip_space(cursor);
    if (*cursor != '(') {
        oasp_ddl_error(error, error_capacity, "CREATE TABLE requires a column list");
        return 0;
    }
    item_start = ++cursor;
    close = NULL;
    while (*cursor) {
        unsigned char ch = (unsigned char)*cursor;
        if (quote) {
            if (ch == (unsigned char)quote) {
                quote = '\0';
            }
        } else if (ch == '\'' || ch == '"' || ch == '`') {
            quote = (char)ch;
        } else if (ch == '[') {
            quote = ']';
        } else if (ch == '(') {
            depth++;
        } else if (ch == ')' && depth > 0) {
            depth--;
        } else if ((ch == ',' || ch == ')') && depth == 0) {
            if (!oasp_ddl_parse_item(item_start, cursor, table, error, error_capacity)) {
                return 0;
            }
            if (ch == ')') {
                close = cursor;
                break;
            }
            item_start = cursor + 1;
        }
        cursor++;
    }
    if (!close || quote || table->column_count == 0) {
        oasp_ddl_error(error, error_capacity, "invalid CREATE TABLE column list");
        return 0;
    }
    cursor = oasp_ddl_skip_space(close + 1);
    if (*cursor == ';') {
        cursor = oasp_ddl_skip_space(cursor + 1);
    }
    if (*cursor) {
        oasp_ddl_error(error, error_capacity, "unexpected text after CREATE TABLE");
        return 0;
    }
    table->primary_key_column = -1;
    if (table->primary_key_name) {
        size_t primary_index = 0;
        while (primary_index < table->column_count
               && strcasecmp(table->columns[primary_index].name, table->primary_key_name)) {
            primary_index++;
        }
        if (primary_index == table->column_count) {
            oasp_ddl_error(error, error_capacity, "PRIMARY KEY column was not found");
            return 0;
        }
        if (table->columns[primary_index].type != MDB_LONGINT || !(table->columns[primary_index].flags & 0x04)) {
            oasp_ddl_error(error, error_capacity, "native Jet 4 primary keys currently require an AutoIncrement column");
            return 0;
        }
        table->columns[primary_index].primary_key = 1;
        table->primary_key_column = (int)primary_index;
    }
    {
        uint16_t fixed_offset = 0;
        uint16_t variable_index = 0;
        uint16_t long_variable_index;
        size_t i = 0;
        while (i < table->column_count) {
            EgDdlColumn* column = &table->columns[i];
            if (column->variable && !column->long_value) {
                column->variable_index = variable_index++;
            } else if (!column->long_value) {
                column->variable_index = variable_index;
            }
            i++;
        }
        long_variable_index = variable_index;
        i = 0;
        while (i < table->column_count) {
            EgDdlColumn* column = &table->columns[i];
            if (column->long_value) {
                column->variable_index = long_variable_index++;
            }
            if (!column->variable && column->type != MDB_BOOL) {
                column->fixed_offset = fixed_offset;
                fixed_offset = (uint16_t)(fixed_offset + column->length);
            }
            i++;
        }
        table->variable_count = long_variable_index;
    }
    return 1;
}

static void oasp_ddl_put_u16(unsigned char* buffer, size_t offset, uint16_t value) {
    buffer[offset] = (unsigned char)(value & 0xff);
    buffer[offset + 1] = (unsigned char)((value >> 8) & 0xff);
}

static void oasp_ddl_put_u24(unsigned char* buffer, size_t offset, uint32_t value) {
    buffer[offset] = (unsigned char)(value & 0xff);
    buffer[offset + 1] = (unsigned char)((value >> 8) & 0xff);
    buffer[offset + 2] = (unsigned char)((value >> 16) & 0xff);
}

static void oasp_ddl_put_u32(unsigned char* buffer, size_t offset, uint32_t value) {
    oasp_ddl_put_u16(buffer, offset, (uint16_t)(value & 0xffff));
    oasp_ddl_put_u16(buffer, offset + 2, (uint16_t)(value >> 16));
}

static int oasp_ddl_utf16le(const char* text, unsigned char** output, uint16_t* byte_length) {
    glong units = 0;
    gunichar2* utf16 = g_utf8_to_utf16(text, -1, NULL, &units, NULL);
    unsigned char* bytes;
    glong i = 0;
    if (!utf16 || units < 0 || units > UINT16_MAX / 2) {
        g_free(utf16);
        return 0;
    }
    bytes = (unsigned char*)malloc((size_t)units * 2);
    if (!bytes && units) {
        g_free(utf16);
        return 0;
    }
    while (i < units) {
        bytes[i * 2] = (unsigned char)(utf16[i] & 0xff);
        bytes[i * 2 + 1] = (unsigned char)((utf16[i] >> 8) & 0xff);
        i++;
    }
    g_free(utf16);
    *output = bytes;
    *byte_length = (uint16_t)(units * 2);
    return 1;
}

static int oasp_ddl_buffer_reserve(EgDdlBuffer* buffer, size_t extra) {
    size_t capacity;
    unsigned char* data;
    if (extra > SIZE_MAX - buffer->length) {
        return 0;
    }
    if (buffer->length + extra <= buffer->capacity) {
        return 1;
    }
    capacity = buffer->capacity ? buffer->capacity : 128;
    while (capacity < buffer->length + extra) {
        if (capacity > SIZE_MAX / 2) {
            return 0;
        }
        capacity *= 2;
    }
    data = (unsigned char*)realloc(buffer->data, capacity);
    if (!data) {
        return 0;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return 1;
}

static int oasp_ddl_buffer_append(EgDdlBuffer* buffer, const void* data, size_t length) {
    if (!oasp_ddl_buffer_reserve(buffer, length)) {
        return 0;
    }
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    return 1;
}

static int oasp_ddl_buffer_u16(EgDdlBuffer* buffer, uint16_t value) {
    unsigned char bytes[2];
    oasp_ddl_put_u16(bytes, 0, value);
    return oasp_ddl_buffer_append(buffer, bytes, sizeof(bytes));
}

static int oasp_ddl_buffer_u32(EgDdlBuffer* buffer, uint32_t value) {
    unsigned char bytes[4];
    oasp_ddl_put_u32(bytes, 0, value);
    return oasp_ddl_buffer_append(buffer, bytes, sizeof(bytes));
}

static int oasp_ddl_buffer_name(EgDdlBuffer* buffer, const char* name) {
    unsigned char* encoded = NULL;
    uint16_t encoded_length = 0;
    int result;
    if (!oasp_ddl_utf16le(name, &encoded, &encoded_length)) {
        return 0;
    }
    result = oasp_ddl_buffer_u16(buffer, encoded_length)
        && oasp_ddl_buffer_append(buffer, encoded, encoded_length);
    free(encoded);
    return result;
}

static int oasp_ddl_buffer_patch_u32(EgDdlBuffer* buffer, size_t offset, uint32_t value) {
    if (offset + 4 > buffer->length) {
        return 0;
    }
    oasp_ddl_put_u32(buffer->data, offset, value);
    return 1;
}

static int oasp_ddl_property_value(EgDdlBuffer* buffer, uint8_t type, uint16_t name_index, const unsigned char* data, uint16_t data_length) {
    size_t start = buffer->length;
    unsigned char flags = 1;
    return oasp_ddl_buffer_u16(buffer, 0)
        && oasp_ddl_buffer_append(buffer, &flags, 1)
        && oasp_ddl_buffer_append(buffer, &type, 1)
        && oasp_ddl_buffer_u16(buffer, name_index)
        && oasp_ddl_buffer_u16(buffer, data_length)
        && oasp_ddl_buffer_append(buffer, data, data_length)
        && (oasp_ddl_put_u16(buffer->data, start, (uint16_t)(buffer->length - start)), 1);
}

static int oasp_ddl_build_properties(const EgDdlTable* table, unsigned char** output, size_t* output_length, char* error, size_t error_capacity) {
    static const unsigned char header[] = {'M', 'R', '2', '\0'};
    EgDdlBuffer buffer;
    int has_default = 0;
    int has_required = 0;
    size_t names_start;
    size_t i = 0;
    memset(&buffer, 0, sizeof(buffer));
    *output = NULL;
    *output_length = 0;
    while (i < table->column_count) {
        has_default |= table->columns[i].default_expression != NULL;
        has_required |= table->columns[i].required;
        i++;
    }
    if (!has_default && !has_required) {
        return 1;
    }
    if (!oasp_ddl_buffer_append(&buffer, header, sizeof(header))) {
        goto oom;
    }
    names_start = buffer.length;
    if (!oasp_ddl_buffer_u32(&buffer, 0) || !oasp_ddl_buffer_u16(&buffer, 0x80)
        || (has_default && !oasp_ddl_buffer_name(&buffer, "DefaultValue"))
        || (has_required && !oasp_ddl_buffer_name(&buffer, "Required"))
        || !oasp_ddl_buffer_patch_u32(&buffer, names_start, (uint32_t)(buffer.length - names_start))) {
        goto oom;
    }
    i = 0;
    while (i < table->column_count) {
        const EgDdlColumn* column = &table->columns[i];
        if (column->default_expression || column->required) {
            size_t block_start = buffer.length;
            size_t map_name_start;
            if (!oasp_ddl_buffer_u32(&buffer, 0) || !oasp_ddl_buffer_u16(&buffer, 1)) {
                goto oom;
            }
            map_name_start = buffer.length;
            if (!oasp_ddl_buffer_u32(&buffer, 0) || !oasp_ddl_buffer_name(&buffer, column->name)
                || !oasp_ddl_buffer_patch_u32(&buffer, map_name_start, (uint32_t)(buffer.length - map_name_start))) {
                goto oom;
            }
            if (column->default_expression) {
                unsigned char* encoded = NULL;
                uint16_t encoded_length = 0;
                if (!oasp_ddl_utf16le(column->default_expression, &encoded, &encoded_length)
                    || !oasp_ddl_property_value(&buffer, MDB_MEMO, 0, encoded, encoded_length)) {
                    free(encoded);
                    goto oom;
                }
                free(encoded);
            }
            if (column->required) {
                unsigned char value = 0xff;
                uint16_t name_index = has_default ? 1 : 0;
                if (!oasp_ddl_property_value(&buffer, MDB_BOOL, name_index, &value, 1)) {
                    goto oom;
                }
            }
            if (!oasp_ddl_buffer_patch_u32(&buffer, block_start, (uint32_t)(buffer.length - block_start))) {
                goto oom;
            }
        }
        i++;
    }
    *output = (unsigned char*)calloc(buffer.length + 12, 1);
    if (!*output) {
        goto oom;
    }
    oasp_ddl_put_u32(*output, 0, UINT32_C(0x80000000) | (uint32_t)buffer.length);
    memcpy(*output + 12, buffer.data, buffer.length);
    *output_length = buffer.length + 12;
    free(buffer.data);
    return 1;

oom:
    free(buffer.data);
    oasp_ddl_error(error, error_capacity, "out of memory while building Jet column properties");
    return 0;
}

static int oasp_ddl_write_page(MdbHandle* mdb, uint32_t page_number, const unsigned char* page) {
    memcpy(mdb->pg_buf, page, OASP_JET4_PAGE_SIZE);
    mdb->cur_pg = page_number;
    return mdb_write_pg(mdb, page_number) == OASP_JET4_PAGE_SIZE;
}

static int oasp_ddl_column_is_indexed(MdbTableDef* table, size_t column_index) {
    unsigned int index_number;
    if (!table || !mdb_read_indices(table)) {
        return 0;
    }
    for (index_number = 0; index_number < table->num_idxs; index_number++) {
        MdbIndex* index = g_ptr_array_index(table->indices, index_number);
        unsigned int key;
        if (!index) {
            continue;
        }
        for (key = 0; key < index->num_keys; key++) {
            if (index->key_col_num[key] == (short)column_index) {
                return 1;
            }
        }
    }
    return 0;
}

static int oasp_ddl_load_data_row(MdbTableDef* table, guint32 page_number, guint16 row_number, int* row_start, size_t* row_size) {
    MdbHandle* mdb;
    uint16_t row_count;
    uint16_t current;
    uint16_t following;
    if (!table || !table->entry || !(mdb = table->entry->mdb) || !row_start || !row_size
        || mdb_read_pg(mdb, page_number) != mdb->fmt->pg_size) {
        return 0;
    }
    row_count = (uint16_t)mdb_get_int16(mdb->pg_buf, mdb->fmt->row_count_offset);
    if (row_number >= row_count) {
        return 0;
    }
    current = (uint16_t)mdb_get_int16(mdb->pg_buf, mdb->fmt->row_count_offset + 2 + row_number * 2);
    if (current & 0x4000) {
        return 0;
    }
    current &= 0x1fff;
    following = row_number == 0 ? (uint16_t)mdb->fmt->pg_size
                                : (uint16_t)(mdb_get_int16(mdb->pg_buf,
                                    mdb->fmt->row_count_offset + 2 + (row_number - 1) * 2) & 0x1fff);
    if (current >= following || following > mdb->fmt->pg_size) {
        return 0;
    }
    *row_start = current;
    *row_size = (size_t)(following - current);
    return 1;
}

static int oasp_ddl_snapshot_rows(MdbTableDef* table, size_t changed_column, const EgDdlColumn* target, EgDdlRowSnapshot** output, size_t* output_count,
                                  char* error, size_t error_capacity) {
    EgDdlRowSnapshot* rows = NULL;
    char* bound_value = NULL;
    int bound_length = 0;
    size_t count = 0;
    size_t cleanup_count = 0;
    if (!table || !target || changed_column >= table->num_cols) {
        oasp_ddl_error(error, error_capacity, "invalid ALTER COLUMN migration state");
        return 0;
    }
    bound_value = (char*)calloc(MDB_BIND_SIZE + 1, 1);
    if (!bound_value || mdb_bind_column(table, (int)changed_column + 1, bound_value, &bound_length) == -1) {
        free(bound_value);
        oasp_ddl_error(error, error_capacity, "unable to bind ALTER COLUMN source values");
        return 0;
    }
    mdb_rewind_table(table);
    while (mdb_fetch_row(table)) {
        MdbField decoded[OASP_JET4_MAX_COLUMNS];
        MdbField normalized[OASP_JET4_MAX_COLUMNS];
        EgDdlRowSnapshot* next;
        int row_start;
        size_t row_size;
        int decoded_count;
        size_t i;
        memset(decoded, 0, sizeof(decoded));
        memset(normalized, 0, sizeof(normalized));
        if (!oasp_ddl_load_data_row(table, table->cur_phys_pg, (guint16)(table->cur_row - 1),
                                    &row_start, &row_size) || row_size == 0) {
            oasp_ddl_error(error, error_capacity, "unable to locate ALTER COLUMN source row");
            goto fail;
        }
        decoded_count = oasp_mdb_decode_jet4_row(table, row_start, row_size, decoded);
        if (decoded_count < 0 || (unsigned int)decoded_count > table->num_cols) {
            oasp_ddl_error(error, error_capacity, "unable to decode ALTER COLUMN source row");
            goto fail;
        }
        for (i = 0; i < table->num_cols; i++) {
            MdbColumn* source = g_ptr_array_index(table->columns, i);
            normalized[i].colnum = (int)i;
            normalized[i].is_fixed = source ? source->is_fixed : 0;
            normalized[i].is_null = 1;
        }
        for (i = 0; i < (size_t)decoded_count; i++) {
            if (decoded[i].colnum < 0 || (unsigned int)decoded[i].colnum >= table->num_cols) {
                oasp_ddl_error(error, error_capacity, "invalid ALTER COLUMN source row layout");
                goto fail;
            }
            normalized[decoded[i].colnum] = decoded[i];
        }
        next = (EgDdlRowSnapshot*)realloc(rows, (count + 1) * sizeof(*rows));
        if (!next) {
            oasp_ddl_error(error, error_capacity, "out of memory while snapshotting ALTER COLUMN rows");
            goto fail;
        }
        rows = next;
        memset(&rows[count], 0, sizeof(rows[count]));
        cleanup_count = count + 1;
        rows[count].page_number = table->cur_phys_pg;
        rows[count].row_number = (guint16)(table->cur_row - 1);
        memcpy(rows[count].fields, normalized, table->num_cols * sizeof(MdbField));
        for (i = 0; i < table->num_cols; i++) {
            if (rows[count].fields[i].siz > 0) {
                rows[count].values[i] = (unsigned char*)malloc((size_t)rows[count].fields[i].siz);
                if (!rows[count].values[i]) {
                    oasp_ddl_error(error, error_capacity, "out of memory while copying ALTER COLUMN row");
                    goto fail;
                }
                memcpy(rows[count].values[i], rows[count].fields[i].value,
                       (size_t)rows[count].fields[i].siz);
                rows[count].fields[i].value = rows[count].values[i];
            }
        }
        if (rows[count].fields[changed_column].is_null) {
            rows[count].literal = strdup("n");
        } else {
            size_t value_length = strlen(bound_value);
            rows[count].literal = (char*)malloc(value_length + 2);
            if (rows[count].literal) {
                rows[count].literal[0] = target->type == MDB_TEXT ? 's' : 'v';
                memcpy(rows[count].literal + 1, bound_value, value_length + 1);
            }
        }
        if (!rows[count].literal) {
            oasp_ddl_error(error, error_capacity, "out of memory while converting ALTER COLUMN value");
            goto fail;
        }
        count++;
        cleanup_count = count;
    }
    free(bound_value);
    *output = rows;
    *output_count = count;
    return 1;

fail:
    free(bound_value);
    oasp_ddl_rows_free(rows, cleanup_count);
    return 0;
}

static int oasp_ddl_rewrite_column_definition(MdbTableDef* table, size_t changed_column, const EgDdlColumn* target, char* error, size_t error_capacity) {
    MdbHandle* mdb = table ? table->entry->mdb : NULL;
    unsigned char* page;
    uint32_t index_data_count;
    size_t column_offset;
    uint16_t old_columns;
    uint16_t fixed_offset = 0;
    uint16_t variable_index = 0;
    uint16_t long_variable_index;
    size_t i;
    if (!mdb || !target || changed_column >= table->num_cols
        || mdb_read_pg(mdb, table->entry->table_pg) != OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "unable to read ALTER COLUMN definition");
        return 0;
    }
    page = mdb->pg_buf;
    old_columns = (uint16_t)mdb_get_int16(page, 45);
    index_data_count = (uint32_t)mdb_get_int32(page, 51);
    column_offset = OASP_JET4_TDEF_HEADER_SIZE + (size_t)index_data_count * OASP_JET4_INDEX_ROW_COUNT_SIZE;
    if (old_columns != table->num_cols || column_offset + (size_t)old_columns * OASP_JET4_COLUMN_SIZE > OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "unsupported ALTER COLUMN table definition");
        return 0;
    }
    for (i = 0; i < table->num_cols; i++) {
        MdbColumn* source = g_ptr_array_index(table->columns, i);
        int variable = i == changed_column ? target->variable : !source->is_fixed;
        int long_value = i == changed_column ? target->long_value : source->col_type == MDB_MEMO;
        size_t definition_offset = column_offset + i * OASP_JET4_COLUMN_SIZE;
        if (variable && !long_value) {
            oasp_ddl_put_u16(page, definition_offset + 7, variable_index++);
        } else if (!long_value) {
            oasp_ddl_put_u16(page, definition_offset + 7, variable_index);
        }
    }
    long_variable_index = variable_index;
    for (i = 0; i < table->num_cols; i++) {
        MdbColumn* source = g_ptr_array_index(table->columns, i);
        int variable = i == changed_column ? target->variable : !source->is_fixed;
        int long_value = i == changed_column ? target->long_value : source->col_type == MDB_MEMO;
        uint16_t length = i == changed_column ? target->length : (uint16_t)source->col_size;
        size_t definition_offset = column_offset + i * OASP_JET4_COLUMN_SIZE;
        if (long_value) {
            oasp_ddl_put_u16(page, definition_offset + 7, long_variable_index++);
            length = 0;
        }
        if (!variable && (i == changed_column ? target->type : source->col_type) != MDB_BOOL) {
            oasp_ddl_put_u16(page, definition_offset + 21, fixed_offset);
            fixed_offset = (uint16_t)(fixed_offset + length);
        }
        if (i == changed_column) {
            page[definition_offset] = target->type;
            page[definition_offset + 15] = target->flags;
            page[definition_offset + 16] = target->ext_flags;
        }
        oasp_ddl_put_u16(page, definition_offset + 23, length);
    }
    oasp_ddl_put_u16(page, 43, long_variable_index);
    if (mdb_write_pg(mdb, table->entry->table_pg) != OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "unable to write ALTER COLUMN definition");
        return 0;
    }
    return 1;
}

static int oasp_ddl_migrate_alter_column(MdbHandle** mdb_pointer, MdbTableDef** table_pointer, const char* db_path, const char* table_name, size_t changed_column,
                                         const EgDdlColumn* target, char* error, size_t error_capacity) {
    MdbHandle* mdb = *mdb_pointer;
    MdbTableDef* table = *table_pointer;
    EgDdlRowSnapshot* rows = NULL;
    size_t row_count = 0;
    size_t row;
    int result = 0;
    if (target->long_value || target->required || target->default_literal || target->primary_key) {
        oasp_ddl_error(error, error_capacity,
                     "ALTER COLUMN does not yet support MEMO, DEFAULT, NOT NULL, or PRIMARY KEY targets");
        return 0;
    }
    if (!oasp_ddl_snapshot_rows(table, changed_column, target, &rows, &row_count, error, error_capacity)
        || !oasp_ddl_rewrite_column_definition(table, changed_column, target, error, error_capacity)) {
        goto cleanup;
    }
    mdb_free_tabledef(table);
    table = NULL;
    mdb_close(mdb);
    mdb = NULL;
    mdb = mdb_open(db_path, MDB_WRITABLE);
    if (!mdb) {
        oasp_ddl_error(error, error_capacity, "unable to reopen ALTER COLUMN MDB file");
        goto cleanup;
    }
    table = mdb_read_table_by_name(mdb, (char*)table_name, MDB_TABLE);
    if (!table) {
        oasp_ddl_error(error, error_capacity, "unable to reopen ALTER COLUMN target table");
        goto cleanup;
    }
    if (!mdb_read_columns(table)) {
        oasp_ddl_error(error, error_capacity, "unable to read rewritten ALTER COLUMN definition");
        goto cleanup;
    }
    for (row = 0; row < row_count; row++) {
        MdbColumn* target_column = g_ptr_array_index(table->columns, changed_column);
        char* converted = NULL;
        if (!target_column
            || !oasp_mdb_convert_literal_field(mdb, target_column, rows[row].literal,
                                             &rows[row].fields[changed_column], &converted)) {
            free(converted);
            oasp_ddl_error(error, error_capacity, "ALTER COLUMN value cannot be converted to the target type");
            goto cleanup;
        }
        rows[row].converted_value = converted;
        if (mdb_read_pg(mdb, rows[row].page_number) != mdb->fmt->pg_size) {
            oasp_ddl_error(error, error_capacity, "unable to load ALTER COLUMN source page");
            goto cleanup;
        }
        table->cur_phys_pg = rows[row].page_number;
        table->cur_row = rows[row].row_number + 1;
        if (!oasp_mdb_replace_fields(table, rows[row].row_number, (int)table->num_cols,
                                   rows[row].fields)) {
            oasp_ddl_error(error, error_capacity, "unable to rewrite ALTER COLUMN row");
            goto cleanup;
        }
    }
    result = 1;

cleanup:
    oasp_ddl_rows_free(rows, row_count);
    *mdb_pointer = mdb;
    *table_pointer = table;
    return result;
}

static int oasp_ddl_mark_global_page_used(MdbHandle* mdb, uint32_t page_number, char* error, size_t error_capacity) {
    uint16_t row_start;
    uint8_t map_type;
    if (mdb_read_pg(mdb, 1) != OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "unable to read Jet global usage map");
        return 0;
    }
    row_start = (uint16_t)mdb_get_int16(mdb->pg_buf, 14);
    map_type = mdb->pg_buf[row_start];
    if (map_type == 0) {
        uint32_t base = (uint32_t)mdb_get_int32(mdb->pg_buf, row_start + 1);
        uint32_t relative;
        if (page_number < base || page_number - base >= 512) {
            oasp_ddl_error(error, error_capacity, "Jet global inline usage map capacity exceeded");
            return 0;
        }
        relative = page_number - base;
        mdb->pg_buf[row_start + 5 + relative / 8] &= (unsigned char)~(1U << (relative % 8));
        if (mdb_write_pg(mdb, 1) != OASP_JET4_PAGE_SIZE) {
            oasp_ddl_error(error, error_capacity, "unable to update Jet global usage map");
            return 0;
        }
        return 1;
    }
    if (map_type == 1) {
        uint32_t pages_per_map = (OASP_JET4_PAGE_SIZE - 4) * 8;
        uint32_t map_index = page_number / pages_per_map;
        uint32_t relative = page_number % pages_per_map;
        uint32_t map_page;
        if (row_start + 1 + (map_index + 1) * 4 > OASP_JET4_PAGE_SIZE) {
            oasp_ddl_error(error, error_capacity, "Jet global reference usage map capacity exceeded");
            return 0;
        }
        map_page = (uint32_t)mdb_get_int32(mdb->pg_buf, row_start + 1 + map_index * 4);
        if (!map_page || mdb_read_pg(mdb, map_page) != OASP_JET4_PAGE_SIZE || mdb->pg_buf[0] != 5) {
            oasp_ddl_error(error, error_capacity, "missing Jet global reference usage map page");
            return 0;
        }
        mdb->pg_buf[4 + relative / 8] &= (unsigned char)~(1U << (relative % 8));
        if (mdb_write_pg(mdb, map_page) != OASP_JET4_PAGE_SIZE) {
            oasp_ddl_error(error, error_capacity, "unable to update Jet global reference usage map");
            return 0;
        }
        return 1;
    }
    oasp_ddl_error(error, error_capacity, "unsupported Jet global usage map type");
    return 0;
}

static int oasp_ddl_extend_file(MdbHandle* mdb, uint32_t first_page, uint32_t page_count, char* error, size_t error_capacity) {
    unsigned char zeros[OASP_JET4_PAGE_SIZE];
    uint32_t i = 0;
    memset(zeros, 0, sizeof(zeros));
    if (fseeko(mdb->f->stream, 0, SEEK_END) != 0) {
        oasp_ddl_error(error, error_capacity, "unable to seek to end of MDB file");
        return 0;
    }
    while (i < page_count) {
        if (fwrite(zeros, 1, sizeof(zeros), mdb->f->stream) != sizeof(zeros)) {
            oasp_ddl_error(error, error_capacity, "unable to extend MDB file");
            return 0;
        }
        i++;
    }
    if (fflush(mdb->f->stream) != 0) {
        oasp_ddl_error(error, error_capacity, "unable to flush extended MDB file");
        return 0;
    }
    i = 0;
    while (i < page_count) {
        if (!oasp_ddl_mark_global_page_used(mdb, first_page + i, error, error_capacity)) {
            return 0;
        }
        i++;
    }
    return 1;
}

/*
 * Encode one Jet 4 table-definition page. All offsets are little-endian and
 * derived before writing so an oversized definition fails without touching
 * disk. Primary-key metadata is emitted only for the supported AutoNumber form.
 */
static int oasp_ddl_write_tdef(MdbHandle* mdb, const EgDdlTable* table, uint32_t tdef_page, uint32_t usage_page, uint32_t root_page,
                               char* error, size_t error_capacity) {
    unsigned char page[OASP_JET4_PAGE_SIZE];
    size_t total_size = OASP_JET4_TDEF_HEADER_SIZE + table->column_count * OASP_JET4_COLUMN_SIZE + 2;
    size_t position = OASP_JET4_TDEF_HEADER_SIZE
        + (table->primary_key_column >= 0 ? OASP_JET4_INDEX_ROW_COUNT_SIZE : 0);
    size_t i = 0;
    memset(page, 0, sizeof(page));
    while (i < table->column_count) {
        unsigned char* name = NULL;
        uint16_t name_length = 0;
        if (!oasp_ddl_utf16le(table->columns[i].name, &name, &name_length)) {
            oasp_ddl_error(error, error_capacity, "unable to encode Jet column name");
            return 0;
        }
        total_size += 2 + name_length;
        free(name);
        i++;
    }
    total_size += table->long_value_count * 10;
    if (table->primary_key_column >= 0) {
        unsigned char* index_name = NULL;
        uint16_t index_name_length = 0;
        if (!oasp_ddl_utf16le("PrimaryKey", &index_name, &index_name_length)) {
            oasp_ddl_error(error, error_capacity, "unable to encode Jet primary key name");
            return 0;
        }
        total_size += OASP_JET4_INDEX_ROW_COUNT_SIZE + OASP_JET4_INDEX_COLUMN_BLOCK_SIZE
            + OASP_JET4_INDEX_INFO_BLOCK_SIZE + 2 + index_name_length;
        free(index_name);
    }
    if (total_size > OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "Jet table definition exceeds one page");
        return 0;
    }
    page[0] = 2;
    page[1] = 1;
    oasp_ddl_put_u16(page, 2, (uint16_t)(OASP_JET4_PAGE_SIZE - total_size - 8));
    oasp_ddl_put_u32(page, 8, (uint32_t)total_size);
    oasp_ddl_put_u32(page, 12, OASP_JET4_TABLE_MAGIC);
    page[24] = 1;
    page[40] = 0x4e;
    oasp_ddl_put_u16(page, 41, (uint16_t)table->column_count);
    oasp_ddl_put_u16(page, 43, (uint16_t)table->variable_count);
    oasp_ddl_put_u16(page, 45, (uint16_t)table->column_count);
    oasp_ddl_put_u32(page, 47, table->primary_key_column >= 0 ? 1 : 0);
    oasp_ddl_put_u32(page, 51, table->primary_key_column >= 0 ? 1 : 0);
    page[55] = 0;
    oasp_ddl_put_u24(page, 56, usage_page);
    page[59] = 1;
    oasp_ddl_put_u24(page, 60, usage_page);
    i = 0;
    while (i < table->column_count) {
        const EgDdlColumn* column = &table->columns[i];
        page[position] = column->type;
        oasp_ddl_put_u32(page, position + 1, OASP_JET4_TABLE_MAGIC);
        oasp_ddl_put_u16(page, position + 5, (uint16_t)i);
        oasp_ddl_put_u16(page, position + 7, column->variable_index);
        oasp_ddl_put_u16(page, position + 9, (uint16_t)i);
        if (column->type == MDB_TEXT || column->type == MDB_MEMO) {
            oasp_ddl_put_u16(page, position + 11, 0x0409);
        }
        page[position + 15] = column->flags;
        page[position + 16] = column->ext_flags;
        oasp_ddl_put_u16(page, position + 21, column->fixed_offset);
        oasp_ddl_put_u16(page, position + 23, column->long_value ? 0 : column->length);
        position += OASP_JET4_COLUMN_SIZE;
        i++;
    }
    i = 0;
    while (i < table->column_count) {
        unsigned char* name = NULL;
        uint16_t name_length = 0;
        if (!oasp_ddl_utf16le(table->columns[i].name, &name, &name_length)) {
            oasp_ddl_error(error, error_capacity, "unable to encode Jet column name");
            return 0;
        }
        oasp_ddl_put_u16(page, position, name_length);
        memcpy(page + position + 2, name, name_length);
        position += 2 + name_length;
        free(name);
        i++;
    }
    if (table->primary_key_column >= 0) {
        unsigned char* name = NULL;
        uint16_t name_length = 0;
        size_t key = 0;
        oasp_ddl_put_u32(page, position, OASP_JET4_INDEX_MAGIC);
        position += 4;
        while (key < 10) {
            oasp_ddl_put_u16(page, position, key == 0 ? (uint16_t)table->primary_key_column : UINT16_MAX);
            page[position + 2] = key == 0 ? 1 : 0;
            position += 3;
            key++;
        }
        page[position++] = 2;
        oasp_ddl_put_u24(page, position, usage_page);
        position += 3;
        oasp_ddl_put_u32(page, position, root_page);
        position += 8;
        page[position++] = 0x89;
        position += 5;

        oasp_ddl_put_u32(page, position, OASP_JET4_TABLE_MAGIC);
        position += 4;
        oasp_ddl_put_u32(page, position, 0);
        position += 4;
        oasp_ddl_put_u32(page, position, 0);
        position += 4;
        page[position++] = 0;
        oasp_ddl_put_u32(page, position, UINT32_MAX);
        position += 4;
        oasp_ddl_put_u32(page, position, 0);
        position += 4;
        page[position++] = 0;
        page[position++] = 0;
        page[position++] = 1;
        position += 4;

        if (!oasp_ddl_utf16le("PrimaryKey", &name, &name_length)) {
            oasp_ddl_error(error, error_capacity, "unable to encode Jet primary key name");
            return 0;
        }
        oasp_ddl_put_u16(page, position, name_length);
        memcpy(page + position + 2, name, name_length);
        position += 2 + name_length;
        free(name);
    }
    {
        uint8_t usage_row = table->primary_key_column >= 0 ? 3 : 2;
        i = 0;
        while (i < table->column_count) {
            if (table->columns[i].long_value) {
                oasp_ddl_put_u16(page, position, (uint16_t)i);
                page[position + 2] = usage_row++;
                oasp_ddl_put_u24(page, position + 3, (usage_page << 8) | (uint32_t)(usage_row - 1));
                page[position + 6] = usage_row++;
                oasp_ddl_put_u24(page, position + 7, (usage_page << 8) | (uint32_t)(usage_row - 1));
                position += 10;
            }
            i++;
        }
    }
    page[position++] = 0xff;
    page[position++] = 0xff;
    if (position != total_size || !oasp_ddl_write_page(mdb, tdef_page, page)) {
        oasp_ddl_error(error, error_capacity, "unable to write Jet table definition");
        return 0;
    }
    return 1;
}

static int oasp_ddl_write_usage_page(MdbHandle* mdb, const EgDdlTable* table, uint32_t usage_page, uint32_t root_page, uint32_t data_page,
                                     char* error, size_t error_capacity) {
    unsigned char page[OASP_JET4_PAGE_SIZE];
    size_t row_count = 2 + (table->primary_key_column >= 0 ? 1 : 0) + table->long_value_count * 2;
    size_t row = 0;
    memset(page, 0, sizeof(page));
    page[0] = 1;
    page[1] = 1;
    oasp_ddl_put_u16(page, 2, (uint16_t)(OASP_JET4_PAGE_SIZE - 14 - row_count * (OASP_JET4_USAGE_ROW_SIZE + 2)));
    oasp_ddl_put_u16(page, 12, (uint16_t)row_count);
    while (row < row_count) {
        size_t start = OASP_JET4_PAGE_SIZE - (row + 1) * OASP_JET4_USAGE_ROW_SIZE;
        oasp_ddl_put_u16(page, 14 + row * 2, (uint16_t)start);
        if (row < 2) {
            oasp_ddl_put_u32(page, start + 1, data_page);
            page[start + 5] = 1;
        } else if (row == 2 && table->primary_key_column >= 0) {
            oasp_ddl_put_u32(page, start + 1, root_page);
            page[start + 5] = 1;
        }
        row++;
    }
    if (!oasp_ddl_write_page(mdb, usage_page, page)) {
        oasp_ddl_error(error, error_capacity, "unable to write Jet table usage map");
        return 0;
    }
    return 1;
}

static int oasp_ddl_write_index_root_page(MdbHandle* mdb, uint32_t root_page, uint32_t tdef_page, char* error, size_t error_capacity) {
    unsigned char page[OASP_JET4_PAGE_SIZE];
    memset(page, 0, sizeof(page));
    page[0] = 4;
    page[1] = 1;
    oasp_ddl_put_u16(page, 2, (uint16_t)(OASP_JET4_PAGE_SIZE - 27 - OASP_JET4_INDEX_ENTRY_MASK_SIZE));
    oasp_ddl_put_u32(page, 4, tdef_page);
    if (!oasp_ddl_write_page(mdb, root_page, page)) {
        oasp_ddl_error(error, error_capacity, "unable to write Jet primary key root page");
        return 0;
    }
    return 1;
}

static int oasp_ddl_write_data_page(MdbHandle* mdb, uint32_t data_page, uint32_t tdef_page, char* error, size_t error_capacity) {
    unsigned char page[OASP_JET4_PAGE_SIZE];
    memset(page, 0, sizeof(page));
    page[0] = 1;
    page[1] = 1;
    oasp_ddl_put_u16(page, 2, OASP_JET4_PAGE_SIZE - 16);
    oasp_ddl_put_u32(page, 4, tdef_page);
    if (!oasp_ddl_write_page(mdb, data_page, page)) {
        oasp_ddl_error(error, error_capacity, "unable to write initial Jet data page");
        return 0;
    }
    return 1;
}

static int oasp_ddl_set_usage_map_bit(MdbHandle* mdb, uint32_t packed_page_row, uint32_t page_number, unsigned char* cached_map, size_t cached_size,
                                      char* error, size_t error_capacity) {
    uint32_t map_page_number = packed_page_row >> 8;
    uint32_t row_number = packed_page_row & 0xff;
    uint16_t row_count;
    uint16_t row_start;
    uint8_t map_type;
    if (!map_page_number || mdb_read_pg(mdb, map_page_number) != OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "unable to read Jet table usage map");
        return 0;
    }
    row_count = (uint16_t)mdb_get_int16(mdb->pg_buf, 12);
    if (row_number >= row_count) {
        oasp_ddl_error(error, error_capacity, "invalid Jet table usage map row");
        return 0;
    }
    row_start = (uint16_t)(mdb_get_int16(mdb->pg_buf, 14 + row_number * 2) & 0x1fff);
    map_type = mdb->pg_buf[row_start];
    if (map_type == 0) {
        uint32_t base = (uint32_t)mdb_get_int32(mdb->pg_buf, row_start + 1);
        uint32_t relative;
        if (page_number < base || page_number - base >= 512) {
            oasp_ddl_error(error, error_capacity, "Jet table inline usage map capacity exceeded");
            return 0;
        }
        relative = page_number - base;
        mdb->pg_buf[row_start + 5 + relative / 8] |= (unsigned char)(1U << (relative % 8));
        if (cached_map && cached_size > 5 + relative / 8) {
            cached_map[5 + relative / 8] |= (unsigned char)(1U << (relative % 8));
        }
        if (mdb_write_pg(mdb, map_page_number) != OASP_JET4_PAGE_SIZE) {
            oasp_ddl_error(error, error_capacity, "unable to update Jet table inline usage map");
            return 0;
        }
        return 1;
    }
    if (map_type == 1) {
        uint32_t pages_per_map = (OASP_JET4_PAGE_SIZE - 4) * 8;
        uint32_t map_index = page_number / pages_per_map;
        uint32_t relative = page_number % pages_per_map;
        uint32_t reference_page;
        if (row_start + 1 + (map_index + 1) * 4 > OASP_JET4_PAGE_SIZE) {
            oasp_ddl_error(error, error_capacity, "Jet table reference usage map capacity exceeded");
            return 0;
        }
        reference_page = (uint32_t)mdb_get_int32(mdb->pg_buf, row_start + 1 + map_index * 4);
        if (!reference_page || mdb_read_pg(mdb, reference_page) != OASP_JET4_PAGE_SIZE || mdb->pg_buf[0] != 5) {
            oasp_ddl_error(error, error_capacity, "missing Jet table reference usage map page");
            return 0;
        }
        mdb->pg_buf[4 + relative / 8] |= (unsigned char)(1U << (relative % 8));
        if (mdb_write_pg(mdb, reference_page) != OASP_JET4_PAGE_SIZE) {
            oasp_ddl_error(error, error_capacity, "unable to update Jet table reference usage map");
            return 0;
        }
        return 1;
    }
    oasp_ddl_error(error, error_capacity, "unsupported Jet table usage map type");
    return 0;
}

/*
 * Append and register a data page for an existing table. The caller must hold
 * the MDB write lock and an active journal transaction.
 */
int oasp_mdb_append_table_data_page(MdbTableDef* table, char* error, size_t error_capacity) {
    MdbHandle* mdb;
    struct stat status;
    uint32_t page_number;
    uint32_t owned_map;
    uint32_t free_map;
    if (!table || !table->entry || !(mdb = table->entry->mdb) || !mdb->f || !mdb->f->writable) {
        oasp_ddl_error(error, error_capacity, "invalid writable MDB table");
        return 0;
    }
    if (mdb->f->jet_version != MDB_VER_JET4 || fstat(fileno(mdb->f->stream), &status) != 0
        || status.st_size <= 0 || status.st_size % OASP_JET4_PAGE_SIZE != 0) {
        oasp_ddl_error(error, error_capacity, "native page allocation supports Jet 4 MDB files only");
        return 0;
    }
    page_number = (uint32_t)(status.st_size / OASP_JET4_PAGE_SIZE);
    if (mdb_read_pg(mdb, table->entry->table_pg) != OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "unable to read Jet table definition");
        return 0;
    }
    owned_map = (uint32_t)mdb_get_int32(mdb->pg_buf, 55);
    free_map = (uint32_t)mdb_get_int32(mdb->pg_buf, 59);
    if (!oasp_ddl_extend_file(mdb, page_number, 1, error, error_capacity)
        || !oasp_ddl_write_data_page(mdb, page_number, (uint32_t)table->entry->table_pg, error, error_capacity)
        || !oasp_ddl_set_usage_map_bit(mdb, owned_map, page_number, table->usage_map, table->map_sz, error, error_capacity)
        || !oasp_ddl_set_usage_map_bit(mdb, free_map, page_number, table->free_usage_map, table->freemap_sz, error, error_capacity)) {
        return 0;
    }
    return 1;
}

int oasp_mdb_append_index_page(MdbTableDef* table, MdbIndex* index, unsigned int* page_number, char* error, size_t error_capacity) {
    MdbHandle* mdb;
    struct stat status;
    uint32_t allocated_page;
    uint32_t index_map;
    size_t index_position;
    if (!table || !index || !page_number || !table->entry || !(mdb = table->entry->mdb)
        || !mdb->f || !mdb->f->writable || index->index_num < 0) {
        oasp_ddl_error(error, error_capacity, "invalid writable MDB index");
        return 0;
    }
    if (mdb->f->jet_version != MDB_VER_JET4 || fstat(fileno(mdb->f->stream), &status) != 0
        || status.st_size <= 0 || status.st_size % OASP_JET4_PAGE_SIZE != 0) {
        oasp_ddl_error(error, error_capacity, "native index allocation supports Jet 4 MDB files only");
        return 0;
    }
    index_position = (size_t)table->index_start + (size_t)index->index_num * OASP_JET4_INDEX_COLUMN_BLOCK_SIZE;
    if (index_position + OASP_JET4_INDEX_COLUMN_BLOCK_SIZE > OASP_JET4_PAGE_SIZE
        || mdb_read_pg(mdb, table->entry->table_pg) != OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "unable to read Jet index definition");
        return 0;
    }
    index_map = (uint32_t)mdb_get_int32(mdb->pg_buf, (unsigned int)index_position + 34);
    allocated_page = (uint32_t)(status.st_size / OASP_JET4_PAGE_SIZE);
    if (!oasp_ddl_extend_file(mdb, allocated_page, 1, error, error_capacity)
        || !oasp_ddl_set_usage_map_bit(mdb, index_map, allocated_page, NULL, 0, error, error_capacity)) {
        return 0;
    }
    *page_number = allocated_page;
    return 1;
}

/*
 * Long values have dedicated usage-map rows encoded after the table definition.
 * Keep LVAL pages out of the table's data-page map: mdbtools would otherwise
 * attempt to decode them as ordinary records during a table scan.
 */
int oasp_mdb_store_long_value(MdbTableDef* table, size_t column_index, const unsigned char* value, size_t length, unsigned char descriptor[12],
                              char* error, size_t error_capacity) {
    MdbHandle* mdb;
    struct stat status;
    unsigned char page[OASP_JET4_PAGE_SIZE];
    uint32_t index_count;
    uint16_t column_count;
    uint32_t page_number;
    uint32_t owned_map = 0;
    size_t position;
    if (!table || !value || !descriptor || column_index >= table->num_cols || !(mdb = table->entry->mdb)
        || !mdb->f || !mdb->f->writable || length > OASP_JET4_PAGE_SIZE - 16
        || mdb->f->jet_version != MDB_VER_JET4 || fstat(fileno(mdb->f->stream), &status) != 0) {
        oasp_ddl_error(error, error_capacity, "invalid Jet long-value allocation");
        return 0;
    }
    if (mdb_read_pg(mdb, table->entry->table_pg) != OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "unable to read Jet long-value definition");
        return 0;
    }
    index_count = (uint32_t)mdb_get_int32(mdb->pg_buf, 51);
    column_count = (uint16_t)mdb_get_int16(mdb->pg_buf, 45);
    position = OASP_JET4_TDEF_HEADER_SIZE + (size_t)index_count * OASP_JET4_INDEX_ROW_COUNT_SIZE
        + (size_t)column_count * OASP_JET4_COLUMN_SIZE;
    for (size_t i = 0; i < column_count; i++) {
        if (position + 2 > OASP_JET4_PAGE_SIZE) {
            oasp_ddl_error(error, error_capacity, "corrupt Jet long-value column names");
            return 0;
        }
        position += 2 + (uint16_t)mdb_get_int16(mdb->pg_buf, (unsigned int)position);
    }
    position += (size_t)index_count * OASP_JET4_INDEX_COLUMN_BLOCK_SIZE;
    for (size_t i = 0; i < index_count; i++) {
        if (position + OASP_JET4_INDEX_INFO_BLOCK_SIZE + 2 > OASP_JET4_PAGE_SIZE) {
            oasp_ddl_error(error, error_capacity, "corrupt Jet index definition");
            return 0;
        }
        position += OASP_JET4_INDEX_INFO_BLOCK_SIZE + 2
            + (uint16_t)mdb_get_int16(mdb->pg_buf, (unsigned int)(position + OASP_JET4_INDEX_INFO_BLOCK_SIZE));
    }
    while (position + 10 <= OASP_JET4_PAGE_SIZE && position < OASP_JET4_PAGE_SIZE) {
        uint16_t mapped_column = (uint16_t)mdb_get_int16(mdb->pg_buf, (unsigned int)position);
        if (mapped_column == UINT16_MAX)
            break;
        if (mapped_column == column_index) {
            owned_map = (uint32_t)mdb->pg_buf[position + 3]
                | ((uint32_t)mdb->pg_buf[position + 4] << 8)
                | ((uint32_t)mdb->pg_buf[position + 5] << 16);
            page_number = (uint32_t)(status.st_size / OASP_JET4_PAGE_SIZE);
            memset(page, 0, sizeof(page));
            page[0] = 1;
            page[1] = 1;
            oasp_ddl_put_u16(page, 2, (uint16_t)(OASP_JET4_PAGE_SIZE - 16 - length));
            oasp_ddl_put_u32(page, 4, UINT32_C(0x4c41564c));
            oasp_ddl_put_u16(page, 12, 1);
            oasp_ddl_put_u16(page, 14, (uint16_t)(OASP_JET4_PAGE_SIZE - length));
            memcpy(page + OASP_JET4_PAGE_SIZE - length, value, length);
            /*
             * Legacy Jet 4 tables omit the per-column LVAL usage-map rows.
             * Their LVAL pages remain outside the table data-page map and are
             * reached only through the MEMO descriptor, so appending a page is
             * sufficient. Tables created by this engine retain explicit
             * ownership tracking for DDL cleanup.
             */
            if (!oasp_ddl_extend_file(mdb, page_number, 1, error, error_capacity)
                || !oasp_ddl_write_page(mdb, page_number, page)
                || (owned_map
                    && !oasp_ddl_set_usage_map_bit(mdb, owned_map, page_number, NULL, 0, error, error_capacity))) {
                return 0;
            }
            memset(descriptor, 0, 12);
            oasp_ddl_put_u24(descriptor, 0, (uint32_t)length);
            descriptor[3] = 0x40;
            oasp_ddl_put_u32(descriptor, 4, page_number << 8);
            return 1;
        }
        position += 10;
    }
    /*
     * Jet 4 table definitions created by Access can end directly after the
     * index metadata. Unlike OpenASP-created tables, they have no dedicated
     * long-value usage-map records. Their LVAL pages are intentionally not
     * listed in the table's data-page map, so reuse the standard single-page
     * LVAL representation without a map update.
     */
    page_number = (uint32_t)(status.st_size / OASP_JET4_PAGE_SIZE);
    memset(page, 0, sizeof(page));
    page[0] = 1;
    page[1] = 1;
    oasp_ddl_put_u16(page, 2, (uint16_t)(OASP_JET4_PAGE_SIZE - 16 - length));
    oasp_ddl_put_u32(page, 4, UINT32_C(0x4c41564c));
    oasp_ddl_put_u16(page, 12, 1);
    oasp_ddl_put_u16(page, 14, (uint16_t)(OASP_JET4_PAGE_SIZE - length));
    memcpy(page + OASP_JET4_PAGE_SIZE - length, value, length);
    if (!oasp_ddl_extend_file(mdb, page_number, 1, error, error_capacity)
        || !oasp_ddl_write_page(mdb, page_number, page)) {
        return 0;
    }
    memset(descriptor, 0, 12);
    oasp_ddl_put_u24(descriptor, 0, (uint32_t)length);
    descriptor[3] = 0x40;
    oasp_ddl_put_u32(descriptor, 4, page_number << 8);
    return 1;

    oasp_ddl_error(error, error_capacity, "MEMO column has no Jet long-value usage map");
    return 0;
}

static int oasp_ddl_catalog_insert(MdbHandle* mdb, const EgDdlTable* ddl, uint32_t tdef_page, char* error, size_t error_capacity) {
    MdbTableDef* table = mdb_read_table_by_name(mdb, (char*)"MSysObjects", MDB_TABLE);
    MdbField fields[OASP_JET4_MAX_COLUMNS];
    void* values[OASP_JET4_MAX_COLUMNS];
    unsigned int i = 0;
    int result = 0;
    unsigned char* properties = NULL;
    size_t properties_length = 0;
    memset(fields, 0, sizeof(fields));
    memset(values, 0, sizeof(values));
    if (!table || !mdb_read_columns(table) || table->num_cols > OASP_JET4_MAX_COLUMNS) {
        oasp_ddl_error(error, error_capacity, "unable to read MSysObjects metadata");
        goto cleanup;
    }
    if (!mdb_read_indices(table)) {
        table->num_idxs = 0;
        table->num_real_idxs = 0;
        table->indices = g_ptr_array_new();
    }
    if (!oasp_ddl_build_properties(ddl, &properties, &properties_length, error, error_capacity)) {
        goto cleanup;
    }
    while (i < table->num_cols) {
        MdbColumn* column = g_ptr_array_index(table->columns, i);
        fields[i].colnum = (int)i;
        fields[i].is_fixed = column->is_fixed;
        fields[i].is_null = 1;
        if (!strcasecmp(column->name, "Id") || !strcasecmp(column->name, "ParentId") || !strcasecmp(column->name, "Flags")) {
            uint32_t value = !strcasecmp(column->name, "Id") ? tdef_page : (!strcasecmp(column->name, "ParentId") ? OASP_JET4_TABLE_PARENT_ID : 0);
            values[i] = calloc(1, 4);
            if (values[i]) {
                oasp_ddl_put_u32(values[i], 0, value);
                fields[i].value = values[i];
                fields[i].siz = 4;
                fields[i].is_null = 0;
            }
        } else if (!strcasecmp(column->name, "Type")) {
            values[i] = calloc(1, 2);
            if (values[i]) {
                oasp_ddl_put_u16(values[i], 0, 1);
                fields[i].value = values[i];
                fields[i].siz = 2;
                fields[i].is_null = 0;
            }
        } else if (!strcasecmp(column->name, "Name")) {
            unsigned char* encoded = NULL;
            uint16_t encoded_length = 0;
            if (oasp_ddl_utf16le(ddl->name, &encoded, &encoded_length)) {
                values[i] = encoded;
                fields[i].value = encoded;
                fields[i].siz = encoded_length;
                fields[i].is_null = 0;
            }
        } else if (!strcasecmp(column->name, "DateCreate") || !strcasecmp(column->name, "DateUpdate")) {
            double date_value;
            struct tm local;
            time_t now = time(NULL);
            localtime_r(&now, &local);
            mdb_tm_to_date(&local, &date_value);
            values[i] = malloc(sizeof(date_value));
            if (values[i]) {
                memcpy(values[i], &date_value, sizeof(date_value));
                fields[i].value = values[i];
                fields[i].siz = sizeof(date_value);
                fields[i].is_null = 0;
            }
        } else if (!strcasecmp(column->name, "LvProp") && properties) {
            values[i] = properties;
            properties = NULL;
            fields[i].value = values[i];
            fields[i].siz = (int)properties_length;
            fields[i].is_null = 0;
        }
        if (!fields[i].is_null && !values[i]) {
            oasp_ddl_error(error, error_capacity, "out of memory while building MSysObjects row");
            goto cleanup;
        }
        i++;
    }
    result = mdb_insert_row(table, (int)table->num_cols, fields);
    if (result != 1) {
        oasp_ddl_error(error, error_capacity, "unable to insert Jet system catalog row");
        result = 0;
    }

cleanup:
    free(properties);
    i = 0;
    while (i < OASP_JET4_MAX_COLUMNS) {
        free(values[i]);
        i++;
    }
    if (table) {
        mdb_free_tabledef(table);
    }
    return result;
}

/*
 * Top-level DDL transaction. Parse first, then lock, verify Jet 4 geometry,
 * journal, append all pages/catalog rows, flush, and commit. Every failure after
 * begin rolls back to the byte-for-byte before-image.
 */
int oasp_mdb_create_table_file(const char* db_path, const char* sql, char* error, size_t error_capacity) {
    EgDdlTable ddl;
    MdbHandle* mdb = NULL;
    MdbTableDef* existing = NULL;
    struct stat status;
    uint32_t tdef_page;
    uint32_t usage_page;
    uint32_t root_page;
    uint32_t data_page;
    uint32_t page_count;
    OaspMdbLock lock;
    int result = 0;
    memset(&ddl, 0, sizeof(ddl));
    memset(&lock, 0, sizeof(lock));
    if (!db_path || !*db_path || !sql || !*sql) {
        oasp_ddl_error(error, error_capacity, "invalid MDB CREATE TABLE arguments");
        return 0;
    }
    if (!oasp_ddl_parse(sql, &ddl, error, error_capacity)) {
        goto cleanup;
    }
    if (!oasp_mdb_lock_acquire(db_path, 1, &lock, error, error_capacity)) {
        goto cleanup;
    }
    if (stat(db_path, &status) != 0 || status.st_size <= 0 || status.st_size % OASP_JET4_PAGE_SIZE != 0) {
        oasp_ddl_error(error, error_capacity, "MDB file size is not Jet 4 page aligned");
        goto cleanup;
    }
    mdb = mdb_open(db_path, MDB_WRITABLE);
    if (!mdb) {
        oasp_ddl_error(error, error_capacity, "unable to open MDB file for writing");
        goto cleanup;
    }
    if (mdb->f->jet_version != MDB_VER_JET4 || mdb->fmt->pg_size != OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "native CREATE TABLE supports Jet 4 MDB files only");
        goto cleanup;
    }
    existing = mdb_read_table_by_name(mdb, ddl.name, MDB_TABLE);
    if (existing) {
        oasp_ddl_error(error, error_capacity, "MDB table already exists");
        goto cleanup;
    }
    if (!oasp_mdb_transaction_begin(&lock, error, error_capacity)) {
        goto cleanup;
    }
    tdef_page = (uint32_t)(status.st_size / OASP_JET4_PAGE_SIZE);
    usage_page = tdef_page + 1;
    root_page = ddl.primary_key_column >= 0 ? tdef_page + 2 : 0;
    data_page = tdef_page + (ddl.primary_key_column >= 0 ? 3 : 2);
    page_count = ddl.primary_key_column >= 0 ? 4 : 3;
    if (!oasp_ddl_extend_file(mdb, tdef_page, page_count, error, error_capacity)
        || !oasp_ddl_write_tdef(mdb, &ddl, tdef_page, usage_page, root_page, error, error_capacity)
        || !oasp_ddl_write_usage_page(mdb, &ddl, usage_page, root_page, data_page, error, error_capacity)
        || (root_page && !oasp_ddl_write_index_root_page(mdb, root_page, tdef_page, error, error_capacity))
        || !oasp_ddl_write_data_page(mdb, data_page, tdef_page, error, error_capacity)
        || !oasp_ddl_catalog_insert(mdb, &ddl, tdef_page, error, error_capacity)) {
        goto cleanup;
    }
    if (fflush(mdb->f->stream) != 0 || fsync(fileno(mdb->f->stream)) != 0) {
        oasp_ddl_error(error, error_capacity, "unable to durably flush CREATE TABLE changes");
        goto cleanup;
    }
    mdb_close(mdb);
    mdb = mdb_open(db_path, MDB_NOFLAGS);
    if (!mdb) {
        oasp_ddl_error(error, error_capacity, "unable to reopen MDB after CREATE TABLE");
        goto cleanup;
    }
    existing = mdb_read_table_by_name(mdb, ddl.name, MDB_TABLE);
    if (!existing || !mdb_read_columns(existing) || existing->num_cols != ddl.column_count) {
        oasp_ddl_error(error, error_capacity, "Jet table verification failed after CREATE TABLE");
        goto cleanup;
    }
    if (ddl.primary_key_column >= 0) {
        (void)mdb_read_indices(existing);
        if (!existing->indices || existing->num_idxs != 1) {
            oasp_ddl_error(error, error_capacity, "Jet primary key verification failed after CREATE TABLE");
            goto cleanup;
        }
    }
    result = 1;

cleanup:
    if (existing) {
        mdb_free_tabledef(existing);
    }
    if (mdb) {
        mdb_close(mdb);
    }
    if (result && lock.transaction_active) {
        if (!oasp_mdb_transaction_commit(&lock, error, error_capacity)) {
            result = 0;
            (void)oasp_mdb_transaction_rollback(&lock, error, error_capacity);
        }
    } else if (!result) {
        (void)oasp_mdb_transaction_rollback(&lock, error, error_capacity);
    }
    oasp_mdb_lock_release(&lock);
    oasp_ddl_table_free(&ddl);
    return result;
}

/*
 * Jet keeps the table definition at the catalog's table page.  Adding a
 * nullable column is the one schema change which does not require rewriting
 * every existing row: old records simply have no physical slot for the new
 * column and therefore read as NULL. Defaults, constraints and long values
 * need a full data/index migration and are kept outside this in-place
 * primitive.
 */
int oasp_mdb_alter_table_file(const char* db_path, const char* sql, char* error, size_t error_capacity) {
    const char* cursor;
    const char* after;
    EgDdlTable parsed;
    EgDdlColumn* column;
    MdbHandle* mdb = NULL;
    MdbTableDef* table = NULL;
    OaspMdbLock lock;
    unsigned char* page;
    unsigned char* encoded_name = NULL;
    uint16_t encoded_length = 0;
    uint16_t old_columns;
    uint16_t old_max_columns;
    uint16_t old_variables;
    uint32_t index_data_count;
    uint32_t old_length;
    size_t column_offset;
    size_t names_offset;
    size_t name_end;
    size_t new_length;
    size_t i;
    size_t existing_index = 0;
    int alter_column = 0;
    int drop_column = 0;
    int existing_found = 0;
    int result = 0;

    memset(&parsed, 0, sizeof(parsed));
    memset(&lock, 0, sizeof(lock));
    if (!db_path || !*db_path || !sql || !*sql
        || !oasp_ddl_keyword(sql, "alter", &cursor) || !oasp_ddl_keyword(cursor, "table", &cursor)) {
        oasp_ddl_error(error, error_capacity, "expected ALTER TABLE");
        goto cleanup;
    }
    parsed.name = oasp_ddl_parse_identifier(&cursor, error, error_capacity);
    if (!parsed.name) {
        goto cleanup;
    }
    if (oasp_ddl_keyword(cursor, "add", &after)) {
        cursor = oasp_ddl_skip_space(after);
        (void)oasp_ddl_keyword(cursor, "column", &cursor);
    } else if (oasp_ddl_keyword(cursor, "alter", &after)) {
        alter_column = 1;
        cursor = oasp_ddl_skip_space(after);
        (void)oasp_ddl_keyword(cursor, "column", &cursor);
    } else if (oasp_ddl_keyword(cursor, "drop", &after)) {
        drop_column = 1;
        cursor = oasp_ddl_skip_space(after);
        (void)oasp_ddl_keyword(cursor, "column", &cursor);
    } else {
        oasp_ddl_error(error, error_capacity, "ALTER TABLE currently supports ADD COLUMN, DROP COLUMN, or safe ALTER COLUMN widening");
        goto cleanup;
    }
    if (drop_column) {
        parsed.columns[0].name = oasp_ddl_parse_identifier(&cursor, error, error_capacity);
        cursor = oasp_ddl_skip_space(cursor);
        if (!parsed.columns[0].name || (*cursor == ';' && *oasp_ddl_skip_space(cursor + 1)) || (*cursor && *cursor != ';')) {
            oasp_ddl_error(error, error_capacity, "DROP COLUMN requires one column name");
            goto cleanup;
        }
        parsed.column_count = 1;
    } else {
        if (!oasp_ddl_parse_column(cursor, sql + strlen(sql), &parsed, error, error_capacity)
            || parsed.column_count != 1) {
            goto cleanup;
        }
    }
    column = &parsed.columns[0];
    if (!alter_column && !drop_column && (column->long_value || column->required || column->default_literal || column->primary_key)) {
        oasp_ddl_error(error, error_capacity,
                     "ALTER ADD COLUMN currently requires a nullable column without DEFAULT or constraints");
        goto cleanup;
    }
    if (!oasp_mdb_lock_acquire(db_path, 1, &lock, error, error_capacity)
        || !(mdb = mdb_open(db_path, MDB_WRITABLE))) {
        if (!mdb) {
            oasp_ddl_error(error, error_capacity, "unable to open MDB file for writing");
        }
        goto cleanup;
    }
    if (mdb->f->jet_version != MDB_VER_JET4 || mdb->fmt->pg_size != OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "native ALTER TABLE supports Jet 4 MDB files only");
        goto cleanup;
    }
    table = mdb_read_table_by_name(mdb, parsed.name, MDB_TABLE);
    if (!table || !mdb_read_columns(table)) {
        oasp_ddl_error(error, error_capacity, "ALTER TABLE target was not found");
        goto cleanup;
    }
    for (i = 0; i < table->num_cols; i++) {
        MdbColumn* existing = g_ptr_array_index(table->columns, i);
        if (existing && !strcasecmp(existing->name, column->name)) {
            existing_index = i;
            existing_found = 1;
            if (!alter_column && !drop_column) {
                oasp_ddl_error(error, error_capacity, "ALTER TABLE column already exists");
                goto cleanup;
            }
            continue;
        }
        if (!alter_column && !column->variable && existing && existing->is_fixed && existing->col_type != MDB_BOOL) {
            uint16_t end = (uint16_t)(existing->fixed_offset + existing->col_size);
            if (end > column->fixed_offset) {
                column->fixed_offset = end;
            }
        }
    }
    if (mdb_read_pg(mdb, table->entry->table_pg) != OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "unable to read Jet table definition");
        goto cleanup;
    }
    page = mdb->pg_buf;
    old_columns = (uint16_t)mdb_get_int16(page, 41);
    old_max_columns = old_columns;
    old_columns = (uint16_t)mdb_get_int16(page, 45);
    old_variables = (uint16_t)mdb_get_int16(page, 43);
    index_data_count = (uint32_t)mdb_get_int32(page, 51);
    old_length = (uint32_t)mdb_get_int32(page, 8);
    column_offset = OASP_JET4_TDEF_HEADER_SIZE + (size_t)index_data_count * OASP_JET4_INDEX_ROW_COUNT_SIZE;
    if (!old_columns || old_columns >= OASP_JET4_MAX_COLUMNS || old_length > OASP_JET4_PAGE_SIZE
        || column_offset + (size_t)old_columns * OASP_JET4_COLUMN_SIZE > old_length) {
        oasp_ddl_error(error, error_capacity, "unsupported or corrupt Jet table definition");
        goto cleanup;
    }
    if (alter_column) {
        MdbColumn* existing = existing_index < table->num_cols ? g_ptr_array_index(table->columns, existing_index) : NULL;
        if (!existing_found || !existing || existing_index >= table->num_cols
            || existing->col_type == MDB_MEMO || column->long_value
            || oasp_ddl_column_is_indexed(table, existing_index)) {
            oasp_ddl_error(error, error_capacity,
                         "ALTER COLUMN does not support indexed or MEMO source/target columns");
            goto cleanup;
        }
        if (!oasp_mdb_transaction_begin(&lock, error, error_capacity)) {
            goto cleanup;
        }
        if (!oasp_ddl_migrate_alter_column(&mdb, &table, db_path, parsed.name, existing_index,
                                         column, error, error_capacity)
            || fflush(mdb->f->stream) != 0 || fsync(fileno(mdb->f->stream)) != 0) {
            if (!*error) {
                oasp_ddl_error(error, error_capacity, "unable to durably flush ALTER COLUMN migration");
            }
            goto cleanup;
        }
        result = 1;
        goto cleanup;
    }
    if (drop_column) {
        MdbColumn* existing = existing_index < table->num_cols ? g_ptr_array_index(table->columns, existing_index) : NULL;
        size_t definition_offset;
        size_t target_name_offset;
        uint16_t target_name_length;
        if (!existing_found || !existing || old_columns <= 1) {
            oasp_ddl_error(error, error_capacity, "ALTER DROP COLUMN target was not found or would leave an empty table");
            goto cleanup;
        }
        if (oasp_ddl_column_is_indexed(table, existing_index)) {
            oasp_ddl_error(error, error_capacity, "ALTER DROP COLUMN does not support indexed columns");
            goto cleanup;
        }
        definition_offset = column_offset + existing_index * OASP_JET4_COLUMN_SIZE;
        names_offset = column_offset + (size_t)old_columns * OASP_JET4_COLUMN_SIZE;
        target_name_offset = names_offset;
        for (i = 0; i < existing_index; i++) {
            target_name_offset += 2 + (uint16_t)mdb_get_int16(page, target_name_offset);
        }
        target_name_length = (uint16_t)mdb_get_int16(page, target_name_offset);
        if (target_name_offset + 2 + target_name_length > old_length) {
            oasp_ddl_error(error, error_capacity, "corrupt Jet column-name list");
            goto cleanup;
        }
        if (!oasp_mdb_transaction_begin(&lock, error, error_capacity)) {
            goto cleanup;
        }
        memmove(page + definition_offset, page + definition_offset + OASP_JET4_COLUMN_SIZE,
                old_length - definition_offset - OASP_JET4_COLUMN_SIZE);
        old_length -= OASP_JET4_COLUMN_SIZE;
        target_name_offset -= OASP_JET4_COLUMN_SIZE;
        memmove(page + target_name_offset, page + target_name_offset + 2 + target_name_length,
                old_length - target_name_offset - 2 - target_name_length);
        old_length -= 2 + target_name_length;
        memset(page + old_length, 0, OASP_JET4_PAGE_SIZE - old_length);
        oasp_ddl_put_u16(page, 2, (uint16_t)(OASP_JET4_PAGE_SIZE - old_length - 8));
        oasp_ddl_put_u32(page, 8, old_length);
        oasp_ddl_put_u16(page, 45, (uint16_t)(old_columns - 1));
        if (mdb_write_pg(mdb, table->entry->table_pg) != OASP_JET4_PAGE_SIZE
            || fflush(mdb->f->stream) != 0 || fsync(fileno(mdb->f->stream)) != 0) {
            oasp_ddl_error(error, error_capacity, "unable to write DROP COLUMN definition");
            goto cleanup;
        }
        result = 1;
        goto cleanup;
    }
    if (!oasp_ddl_utf16le(column->name, &encoded_name, &encoded_length)) {
        oasp_ddl_error(error, error_capacity, "unable to encode Jet column name");
        goto cleanup;
    }
    new_length = (size_t)old_length + OASP_JET4_COLUMN_SIZE + 2 + encoded_length;
    if (new_length + 8 > OASP_JET4_PAGE_SIZE || mdb_get_int32(page, 4) != 0) {
        oasp_ddl_error(error, error_capacity, "ALTER ADD COLUMN requires a single-page Jet table definition with free space");
        goto cleanup;
    }
    if (!oasp_mdb_transaction_begin(&lock, error, error_capacity)) {
        goto cleanup;
    }
    names_offset = column_offset + (size_t)old_columns * OASP_JET4_COLUMN_SIZE;
    memmove(page + names_offset + OASP_JET4_COLUMN_SIZE, page + names_offset, old_length - names_offset);
    memset(page + names_offset, 0, OASP_JET4_COLUMN_SIZE);
    page[names_offset] = column->type;
    oasp_ddl_put_u32(page, names_offset + 1, OASP_JET4_TABLE_MAGIC);
    oasp_ddl_put_u16(page, names_offset + 5, old_max_columns);
    oasp_ddl_put_u16(page, names_offset + 7, column->variable ? old_variables : 0);
    oasp_ddl_put_u16(page, names_offset + 9, old_max_columns);
    oasp_ddl_put_u16(page, names_offset + 11, 0x0409);
    page[names_offset + 15] = column->flags;
    page[names_offset + 16] = column->ext_flags;
    oasp_ddl_put_u16(page, names_offset + 21, column->fixed_offset);
    oasp_ddl_put_u16(page, names_offset + 23, column->length);

    name_end = names_offset + OASP_JET4_COLUMN_SIZE;
    for (i = 0; i < old_columns; i++) {
        uint16_t length;
        if (name_end + 2 > old_length + OASP_JET4_COLUMN_SIZE) {
            oasp_ddl_error(error, error_capacity, "corrupt Jet column-name list");
            goto cleanup;
        }
        length = (uint16_t)mdb_get_int16(page, name_end);
        name_end += 2 + length;
    }
    if (name_end > old_length + OASP_JET4_COLUMN_SIZE) {
        oasp_ddl_error(error, error_capacity, "corrupt Jet column-name length");
        goto cleanup;
    }
    memmove(page + name_end + 2 + encoded_length, page + name_end,
            old_length + OASP_JET4_COLUMN_SIZE - name_end);
    oasp_ddl_put_u16(page, name_end, encoded_length);
    memcpy(page + name_end + 2, encoded_name, encoded_length);
    oasp_ddl_put_u16(page, 2, (uint16_t)(OASP_JET4_PAGE_SIZE - new_length - 8));
    oasp_ddl_put_u32(page, 8, (uint32_t)new_length);
    oasp_ddl_put_u16(page, 41, (uint16_t)(old_max_columns + 1));
    oasp_ddl_put_u16(page, 43, (uint16_t)(old_variables + (column->variable ? 1 : 0)));
    oasp_ddl_put_u16(page, 45, (uint16_t)(mdb_get_int16(page, 45) + 1));
    if (mdb_write_pg(mdb, table->entry->table_pg) != OASP_JET4_PAGE_SIZE
        || fflush(mdb->f->stream) != 0 || fsync(fileno(mdb->f->stream)) != 0) {
        oasp_ddl_error(error, error_capacity, "unable to write ALTER TABLE definition");
        goto cleanup;
    }
    result = 1;

cleanup:
    free(encoded_name);
    if (table) {
        mdb_free_tabledef(table);
    }
    if (mdb) {
        mdb_close(mdb);
    }
    if (result && lock.transaction_active) {
        if (!oasp_mdb_transaction_commit(&lock, error, error_capacity)) {
            result = 0;
            (void)oasp_mdb_transaction_rollback(&lock, error, error_capacity);
        }
    } else if (!result) {
        (void)oasp_mdb_transaction_rollback(&lock, error, error_capacity);
    }
    oasp_mdb_lock_release(&lock);
    oasp_ddl_table_free(&parsed);
    return result;
}

static int oasp_ddl_page_list_add(uint32_t** pages, size_t* count, size_t* capacity, uint32_t page_number) {
    uint32_t* next;
    size_t i;
    if (!page_number || page_number <= 1) {
        return 0;
    }
    for (i = 0; i < *count; i++) {
        if ((*pages)[i] == page_number) {
            return 1;
        }
    }
    if (*count == *capacity) {
        size_t next_capacity = *capacity ? *capacity * 2 : 16;
        next = (uint32_t*)realloc(*pages, next_capacity * sizeof(**pages));
        if (!next) {
            return 0;
        }
        *pages = next;
        *capacity = next_capacity;
    }
    (*pages)[(*count)++] = page_number;
    return 1;
}

static int oasp_ddl_mark_global_page_free(MdbHandle* mdb, uint32_t page_number, char* error, size_t error_capacity) {
    uint16_t row_start;
    uint8_t map_type;
    if (mdb_read_pg(mdb, 1) != OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "unable to read Jet global usage map");
        return 0;
    }
    row_start = (uint16_t)mdb_get_int16(mdb->pg_buf, 14);
    map_type = mdb->pg_buf[row_start];
    if (map_type == 0) {
        uint32_t base = (uint32_t)mdb_get_int32(mdb->pg_buf, row_start + 1);
        uint32_t relative;
        if (page_number < base || page_number - base >= 512) {
            oasp_ddl_error(error, error_capacity, "Jet global inline usage map capacity exceeded");
            return 0;
        }
        relative = page_number - base;
        mdb->pg_buf[row_start + 5 + relative / 8] |= (unsigned char)(1U << (relative % 8));
        if (mdb_write_pg(mdb, 1) != OASP_JET4_PAGE_SIZE) {
            oasp_ddl_error(error, error_capacity, "unable to update Jet global usage map");
            return 0;
        }
        return 1;
    }
    if (map_type == 1) {
        uint32_t pages_per_map = (OASP_JET4_PAGE_SIZE - 4) * 8;
        uint32_t map_index = page_number / pages_per_map;
        uint32_t relative = page_number % pages_per_map;
        uint32_t map_page;
        if (row_start + 1 + (map_index + 1) * 4 > OASP_JET4_PAGE_SIZE) {
            oasp_ddl_error(error, error_capacity, "Jet global reference usage map capacity exceeded");
            return 0;
        }
        map_page = (uint32_t)mdb_get_int32(mdb->pg_buf, row_start + 1 + map_index * 4);
        if (!map_page || mdb_read_pg(mdb, map_page) != OASP_JET4_PAGE_SIZE || mdb->pg_buf[0] != 5) {
            oasp_ddl_error(error, error_capacity, "missing Jet global reference usage map page");
            return 0;
        }
        mdb->pg_buf[4 + relative / 8] |= (unsigned char)(1U << (relative % 8));
        if (mdb_write_pg(mdb, map_page) != OASP_JET4_PAGE_SIZE) {
            oasp_ddl_error(error, error_capacity, "unable to update Jet global reference usage map");
            return 0;
        }
        return 1;
    }
    oasp_ddl_error(error, error_capacity, "unsupported Jet global usage map type");
    return 0;
}

static int oasp_ddl_collect_table_pages(MdbTableDef* table, uint32_t** pages, size_t* count, char* error, size_t error_capacity) {
    MdbHandle* mdb;
    uint32_t* collected = NULL;
    size_t capacity = 0;
    guint32 cursor = 0;
    gint32 page_number;
    uint32_t owned_map;
    uint32_t free_map;
    if (!table || !table->entry || !(mdb = table->entry->mdb)
        || mdb_read_pg(mdb, table->entry->table_pg) != OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "unable to read Jet table definition");
        return 0;
    }
    owned_map = (uint32_t)mdb_get_int32(mdb->pg_buf, 55);
    free_map = (uint32_t)mdb_get_int32(mdb->pg_buf, 59);
    if (!owned_map || !free_map
        || !oasp_ddl_page_list_add(&collected, count, &capacity, (uint32_t)table->entry->table_pg)
        || !oasp_ddl_page_list_add(&collected, count, &capacity, owned_map >> 8)
        || !oasp_ddl_page_list_add(&collected, count, &capacity, free_map >> 8)) {
        free(collected);
        oasp_ddl_error(error, error_capacity, "unsupported Jet table page ownership map");
        return 0;
    }
    while ((page_number = mdb_map_find_next(mdb, table->usage_map, (unsigned int)table->map_sz, cursor)) > 0) {
        if (!oasp_ddl_page_list_add(&collected, count, &capacity, (uint32_t)page_number)) {
            free(collected);
            oasp_ddl_error(error, error_capacity, "out of memory while collecting Jet table pages");
            return 0;
        }
        cursor = (guint32)page_number + 1;
    }
    if (page_number < 0) {
        free(collected);
        oasp_ddl_error(error, error_capacity, "unable to enumerate Jet table ownership map");
        return 0;
    }
    *pages = collected;
    return 1;
}

static int oasp_ddl_delete_catalog_row(MdbHandle* mdb, const char* table_name, char* error, size_t error_capacity) {
    MdbTableDef* catalog = NULL;
    int name_column = -1;
    unsigned int i;
    char name[MDB_BIND_SIZE];
    int name_length = 0;
    int row;
    int row_offset;
    size_t row_size;
    unsigned int row_count_offset;
    int result = 0;
    memset(name, 0, sizeof(name));
    catalog = mdb_read_table_by_name(mdb, (char*)"MSysObjects", MDB_TABLE);
    if (!catalog || !mdb_read_columns(catalog)) {
        oasp_ddl_error(error, error_capacity, "unable to read Jet system catalog");
        goto cleanup;
    }
    for (i = 0; i < catalog->num_cols; i++) {
        MdbColumn* column = g_ptr_array_index(catalog->columns, i);
        if (column && !strcasecmp(column->name, "Name")) {
            name_column = (int)i;
        }
    }
    if (name_column < 0 || mdb_bind_column(catalog, name_column + 1, name, &name_length) == -1) {
        oasp_ddl_error(error, error_capacity, "unable to bind Jet system catalog name");
        goto cleanup;
    }
    mdb_rewind_table(catalog);
    while (mdb_fetch_row(catalog)) {
        if (name_length > 0 && !strcasecmp(name, table_name)) {
            break;
        }
    }
    if (catalog->cur_row == 0 || name_length <= 0 || strcasecmp(name, table_name) != 0) {
        oasp_ddl_error(error, error_capacity, "Jet system catalog entry was not found");
        goto cleanup;
    }
    row = (int)catalog->cur_row - 1;
    if (mdb_read_pg(mdb, catalog->cur_phys_pg) != OASP_JET4_PAGE_SIZE
        || mdb_find_row(mdb, row, &row_offset, &row_size) != 0 || row_size == 0) {
        oasp_ddl_error(error, error_capacity, "unable to locate Jet system catalog entry");
        goto cleanup;
    }
    row_count_offset = mdb->fmt->row_count_offset;
    row_offset = (row_offset & 0x1fff) | 0x4000;
    mdb_put_int16(mdb->pg_buf, row_count_offset + 2 + row * 2, (unsigned int)row_offset);
    if (mdb_write_pg(mdb, catalog->cur_phys_pg) != OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "unable to delete Jet system catalog entry");
        goto cleanup;
    }
    if (catalog->num_rows == 0
        || mdb_read_pg(mdb, catalog->entry->table_pg) != OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "unable to update Jet system catalog row count");
        goto cleanup;
    }
    mdb_put_int32(mdb->pg_buf, mdb->fmt->tab_num_rows_offset, catalog->num_rows - 1);
    if (mdb_write_pg(mdb, catalog->entry->table_pg) != OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "unable to update Jet system catalog row count");
        goto cleanup;
    }
    result = 1;

cleanup:
    if (catalog) {
        mdb_free_tabledef(catalog);
    }
    return result;
}

static int oasp_ddl_append_index_metadata(MdbHandle* mdb, MdbTableDef* table, const EgDdlIndex* definition, const size_t* column_indexes, uint32_t root_page,
                                          char* error, size_t error_capacity) {
    unsigned char original[OASP_JET4_PAGE_SIZE];
    unsigned char page[OASP_JET4_PAGE_SIZE];
    unsigned char usage[OASP_JET4_PAGE_SIZE];
    unsigned char* name = NULL;
    uint16_t name_length = 0;
    uint32_t old_count;
    uint16_t columns;
    uint32_t old_length;
    uint32_t usage_map;
    size_t column_offset;
    size_t names_end;
    size_t block_offset;
    size_t info_offset;
    size_t trailer_offset;
    size_t old_usage_rows;
    size_t new_length;
    if (!mdb || !table || !definition
        || mdb_read_pg(mdb, table->entry->table_pg) != OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "unable to read Jet index table definition");
        return 0;
    }
    memcpy(original, mdb->pg_buf, sizeof(original));
    old_count = (uint32_t)mdb_get_int32(original, 51);
    columns = (uint16_t)mdb_get_int16(original, 45);
    old_length = (uint32_t)mdb_get_int32(original, 8);
    column_offset = OASP_JET4_TDEF_HEADER_SIZE + (size_t)old_count * OASP_JET4_INDEX_ROW_COUNT_SIZE;
    if (old_length > sizeof(page) || column_offset + (size_t)columns * OASP_JET4_COLUMN_SIZE > old_length
        || old_count >= OASP_JET4_MAX_INDEX_COLUMNS * 20U || !definition->column_count
        || !oasp_ddl_utf16le(definition->name, &name, &name_length)) {
        free(name);
        oasp_ddl_error(error, error_capacity, "unsupported Jet index definition");
        return 0;
    }
    names_end = column_offset + (size_t)columns * OASP_JET4_COLUMN_SIZE;
    for (size_t i = 0; i < columns; i++) {
        uint16_t length;
        if (names_end + 2 > old_length || (length = (uint16_t)mdb_get_int16(original, names_end),
            names_end + 2 + length > old_length)) {
            free(name);
            oasp_ddl_error(error, error_capacity, "corrupt Jet column names");
            return 0;
        }
        names_end += 2 + length;
    }
    block_offset = names_end + (size_t)old_count * OASP_JET4_INDEX_COLUMN_BLOCK_SIZE;
    info_offset = block_offset;
    for (size_t i = 0; i < old_count; i++) {
        uint16_t length;
        if (info_offset + OASP_JET4_INDEX_INFO_BLOCK_SIZE + 2 > old_length
            || (length = (uint16_t)mdb_get_int16(original, info_offset + OASP_JET4_INDEX_INFO_BLOCK_SIZE),
                info_offset + OASP_JET4_INDEX_INFO_BLOCK_SIZE + 2 + length > old_length)) {
            free(name);
            oasp_ddl_error(error, error_capacity, "corrupt Jet index directory");
            return 0;
        }
        info_offset += OASP_JET4_INDEX_INFO_BLOCK_SIZE + 2 + length;
    }
    trailer_offset = info_offset;
    new_length = (size_t)old_length + OASP_JET4_INDEX_ROW_COUNT_SIZE + OASP_JET4_INDEX_COLUMN_BLOCK_SIZE
        + OASP_JET4_INDEX_INFO_BLOCK_SIZE + 2 + name_length;
    if (new_length + 8 > sizeof(page)) {
        free(name);
        oasp_ddl_error(error, error_capacity, "Jet table definition has no room for an index");
        return 0;
    }
    memset(page, 0, sizeof(page));
    memcpy(page, original, OASP_JET4_TDEF_HEADER_SIZE + (size_t)old_count * OASP_JET4_INDEX_ROW_COUNT_SIZE);
    memcpy(page + OASP_JET4_TDEF_HEADER_SIZE + (size_t)(old_count + 1) * OASP_JET4_INDEX_ROW_COUNT_SIZE,
           original + OASP_JET4_TDEF_HEADER_SIZE + (size_t)old_count * OASP_JET4_INDEX_ROW_COUNT_SIZE,
           names_end - (OASP_JET4_TDEF_HEADER_SIZE + (size_t)old_count * OASP_JET4_INDEX_ROW_COUNT_SIZE));
    block_offset += OASP_JET4_INDEX_ROW_COUNT_SIZE;
    info_offset += OASP_JET4_INDEX_ROW_COUNT_SIZE + OASP_JET4_INDEX_COLUMN_BLOCK_SIZE;
    memcpy(page + block_offset + OASP_JET4_INDEX_COLUMN_BLOCK_SIZE,
           original + names_end, (size_t)old_count * OASP_JET4_INDEX_COLUMN_BLOCK_SIZE);
    memcpy(page + info_offset + OASP_JET4_INDEX_INFO_BLOCK_SIZE + 2 + name_length,
           original + trailer_offset, old_length - trailer_offset);
    oasp_ddl_put_u32(page, 47, old_count + 1);
    oasp_ddl_put_u32(page, 51, old_count + 1);
    oasp_ddl_put_u32(page, 8, (uint32_t)new_length);
    oasp_ddl_put_u16(page, 2, (uint16_t)(OASP_JET4_PAGE_SIZE - new_length - 8));
    oasp_ddl_put_u32(page, OASP_JET4_TDEF_HEADER_SIZE + (size_t)old_count * OASP_JET4_INDEX_ROW_COUNT_SIZE,
                     OASP_JET4_INDEX_MAGIC);
    oasp_ddl_put_u32(page, block_offset, OASP_JET4_INDEX_MAGIC);
    for (size_t key = 0; key < 10; key++) {
        size_t offset = block_offset + 4 + key * 3;
        oasp_ddl_put_u16(page, offset, key < definition->column_count ? (uint16_t)column_indexes[key] : UINT16_MAX);
        page[offset + 2] = key < definition->column_count ? 1 : 0;
    }
    usage_map = (uint32_t)mdb_get_int32(original, 55);
    page[block_offset + 34] = 2;
    oasp_ddl_put_u24(page, block_offset + 35, usage_map >> 8);
    oasp_ddl_put_u32(page, block_offset + 38, root_page);
    page[block_offset + 46] = definition->unique ? (MDB_IDX_UNIQUE | MDB_IDX_REQUIRED) : 0;
    oasp_ddl_put_u32(page, info_offset, OASP_JET4_TABLE_MAGIC);
    oasp_ddl_put_u32(page, info_offset + 4, 0);
    oasp_ddl_put_u32(page, info_offset + 8, 0);
    oasp_ddl_put_u32(page, info_offset + 13, UINT32_MAX);
    page[info_offset + 23] = 1;
    oasp_ddl_put_u16(page, info_offset + OASP_JET4_INDEX_INFO_BLOCK_SIZE, name_length);
    memcpy(page + info_offset + OASP_JET4_INDEX_INFO_BLOCK_SIZE + 2, name, name_length);
    free(name);
    if (mdb_read_pg(mdb, usage_map >> 8) != OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "unable to read Jet index usage map");
        return 0;
    }
    memcpy(usage, mdb->pg_buf, sizeof(usage));
    old_usage_rows = (uint16_t)mdb_get_int16(usage, 12);
    if (old_usage_rows >= 58 || 14 + (old_usage_rows + 1) * 2 > OASP_JET4_PAGE_SIZE
        || OASP_JET4_PAGE_SIZE - (old_usage_rows + 1) * OASP_JET4_USAGE_ROW_SIZE
           <= 14 + (old_usage_rows + 1) * 2) {
        oasp_ddl_error(error, error_capacity, "Jet index usage map is full");
        return 0;
    }
    oasp_ddl_put_u16(usage, 12, (uint16_t)(old_usage_rows + 1));
    oasp_ddl_put_u16(usage, 14 + old_usage_rows * 2,
                     (uint16_t)(OASP_JET4_PAGE_SIZE - (old_usage_rows + 1) * OASP_JET4_USAGE_ROW_SIZE));
    oasp_ddl_put_u32(usage, OASP_JET4_PAGE_SIZE - (old_usage_rows + 1) * OASP_JET4_USAGE_ROW_SIZE + 1, root_page);
    usage[OASP_JET4_PAGE_SIZE - (old_usage_rows + 1) * OASP_JET4_USAGE_ROW_SIZE + 5] = 1;
    oasp_ddl_put_u16(usage, 2, (uint16_t)(OASP_JET4_PAGE_SIZE - 14
                                           - (old_usage_rows + 1) * (OASP_JET4_USAGE_ROW_SIZE + 2)));
    return oasp_ddl_write_page(mdb, usage_map >> 8, usage)
        && oasp_ddl_write_page(mdb, (uint32_t)table->entry->table_pg, page);
}

int oasp_mdb_create_index_file(const char* db_path, const char* sql, char* error, size_t error_capacity) {
    EgDdlIndex definition;
    MdbHandle* mdb = NULL;
    MdbTableDef* table = NULL;
    OaspMdbLock lock;
    struct stat status;
    uint32_t root_page;
    size_t column_indexes[OASP_JET4_MAX_INDEX_COLUMNS];
    int result = 0;
    memset(&definition, 0, sizeof(definition));
    memset(&lock, 0, sizeof(lock));
    if (!oasp_ddl_parse_index(sql, &definition, error, error_capacity)
        || !oasp_mdb_lock_acquire(db_path, 1, &lock, error, error_capacity)
        || !(mdb = mdb_open(db_path, MDB_WRITABLE))) {
        if (!mdb && !error[0]) oasp_ddl_error(error, error_capacity, "unable to open MDB file for writing");
        goto cleanup;
    }
    table = mdb_read_table_by_name(mdb, definition.table_name, MDB_TABLE);
    if (!table || !mdb_read_columns(table)) {
        oasp_ddl_error(error, error_capacity, "unable to read Jet index table metadata");
        goto cleanup;
    }
    (void)mdb_read_indices(table);
    if (!table->indices) {
        table->indices = g_ptr_array_new();
        table->num_idxs = 0;
        table->num_real_idxs = 0;
    }
    for (size_t key = 0; key < definition.column_count; key++) {
        size_t column_index = 0;
        while (column_index < table->num_cols) {
            MdbColumn* column = g_ptr_array_index(table->columns, column_index);
            if (column && !strcasecmp(column->name, definition.column_names[key])) break;
            column_index++;
        }
        if (column_index == table->num_cols) {
            oasp_ddl_error(error, error_capacity, "CREATE INDEX references an unknown column");
            goto cleanup;
        }
        column_indexes[key] = column_index;
    }
    if (table->num_idxs >= 200) {
        oasp_ddl_error(error, error_capacity, "Jet table already has the maximum number of indexes");
        goto cleanup;
    }
    for (size_t i = 0; i < table->indices->len; i++) {
        MdbIndex* index = g_ptr_array_index(table->indices, i);
        if (index && !strcasecmp(index->name, definition.name)) {
            oasp_ddl_error(error, error_capacity, "Jet index already exists");
            goto cleanup;
        }
    }
    if (fstat(fileno(mdb->f->stream), &status) != 0 || status.st_size % OASP_JET4_PAGE_SIZE
        || !oasp_mdb_transaction_begin(&lock, error, error_capacity)) goto cleanup;
    root_page = (uint32_t)(status.st_size / OASP_JET4_PAGE_SIZE);
    if (!oasp_ddl_extend_file(mdb, root_page, 1, error, error_capacity)
        || !oasp_ddl_write_index_root_page(mdb, root_page, (uint32_t)table->entry->table_pg, error, error_capacity)
        || !oasp_ddl_append_index_metadata(mdb, table, &definition, column_indexes, root_page, error, error_capacity)) goto cleanup;
    mdb_free_tabledef(table); table = NULL; mdb_close(mdb); mdb = mdb_open(db_path, MDB_WRITABLE);
    table = mdb ? mdb_read_table_by_name(mdb, definition.table_name, MDB_TABLE) : NULL;
    if (!table || !mdb_read_columns(table)) {
        oasp_ddl_error(error, error_capacity, "unable to backfill Jet index");
        goto cleanup;
    }
    (void)mdb_read_indices(table);
    if (!table->indices || !oasp_mdb_backfill_indexes(table)) {
        oasp_ddl_error(error, error_capacity, "unable to backfill Jet index");
        goto cleanup;
    }
    result = fflush(mdb->f->stream) == 0 && fsync(fileno(mdb->f->stream)) == 0;
    if (!result) oasp_ddl_error(error, error_capacity, "unable to flush Jet index");
cleanup:
    if (table) mdb_free_tabledef(table);
    if (mdb) mdb_close(mdb);
    if (result) result = oasp_mdb_transaction_commit(&lock, error, error_capacity);
    else (void)oasp_mdb_transaction_rollback(&lock, error, error_capacity);
    oasp_mdb_lock_release(&lock);
    oasp_ddl_index_free(&definition);
    return result;
}

static int oasp_ddl_clear_usage_map_bit(MdbHandle* mdb, uint32_t packed_page_row, uint32_t page_number, char* error, size_t error_capacity) {
    uint32_t map_page_number = packed_page_row >> 8;
    uint32_t row_number = packed_page_row & 0xff;
    uint16_t row_count;
    uint16_t row_start;
    uint8_t map_type;
    if (!map_page_number || mdb_read_pg(mdb, map_page_number) != OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "unable to read Jet index usage map");
        return 0;
    }
    row_count = (uint16_t)mdb_get_int16(mdb->pg_buf, 12);
    if (row_number >= row_count) {
        oasp_ddl_error(error, error_capacity, "invalid Jet index usage-map row");
        return 0;
    }
    row_start = (uint16_t)(mdb_get_int16(mdb->pg_buf, 14 + row_number * 2) & 0x1fff);
    map_type = mdb->pg_buf[row_start];
    if (map_type == 0) {
        uint32_t base = (uint32_t)mdb_get_int32(mdb->pg_buf, row_start + 1);
        uint32_t relative;
        if (page_number < base || page_number - base >= 512) {
            oasp_ddl_error(error, error_capacity, "Jet index page is outside its usage map");
            return 0;
        }
        relative = page_number - base;
        mdb->pg_buf[row_start + 5 + relative / 8] &= (unsigned char)~(1U << (relative % 8));
        if (mdb_write_pg(mdb, map_page_number) != OASP_JET4_PAGE_SIZE) {
            oasp_ddl_error(error, error_capacity, "unable to update Jet index usage map");
            return 0;
        }
        return 1;
    }
    if (map_type == 1) {
        uint32_t pages_per_map = (OASP_JET4_PAGE_SIZE - 4) * 8;
        uint32_t map_index = page_number / pages_per_map;
        uint32_t relative = page_number % pages_per_map;
        uint32_t referenced_page;
        if (row_start + 1 + (map_index + 1) * 4 > OASP_JET4_PAGE_SIZE) {
            oasp_ddl_error(error, error_capacity, "Jet index reference usage map capacity exceeded");
            return 0;
        }
        referenced_page = (uint32_t)mdb_get_int32(mdb->pg_buf, row_start + 1 + map_index * 4);
        if (!referenced_page || mdb_read_pg(mdb, referenced_page) != OASP_JET4_PAGE_SIZE || mdb->pg_buf[0] != 5) {
            oasp_ddl_error(error, error_capacity, "missing Jet index reference usage map page");
            return 0;
        }
        mdb->pg_buf[4 + relative / 8] &= (unsigned char)~(1U << (relative % 8));
        if (mdb_write_pg(mdb, referenced_page) != OASP_JET4_PAGE_SIZE) {
            oasp_ddl_error(error, error_capacity, "unable to update Jet index reference usage map");
            return 0;
        }
        return 1;
    }
    oasp_ddl_error(error, error_capacity, "unsupported Jet index usage-map type");
    return 0;
}

static int oasp_ddl_index_page_add(uint32_t** pages, size_t* count, size_t* capacity, uint32_t page_number) {
    uint32_t* next;
    for (size_t i = 0; i < *count; i++) {
        if ((*pages)[i] == page_number) {
            return 1;
        }
    }
    if (*count == *capacity) {
        size_t next_capacity = *capacity ? *capacity * 2 : 8;
        next = (uint32_t*)realloc(*pages, next_capacity * sizeof(**pages));
        if (!next) {
            return 0;
        }
        *pages = next;
        *capacity = next_capacity;
    }
    (*pages)[(*count)++] = page_number;
    return 1;
}

static int oasp_ddl_collect_index_pages(MdbHandle* mdb, uint32_t page_number, uint32_t page_limit, uint32_t** pages, size_t* count,
                                        size_t* capacity, char* error, size_t error_capacity) {
    unsigned char page[OASP_JET4_PAGE_SIZE];
    size_t boundaries[512];
    size_t boundary_count = 0;
    size_t previous = 0;
    if (!page_number || page_number >= page_limit) {
        oasp_ddl_error(error, error_capacity, "invalid Jet index root page");
        return 0;
    }
    for (size_t i = 0; i < *count; i++) {
        if ((*pages)[i] == page_number) {
            return 1;
        }
    }
    if (mdb_read_pg(mdb, page_number) != OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "unable to read Jet index page");
        return 0;
    }
    memcpy(page, mdb->pg_buf, sizeof(page));
    if ((page[0] != 3 && page[0] != 4) || !oasp_ddl_index_page_add(pages, count, capacity, page_number)) {
        oasp_ddl_error(error, error_capacity, "corrupt Jet index page");
        return 0;
    }
    for (size_t bit = 1; bit < OASP_JET4_INDEX_ENTRY_MASK_SIZE * 8; bit++) {
        if (page[27 + bit / 8] & (unsigned char)(1U << (bit % 8))) {
            if (bit <= previous || boundary_count == sizeof(boundaries) / sizeof(boundaries[0])) {
                oasp_ddl_error(error, error_capacity, "corrupt Jet index entry mask");
                return 0;
            }
            boundaries[boundary_count++] = bit;
            previous = bit;
        }
    }
    if (page[0] == 4) {
        uint32_t next_page = (uint32_t)mdb_get_int32(page, 16);
        return !next_page || oasp_ddl_collect_index_pages(mdb, next_page, page_limit, pages, count, capacity,
                                                           error, error_capacity);
    }
    for (size_t entry = 0, start = 0; entry < boundary_count; entry++) {
        size_t end = boundaries[entry];
        uint32_t child;
        if (end - start < 4 || 27 + end > sizeof(page)) {
            oasp_ddl_error(error, error_capacity, "corrupt Jet index node");
            return 0;
        }
        child = (uint32_t)mdb_get_int32(page, 27 + end - 4);
        if (!oasp_ddl_collect_index_pages(mdb, child, page_limit, pages, count, capacity, error, error_capacity)) {
            return 0;
        }
        start = end;
    }
    {
        uint32_t rightmost = (uint32_t)mdb_get_int32(page, 20);
        return !rightmost || oasp_ddl_collect_index_pages(mdb, rightmost, page_limit, pages, count, capacity,
                                                           error, error_capacity);
    }
}

static int oasp_ddl_remove_index_metadata(MdbHandle* mdb, MdbTableDef* table, size_t target, char* error, size_t error_capacity) {
    unsigned char original[OASP_JET4_PAGE_SIZE];
    unsigned char page[OASP_JET4_PAGE_SIZE];
    uint32_t count;
    uint16_t columns;
    uint32_t old_length;
    size_t row_offsets[200];
    size_t block_offset;
    size_t info_offsets[200];
    size_t info_sizes[200];
    size_t names_end;
    size_t trailer_offset;
    size_t out = 0;
    if (!mdb || !table || mdb_read_pg(mdb, table->entry->table_pg) != OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "unable to read Jet index table definition");
        return 0;
    }
    memcpy(original, mdb->pg_buf, sizeof(original));
    count = (uint32_t)mdb_get_int32(original, 51);
    columns = (uint16_t)mdb_get_int16(original, 45);
    old_length = (uint32_t)mdb_get_int32(original, 8);
    if (!count || target >= count || count > sizeof(row_offsets) / sizeof(row_offsets[0]) || old_length > sizeof(page)) {
        oasp_ddl_error(error, error_capacity, "corrupt Jet index directory");
        return 0;
    }
    names_end = OASP_JET4_TDEF_HEADER_SIZE + (size_t)count * OASP_JET4_INDEX_ROW_COUNT_SIZE
              + (size_t)columns * OASP_JET4_COLUMN_SIZE;
    if (names_end > old_length) {
        oasp_ddl_error(error, error_capacity, "corrupt Jet index table definition");
        return 0;
    }
    for (size_t i = 0; i < columns; i++) {
        uint16_t length;
        if (names_end + 2 > old_length || (length = (uint16_t)mdb_get_int16(original, names_end),
            names_end + 2 + length > old_length)) {
            oasp_ddl_error(error, error_capacity, "corrupt Jet column names");
            return 0;
        }
        names_end += 2 + length;
    }
    block_offset = names_end;
    if (block_offset + (size_t)count * OASP_JET4_INDEX_COLUMN_BLOCK_SIZE > old_length) {
        oasp_ddl_error(error, error_capacity, "corrupt Jet index column blocks");
        return 0;
    }
    trailer_offset = block_offset + (size_t)count * OASP_JET4_INDEX_COLUMN_BLOCK_SIZE;
    for (size_t i = 0; i < count; i++) {
        uint16_t name_length;
        info_offsets[i] = trailer_offset;
        if (trailer_offset + OASP_JET4_INDEX_INFO_BLOCK_SIZE + 2 > old_length
            || (name_length = (uint16_t)mdb_get_int16(original, trailer_offset + OASP_JET4_INDEX_INFO_BLOCK_SIZE),
                trailer_offset + OASP_JET4_INDEX_INFO_BLOCK_SIZE + 2 + name_length > old_length)) {
            oasp_ddl_error(error, error_capacity, "corrupt Jet index information blocks");
            return 0;
        }
        info_sizes[i] = OASP_JET4_INDEX_INFO_BLOCK_SIZE + 2 + name_length;
        trailer_offset += info_sizes[i];
    }
    memset(page, 0, sizeof(page));
    for (size_t i = 0; i < count; i++) {
        if (i == target) {
            continue;
        }
        row_offsets[out++] = i;
    }
    memcpy(page, original, OASP_JET4_TDEF_HEADER_SIZE);
    out = OASP_JET4_TDEF_HEADER_SIZE;
    for (size_t i = 0; i < count; i++) {
        if (i != target) {
            memcpy(page + out, original + OASP_JET4_TDEF_HEADER_SIZE + i * OASP_JET4_INDEX_ROW_COUNT_SIZE,
                   OASP_JET4_INDEX_ROW_COUNT_SIZE);
            out += OASP_JET4_INDEX_ROW_COUNT_SIZE;
        }
    }
    memcpy(page + out, original + OASP_JET4_TDEF_HEADER_SIZE + (size_t)count * OASP_JET4_INDEX_ROW_COUNT_SIZE,
           names_end - (OASP_JET4_TDEF_HEADER_SIZE + (size_t)count * OASP_JET4_INDEX_ROW_COUNT_SIZE));
    out += names_end - (OASP_JET4_TDEF_HEADER_SIZE + (size_t)count * OASP_JET4_INDEX_ROW_COUNT_SIZE);
    for (size_t i = 0; i < count; i++) {
        if (i != target) {
            memcpy(page + out, original + block_offset + i * OASP_JET4_INDEX_COLUMN_BLOCK_SIZE,
                   OASP_JET4_INDEX_COLUMN_BLOCK_SIZE);
            out += OASP_JET4_INDEX_COLUMN_BLOCK_SIZE;
        }
    }
    for (size_t i = 0; i < count; i++) {
        if (i != target) {
            memcpy(page + out, original + info_offsets[i], info_sizes[i]);
            out += info_sizes[i];
        }
    }
    memcpy(page + out, original + trailer_offset, old_length - trailer_offset);
    out += old_length - trailer_offset;
    oasp_ddl_put_u32(page, 47, count - 1);
    oasp_ddl_put_u32(page, 51, count - 1);
    oasp_ddl_put_u32(page, 8, (uint32_t)out);
    oasp_ddl_put_u16(page, 2, (uint16_t)(OASP_JET4_PAGE_SIZE - out - 8));
    return oasp_ddl_write_page(mdb, (uint32_t)table->entry->table_pg, page);
}

int oasp_mdb_drop_index_file(const char* db_path, const char* sql, char* error, size_t error_capacity) {
    const char* cursor;
    char* index_name = NULL;
    MdbHandle* mdb = NULL;
    MdbTableDef* table = NULL;
    MdbIndex* index = NULL;
    OaspMdbLock lock;
    struct stat status;
    uint32_t* pages = NULL;
    size_t page_count = 0;
    size_t page_capacity = 0;
    uint32_t index_usage_map = 0;
    int result = 0;
    memset(&lock, 0, sizeof(lock));
    if (!sql || !oasp_ddl_keyword(sql, "drop", &cursor) || !oasp_ddl_keyword(cursor, "index", &cursor)) {
        oasp_ddl_error(error, error_capacity, "expected DROP INDEX");
        goto cleanup;
    }
    index_name = oasp_ddl_parse_identifier(&cursor, error, error_capacity);
    cursor = oasp_ddl_skip_space(cursor);
    if (!index_name || (*cursor == ';' && *oasp_ddl_skip_space(cursor + 1)) || (*cursor && *cursor != ';')) {
        oasp_ddl_error(error, error_capacity, "DROP INDEX requires one index name");
        goto cleanup;
    }
    if (!oasp_mdb_lock_acquire(db_path, 1, &lock, error, error_capacity) || !(mdb = mdb_open(db_path, MDB_WRITABLE))
        || mdb->f->jet_version != MDB_VER_JET4 || fstat(fileno(mdb->f->stream), &status) != 0
        || status.st_size % OASP_JET4_PAGE_SIZE != 0) {
        if (!error[0]) oasp_ddl_error(error, error_capacity, "unable to open Jet 4 MDB file for writing");
        goto cleanup;
    }
    {
        GPtrArray* catalog_entries = mdb_read_catalog(mdb, MDB_TABLE);
        for (guint entry = 0; catalog_entries && entry < catalog_entries->len && !table; entry++) {
            MdbCatalogEntry* catalog = g_ptr_array_index(catalog_entries, entry);
            MdbTableDef* candidate = mdb_read_table(catalog);
        if (!candidate || !mdb_read_columns(candidate)) {
            if (candidate) mdb_free_tabledef(candidate);
            continue;
        }
        (void)mdb_read_indices(candidate);
        if (!candidate->indices) {
            mdb_free_tabledef(candidate);
            continue;
        }
        for (size_t i = 0; i < candidate->indices->len; i++) {
            MdbIndex* candidate_index = g_ptr_array_index(candidate->indices, i);
            if (candidate_index && !strcasecmp(candidate_index->name, index_name)) {
                table = candidate;
                index = candidate_index;
                break;
            }
        }
        if (candidate != table) mdb_free_tabledef(candidate);
        }
    }
    if (!table || !index || index->index_num < 0 || !index->first_pg) {
        oasp_ddl_error(error, error_capacity, "Jet index was not found");
        goto cleanup;
    }
    if (mdb_read_pg(mdb, table->entry->table_pg) != OASP_JET4_PAGE_SIZE
        || (size_t)table->index_start + (size_t)index->index_num * OASP_JET4_INDEX_COLUMN_BLOCK_SIZE
           + OASP_JET4_INDEX_COLUMN_BLOCK_SIZE > OASP_JET4_PAGE_SIZE) {
        oasp_ddl_error(error, error_capacity, "unable to read Jet index usage-map reference");
        goto cleanup;
    }
    index_usage_map = (uint32_t)mdb_get_int32(mdb->pg_buf,
        (unsigned int)((size_t)table->index_start + (size_t)index->index_num * OASP_JET4_INDEX_COLUMN_BLOCK_SIZE + 34));
    if (!oasp_mdb_transaction_begin(&lock, error, error_capacity)
        || !oasp_ddl_collect_index_pages(mdb, index->first_pg, (uint32_t)(status.st_size / OASP_JET4_PAGE_SIZE),
                                         &pages, &page_count, &page_capacity, error, error_capacity)
        || !oasp_ddl_remove_index_metadata(mdb, table, (size_t)index->index_num, error, error_capacity)) {
        goto cleanup;
    }
    for (size_t i = 0; i < page_count; i++) {
        unsigned char blank[OASP_JET4_PAGE_SIZE] = {0};
        if (!oasp_ddl_clear_usage_map_bit(mdb, index_usage_map, pages[i], error, error_capacity)
            || !oasp_ddl_write_page(mdb, pages[i], blank)) {
            goto cleanup;
        }
    }
    result = fflush(mdb->f->stream) == 0 && fsync(fileno(mdb->f->stream)) == 0;
    if (!result) oasp_ddl_error(error, error_capacity, "unable to flush DROP INDEX");
cleanup:
    free(pages);
    free(index_name);
    if (table) mdb_free_tabledef(table);
    if (mdb) mdb_close(mdb);
    if (result) result = oasp_mdb_transaction_commit(&lock, error, error_capacity);
    else (void)oasp_mdb_transaction_rollback(&lock, error, error_capacity);
    oasp_mdb_lock_release(&lock);
    return result;
}

int oasp_mdb_drop_table_file(const char* db_path, const char* sql, char* error, size_t error_capacity) {
    const char* cursor;
    const char* after;
    char* table_name = NULL;
    MdbHandle* mdb = NULL;
    MdbTableDef* table = NULL;
    OaspMdbLock lock;
    uint32_t* pages = NULL;
    size_t page_count = 0;
    size_t i;
    struct stat status;
    int if_exists = 0;
    int result = 0;
    unsigned char zero_page[OASP_JET4_PAGE_SIZE];
    memset(&lock, 0, sizeof(lock));
    memset(zero_page, 0, sizeof(zero_page));
    if (!db_path || !*db_path || !sql || !*sql
        || !oasp_ddl_keyword(sql, "drop", &cursor) || !oasp_ddl_keyword(cursor, "table", &cursor)) {
        oasp_ddl_error(error, error_capacity, "expected DROP TABLE");
        goto cleanup;
    }
    if (oasp_ddl_keyword(cursor, "if", &after)) {
        if (!oasp_ddl_keyword(after, "exists", &cursor)) {
            oasp_ddl_error(error, error_capacity, "expected EXISTS after DROP TABLE IF");
            goto cleanup;
        }
        if_exists = 1;
    }
    table_name = oasp_ddl_parse_identifier(&cursor, error, error_capacity);
    cursor = oasp_ddl_skip_space(cursor);
    if (!table_name || (*cursor == ';' && *oasp_ddl_skip_space(cursor + 1)) || (*cursor && *cursor != ';')) {
        oasp_ddl_error(error, error_capacity, "DROP TABLE requires one table name");
        goto cleanup;
    }
    if (!oasp_mdb_lock_acquire(db_path, 1, &lock, error, error_capacity)
        || !(mdb = mdb_open(db_path, MDB_WRITABLE))) {
        if (!mdb) {
            oasp_ddl_error(error, error_capacity, "unable to open MDB file for writing");
        }
        goto cleanup;
    }
    if (mdb->f->jet_version != MDB_VER_JET4 || mdb->fmt->pg_size != OASP_JET4_PAGE_SIZE
        || fstat(fileno(mdb->f->stream), &status) != 0 || status.st_size % OASP_JET4_PAGE_SIZE != 0) {
        oasp_ddl_error(error, error_capacity, "native DROP TABLE supports page-aligned Jet 4 MDB files only");
        goto cleanup;
    }
    table = mdb_read_table_by_name(mdb, table_name, MDB_TABLE);
    if (!table) {
        if (if_exists) {
            result = 1;
        } else {
            oasp_ddl_error(error, error_capacity, "DROP TABLE target was not found");
        }
        goto cleanup;
    }
    if (!mdb_read_columns(table) || !table->usage_map || table->map_sz == 0) {
        oasp_ddl_error(error, error_capacity, "unable to read Jet table ownership metadata");
        goto cleanup;
    }
    if (!oasp_ddl_collect_table_pages(table, &pages, &page_count, error, error_capacity)
        || !oasp_mdb_transaction_begin(&lock, error, error_capacity)
        || !oasp_ddl_delete_catalog_row(mdb, table_name, error, error_capacity)) {
        goto cleanup;
    }
    for (i = 0; i < page_count; i++) {
        if ((off_t)pages[i] * OASP_JET4_PAGE_SIZE >= status.st_size
            || !oasp_ddl_write_page(mdb, pages[i], zero_page)
            || !oasp_ddl_mark_global_page_free(mdb, pages[i], error, error_capacity)) {
            if (!error[0]) {
                oasp_ddl_error(error, error_capacity, "unable to release Jet table page");
            }
            goto cleanup;
        }
    }
    if (fflush(mdb->f->stream) != 0 || fsync(fileno(mdb->f->stream)) != 0) {
        oasp_ddl_error(error, error_capacity, "unable to durably flush DROP TABLE changes");
        goto cleanup;
    }
    result = 1;

cleanup:
    free(pages);
    free(table_name);
    if (table) {
        mdb_free_tabledef(table);
    }
    if (mdb) {
        mdb_close(mdb);
    }
    if (result && lock.transaction_active) {
        if (!oasp_mdb_transaction_commit(&lock, error, error_capacity)) {
            result = 0;
            (void)oasp_mdb_transaction_rollback(&lock, error, error_capacity);
        }
    } else if (!result) {
        (void)oasp_mdb_transaction_rollback(&lock, error, error_capacity);
    }
    oasp_mdb_lock_release(&lock);
    return result;
}
