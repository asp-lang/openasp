// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#define _DEFAULT_SOURCE

/*
 * Module overview (`src/components/mdb.c`): Native component boundary. It maps ASP-compatible
 * object operations onto C libraries while preserving runtime error semantics, ownership, and
 * garbage-collector tracing. Result and error variables are initialized before crossing the
 * boundary.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#include "mdb.h"
#include "mdb_write.h"
#include "mdb_write_ddl.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <mdbtools.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static pthread_mutex_t oasp_mdb_stdout_mutex = PTHREAD_MUTEX_INITIALIZER;

bool oasp_mdb_stdout_silence_begin(oasp_mdb_stdout_silence *silence) {
    int null_fd;
    if (!silence)
        return false;
    silence->saved_stdout = -1;
    if (pthread_mutex_lock(&oasp_mdb_stdout_mutex))
        return false;
    (void)fflush(stdout);
    silence->saved_stdout = dup(STDOUT_FILENO);
    null_fd = open("/dev/null", O_WRONLY);
    if (silence->saved_stdout < 0 || null_fd < 0 || dup2(null_fd, STDOUT_FILENO) < 0) {
        if (null_fd >= 0)
            close(null_fd);
        if (silence->saved_stdout >= 0) {
            (void)dup2(silence->saved_stdout, STDOUT_FILENO);
            close(silence->saved_stdout);
        }
        silence->saved_stdout = -1;
        pthread_mutex_unlock(&oasp_mdb_stdout_mutex);
        return false;
    }
    close(null_fd);
    return true;
}

void oasp_mdb_stdout_silence_end(oasp_mdb_stdout_silence *silence) {
    if (!silence || silence->saved_stdout < 0)
        return;
    (void)fflush(stdout);
    (void)dup2(silence->saved_stdout, STDOUT_FILENO);
    close(silence->saved_stdout);
    silence->saved_stdout = -1;
    pthread_mutex_unlock(&oasp_mdb_stdout_mutex);
}

typedef struct {
    char *name;
    int type;
} oasp_mdb_column;

typedef struct {
    char *text;
    bool is_null;
} oasp_mdb_source_cell;

typedef struct {
    oasp_mdb_column *columns;
    oasp_mdb_source_cell *cells;
    size_t column_count;
    size_t row_count;
} oasp_mdb_table;

typedef enum {
    OASP_MDB_CONDITION_EQUAL,
    OASP_MDB_CONDITION_NOT_EQUAL,
    OASP_MDB_CONDITION_LESS,
    OASP_MDB_CONDITION_LESS_EQUAL,
    OASP_MDB_CONDITION_GREATER,
    OASP_MDB_CONDITION_GREATER_EQUAL,
    OASP_MDB_CONDITION_NULL,
    OASP_MDB_CONDITION_NOT_NULL,
    OASP_MDB_CONDITION_LIKE,
    OASP_MDB_CONDITION_BETWEEN,
    OASP_MDB_CONDITION_IN,
    OASP_MDB_CONDITION_NOT_IN,
    OASP_MDB_CONDITION_EXISTS
} oasp_mdb_condition_kind;

typedef struct {
    char *column;
    char *value;
    char *second_value;
    oasp_mdb_condition_kind kind;
    bool disjunction;
} oasp_mdb_condition;

typedef struct {
    char *column;
    bool descending;
} oasp_mdb_order;

typedef struct {
    char **projections;
    char **groups;
    size_t projection_count;
    size_t group_count;
    char *left_table;
    char *left_alias;
    char *left_sql;
    char *right_table;
    char *right_alias;
    char *right_sql;
    char *join_left;
    char *join_right;
    char *where_expression;
    char *having_expression;
    oasp_mdb_condition *conditions;
    oasp_mdb_condition *having;
    size_t condition_count;
    size_t having_count;
    oasp_mdb_order *orders;
    size_t order_count;
    size_t top_limit;
    bool distinct;
    bool left_join;
} oasp_mdb_query;

typedef struct {
    size_t left;
    size_t right;
} oasp_mdb_match;

static char *oasp_mdb_copy(const char *text, size_t length) {
    char *copy = malloc(length + 1);
    if (!copy)
        return NULL;
    if (length)
        memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static char *oasp_mdb_trim_copy(const char *begin, const char *end) {
    while (begin < end && isspace((unsigned char)*begin))
        begin++;
    while (end > begin && (isspace((unsigned char)end[-1]) || end[-1] == ';'))
        end--;
    if (end > begin + 1 && ((*begin == '[' && end[-1] == ']') || (*begin == '`' && end[-1] == '`')))
        return oasp_mdb_copy(begin + 1, (size_t)(end - begin - 2));
    return oasp_mdb_copy(begin, (size_t)(end - begin));
}

static const char *oasp_mdb_find_phrase(const char *text, const char *phrase) {
    size_t length = strlen(phrase);
    bool quoted = false;
    int depth = 0;
    for (const char *cursor = text; *cursor; cursor++) {
        if (*cursor == '\'') {
            if (quoted && cursor[1] == '\'') {
                cursor++;
                continue;
            }
            quoted = !quoted;
        }
        if (!quoted && *cursor == '(') {
            depth++;
            continue;
        }
        if (!quoted && *cursor == ')' && depth > 0) {
            depth--;
            continue;
        }
        if (!quoted && !depth && !strncasecmp(cursor, phrase, length))
            return cursor;
        /*
         * Jet accepts a clause immediately after a bracketed identifier, such
         * as "[Admin_ACT]ORDER BY [ID]". Preserve the closing bracket in the
         * returned span so table-name trimming still removes it correctly.
         */
        if (!quoted && !depth && *cursor == ']' && phrase[0] == ' ' &&
            !strncasecmp(cursor + 1, phrase + 1, length - 1))
            return cursor;
    }
    return NULL;
}

static bool oasp_mdb_push_string(char ***items, size_t *count, char *value) {
    char **next = realloc(*items, (*count + 1) * sizeof(**items));
    if (!next) {
        free(value);
        return false;
    }
    *items = next;
    next[(*count)++] = value;
    return true;
}

static bool oasp_mdb_split_list(const char *begin, const char *end, char ***items, size_t *count) {
    const char *start = begin;
    bool quoted = false;
    int depth = 0;
    for (const char *cursor = begin; cursor <= end; cursor++) {
        if (cursor < end && *cursor == '\'')
            quoted = !quoted;
        else if (cursor < end && !quoted && *cursor == '(')
            depth++;
        else if (cursor < end && !quoted && *cursor == ')' && depth > 0)
            depth--;
        if (cursor == end || (!quoted && !depth && *cursor == ',')) {
            char *item = oasp_mdb_trim_copy(start, cursor);
            if (!item || !*item || !oasp_mdb_push_string(items, count, item))
                return false;
            start = cursor + 1;
        }
    }
    return true;
}

static const char *oasp_mdb_column_suffix(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : name;
}

static bool oasp_mdb_count_projection(const char *projection) {
    size_t length = strlen(projection);
    return length >= 7 && !strncasecmp(projection, "count(", 6) && projection[length - 1] == ')';
}

typedef enum {
    OASP_MDB_AGGREGATE_NONE,
    OASP_MDB_AGGREGATE_COUNT,
    OASP_MDB_AGGREGATE_SUM,
    OASP_MDB_AGGREGATE_AVG,
    OASP_MDB_AGGREGATE_MIN,
    OASP_MDB_AGGREGATE_MAX
} oasp_mdb_aggregate_kind;

static oasp_mdb_aggregate_kind oasp_mdb_aggregate(const char *projection, const char **argument) {
    static const struct {
        const char *name;
        oasp_mdb_aggregate_kind kind;
    } names[] = {
        {"count(", OASP_MDB_AGGREGATE_COUNT},
        {"sum(", OASP_MDB_AGGREGATE_SUM},
        {"avg(", OASP_MDB_AGGREGATE_AVG},
        {"min(", OASP_MDB_AGGREGATE_MIN},
        {"max(", OASP_MDB_AGGREGATE_MAX},
    };
    size_t length = strlen(projection);
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        size_t name_length = strlen(names[i].name);
        if (length > name_length && !strncasecmp(projection, names[i].name, name_length)
            && projection[length - 1] == ')') {
            if (argument)
                *argument = projection + name_length;
            return names[i].kind;
        }
    }
    return OASP_MDB_AGGREGATE_NONE;
}

static void oasp_mdb_query_dispose(oasp_mdb_query *query) {
    for (size_t i = 0; i < query->projection_count; i++)
        free(query->projections[i]);
    for (size_t i = 0; i < query->group_count; i++)
        free(query->groups[i]);
    for (size_t i = 0; i < query->condition_count; i++) {
        free(query->conditions[i].column);
        free(query->conditions[i].value);
        free(query->conditions[i].second_value);
    }
    for (size_t i = 0; i < query->having_count; i++) {
        free(query->having[i].column);
        free(query->having[i].value);
        free(query->having[i].second_value);
    }
    for (size_t i = 0; i < query->order_count; i++)
        free(query->orders[i].column);
    free(query->projections);
    free(query->groups);
    free(query->left_table);
    free(query->left_alias);
    free(query->left_sql);
    free(query->right_table);
    free(query->right_alias);
    free(query->right_sql);
    free(query->join_left);
    free(query->join_right);
    free(query->where_expression);
    free(query->having_expression);
    free(query->conditions);
    free(query->having);
    free(query->orders);
    memset(query, 0, sizeof(*query));
}

static bool oasp_mdb_add_condition(oasp_mdb_condition **conditions, size_t *count, const char *begin, const char *end) {
    oasp_mdb_condition condition;
    const char *is_null;
    const char *between;
    const char *like;
    const char *operator = NULL;
    bool negated = false;
    bool exists = false;
    memset(&condition, 0, sizeof(condition));
    while (begin < end && isspace((unsigned char)*begin))
        begin++;
    if ((size_t)(end - begin) >= 6 && !strncasecmp(begin, "exists", 6)
        && ((size_t)(end - begin) == 6 || isspace((unsigned char)begin[6]) || begin[6] == '(')) {
        begin += 6;
        while (begin < end && isspace((unsigned char)*begin))
            begin++;
        exists = true;
    }
    if (exists) {
        condition.column = oasp_mdb_copy("", 0);
        condition.value = oasp_mdb_trim_copy(begin, end);
        condition.kind = OASP_MDB_CONDITION_EXISTS;
        if (condition.value && condition.value[0] == '(') {
            size_t length = strlen(condition.value);
            if (length < 3 || condition.value[length - 1] != ')') {
                free(condition.column);
                free(condition.value);
                return false;
            }
            memmove(condition.value, condition.value + 1, length - 2);
            condition.value[length - 2] = '\0';
        }
    } else if ((is_null = oasp_mdb_find_phrase(begin, " is not null")) && is_null < end) {
        condition.column = oasp_mdb_trim_copy(begin, is_null);
        condition.value = oasp_mdb_copy("", 0);
        condition.kind = OASP_MDB_CONDITION_NOT_NULL;
    } else if ((is_null = oasp_mdb_find_phrase(begin, " is null")) && is_null < end) {
        condition.column = oasp_mdb_trim_copy(begin, is_null);
        condition.value = oasp_mdb_copy("", 0);
        condition.kind = OASP_MDB_CONDITION_NULL;
    } else if ((between = oasp_mdb_find_phrase(begin, " between ")) && between < end) {
        const char *and = oasp_mdb_find_phrase(between + 9, " and ");
        if (!and || and >= end)
            return false;
        condition.column = oasp_mdb_trim_copy(begin, between);
        condition.value = oasp_mdb_trim_copy(between + 9, and);
        condition.second_value = oasp_mdb_trim_copy(and + 5, end);
        condition.kind = OASP_MDB_CONDITION_BETWEEN;
    } else if ((like = oasp_mdb_find_phrase(begin, " like ")) && like < end) {
        condition.column = oasp_mdb_trim_copy(begin, like);
        condition.value = oasp_mdb_trim_copy(like + 6, end);
        condition.kind = OASP_MDB_CONDITION_LIKE;
        if (condition.value && strlen(condition.value) >= 2 && condition.value[0] == '\'' &&
            condition.value[strlen(condition.value) - 1] == '\'') {
            size_t length = strlen(condition.value);
            memmove(condition.value, condition.value + 1, length - 2);
            condition.value[length - 2] = '\0';
        }
    } else {
        const char *in = oasp_mdb_find_phrase(begin, " not in ");
        if (in && in < end)
            negated = true;
        else
            in = oasp_mdb_find_phrase(begin, " in ");
        if (in && in < end) {
            condition.column = oasp_mdb_trim_copy(begin, in);
            condition.value = oasp_mdb_trim_copy(in + (negated ? 8 : 4), end);
            condition.kind = negated ? OASP_MDB_CONDITION_NOT_IN : OASP_MDB_CONDITION_IN;
            if (condition.value && condition.value[0] == '(') {
                size_t length = strlen(condition.value);
                if (length < 3 || condition.value[length - 1] != ')') {
                    free(condition.column);
                    free(condition.value);
                    return false;
                }
                memmove(condition.value, condition.value + 1, length - 2);
                condition.value[length - 2] = '\0';
            }
        } else {
        for (const char *cursor = begin; cursor < end; cursor++) {
            if (*cursor == '\'' && cursor + 1 < end && cursor[1] == '\'') {
                cursor++;
                continue;
            }
            if (*cursor == '\'' || (*cursor != '<' && *cursor != '>' && *cursor != '='))
                continue;
            operator = cursor;
            break;
        }
        if (!operator)
            return false;
        size_t operator_length = 1;
        if (operator[0] == '<' && operator + 1 < end && (operator[1] == '>' || operator[1] == '='))
            operator_length = 2;
        else if (operator[0] == '>' && operator + 1 < end && operator[1] == '=')
            operator_length = 2;
        condition.column = oasp_mdb_trim_copy(begin, operator);
        condition.value = oasp_mdb_trim_copy(operator + operator_length, end);
        if (operator[0] == '=')
            condition.kind = OASP_MDB_CONDITION_EQUAL;
        else if (operator[0] == '<' && operator_length == 2 && operator[1] == '>')
            condition.kind = OASP_MDB_CONDITION_NOT_EQUAL;
        else if (operator[0] == '<' && operator_length == 2)
            condition.kind = OASP_MDB_CONDITION_LESS_EQUAL;
        else if (operator[0] == '<')
            condition.kind = OASP_MDB_CONDITION_LESS;
        else if (operator_length == 2)
            condition.kind = OASP_MDB_CONDITION_GREATER_EQUAL;
        else
            condition.kind = OASP_MDB_CONDITION_GREATER;
        if (condition.value && strlen(condition.value) >= 2 && condition.value[0] == '\'' && condition.value[strlen(condition.value) - 1] == '\'') {
            size_t length = strlen(condition.value);
            memmove(condition.value, condition.value + 1, length - 2);
            condition.value[length - 2] = '\0';
        }
        }
    }
    if (!condition.column || (!*condition.column && condition.kind != OASP_MDB_CONDITION_EXISTS) || !condition.value ||
        (condition.kind == OASP_MDB_CONDITION_BETWEEN && !condition.second_value)) {
        free(condition.column);
        free(condition.value);
        free(condition.second_value);
        return false;
    }
    oasp_mdb_condition *next = realloc(*conditions, (*count + 1) * sizeof(**conditions));
    if (!next) {
        free(condition.column);
        free(condition.value);
        free(condition.second_value);
        return false;
    }
    *conditions = next;
    next[(*count)++] = condition;
    return true;
}

static bool oasp_mdb_parse_orders(oasp_mdb_query *query, const char *begin, const char *end) {
    char **items = NULL;
    size_t count = 0;
    if (!oasp_mdb_split_list(begin, end, &items, &count))
        goto fail;
    for (size_t i = 0; i < count; i++) {
        size_t length = strlen(items[i]);
        bool descending = false;
        if (length > 5 && !strcasecmp(items[i] + length - 5, " desc")) {
            items[i][length - 5] = '\0';
            descending = true;
        } else if (length > 4 && !strcasecmp(items[i] + length - 4, " asc")) {
            items[i][length - 4] = '\0';
        }
        char *column = oasp_mdb_trim_copy(items[i], items[i] + strlen(items[i]));
        if (!column || !*column) {
            free(column);
            goto fail;
        }
        oasp_mdb_order *next = realloc(query->orders, (query->order_count + 1) * sizeof(*query->orders));
        if (!next) {
            free(column);
            goto fail;
        }
        query->orders = next;
        query->orders[query->order_count++] = (oasp_mdb_order){column, descending};
    }
    for (size_t i = 0; i < count; i++)
        free(items[i]);
    free(items);
    return true;
fail:
    for (size_t i = 0; i < count; i++)
        free(items[i]);
    free(items);
    return false;
}

static bool oasp_mdb_parse_conditions(oasp_mdb_condition **conditions, size_t *count, const char *begin, const char *end) {
    const char *start = begin;
    bool disjunction = false;
    while (start < end) {
        const char *and = oasp_mdb_find_phrase(start, " and ");
        const char *or = oasp_mdb_find_phrase(start, " or ");
        const char *separator = and && (!or || and < or) ? and : or;
        const char *between = oasp_mdb_find_phrase(start, " between ");
        if (between && (!separator || between < separator)) {
            const char *range_end = oasp_mdb_find_phrase(between + 9, " and ");
            if (!range_end || range_end >= end)
                return false;
            and = oasp_mdb_find_phrase(range_end + 5, " and ");
            or = oasp_mdb_find_phrase(range_end + 5, " or ");
            separator = and && (!or || and < or) ? and : or;
        }
        if (!separator || separator >= end)
            separator = end;
        if (!oasp_mdb_add_condition(conditions, count, start, separator))
            return false;
        (*conditions)[*count - 1].disjunction = disjunction;
        if (separator == end)
            break;
        disjunction = !strncasecmp(separator, " or ", 4);
        start = separator + (disjunction ? 4 : 5);
    }
    return true;
}

static bool oasp_mdb_parse_source(const char *begin, const char *end, char **name, char **alias, char **sql) {
    char *source = oasp_mdb_trim_copy(begin, end);
    char *suffix;
    *name = NULL;
    *alias = NULL;
    *sql = NULL;
    if (!source || !*source)
        goto fail;
    if (source[0] == '(') {
        int depth = 0;
        bool quoted = false;
        size_t close = 0;
        for (size_t i = 0; source[i]; i++) {
            if (source[i] == '\'' && (!quoted || source[i + 1] != '\''))
                quoted = !quoted;
            if (!quoted && source[i] == '(')
                depth++;
            else if (!quoted && source[i] == ')' && --depth == 0) {
                close = i;
                break;
            }
        }
        if (!close)
            goto fail;
        *sql = oasp_mdb_trim_copy(source + 1, source + close);
        if (*sql && strncasecmp(*sql, "select ", 7)) {
            size_t length = strlen(*sql);
            char *nested = malloc(length + 15);
            if (!nested)
                goto fail;
            memcpy(nested, "SELECT * FROM ", 14);
            memcpy(nested + 14, *sql, length + 1);
            free(*sql);
            *sql = nested;
        }
        suffix = source + close + 1;
    } else {
        suffix = source;
        while (*suffix && !isspace((unsigned char)*suffix))
            suffix++;
        *name = oasp_mdb_copy(source, (size_t)(suffix - source));
    }
    while (isspace((unsigned char)*suffix))
        suffix++;
    if (!strncasecmp(suffix, "as ", 3) && isspace((unsigned char)suffix[2]))
        suffix += 3;
    while (isspace((unsigned char)*suffix))
        suffix++;
    if (*suffix)
        *alias = oasp_mdb_trim_copy(suffix, suffix + strlen(suffix));
    if (!*alias)
        *alias = *name ? oasp_mdb_copy(*name, strlen(*name)) : NULL;
    if (!*name && !*sql)
        goto fail;
    free(source);
    return true;
fail:
    free(source);
    free(*name);
    free(*alias);
    free(*sql);
    *name = NULL;
    *alias = NULL;
    *sql = NULL;
    return false;
}

static bool oasp_mdb_parse_query(const char *sql, oasp_mdb_query *query) {
    const char *select = sql;
    const char *from;
    const char *join;
    const char *on;
    const char *where;
    const char *group;
    const char *having;
    const char *order;
    const char *end = sql + strlen(sql);
    memset(query, 0, sizeof(*query));
    while (isspace((unsigned char)*select))
        select++;
    if (strncasecmp(select, "select ", 7))
        return false;
    from = oasp_mdb_find_phrase(select + 7, " from ");
    const char *projections = select + 7;
    while (isspace((unsigned char)*projections))
        projections++;
    if (!strncasecmp(projections, "distinct ", 9)) {
        query->distinct = true;
        projections += 9;
        while (isspace((unsigned char)*projections))
            projections++;
    }
    if (!strncasecmp(projections, "top ", 4)) {
        char *limit_end = NULL;
        unsigned long long limit;
        errno = 0;
        limit = strtoull(projections + 4, &limit_end, 10);
        if (errno || limit_end == projections + 4 || limit > SIZE_MAX)
            goto fail;
        projections = limit_end;
        while (isspace((unsigned char)*projections))
            projections++;
        query->top_limit = (size_t)limit;
    }
    if (!from || projections >= from || !oasp_mdb_split_list(projections, from, &query->projections, &query->projection_count))
        goto fail;
    join = oasp_mdb_find_phrase(from + 6, " left join ");
    if (join)
        query->left_join = true;
    else {
        join = oasp_mdb_find_phrase(from + 6, " left outer join ");
        if (join)
            query->left_join = true;
        else
            join = oasp_mdb_find_phrase(from + 6, " inner join ");
    }
    where = oasp_mdb_find_phrase(from + 6, " where ");
    group = oasp_mdb_find_phrase(from + 6, " group by ");
    having = oasp_mdb_find_phrase(from + 6, " having ");
    order = oasp_mdb_find_phrase(from + 6, " order by ");
    const char *left_end = end;
    if (join && join < left_end)
        left_end = join;
    if (where && where < left_end)
        left_end = where;
    if (group && group < left_end)
        left_end = group;
    if (having && having < left_end)
        left_end = having;
    if (order && order < left_end)
        left_end = order;
    if (left_end < end && *left_end == ']')
        left_end++;
    if (!oasp_mdb_parse_source(from + 6, left_end, &query->left_table, &query->left_alias, &query->left_sql))
        goto fail;
    if (join) {
        const char *join_end = join + (query->left_join ? (!strncasecmp(join, " left outer join ", 17) ? 17 : 11) : 12);
        on = oasp_mdb_find_phrase(join_end, " on ");
        if (!on || (where && on > where) || (order && on > order))
            goto fail;
        if (!oasp_mdb_parse_source(join_end, on, &query->right_table, &query->right_alias, &query->right_sql))
            goto fail;
        const char *on_end = end;
        if (where && where < on_end)
            on_end = where;
        if (group && group < on_end)
            on_end = group;
        if (having && having < on_end)
            on_end = having;
        if (order && order < on_end)
            on_end = order;
        const char *equals = memchr(on + 4, '=', (size_t)(on_end - on - 4));
        if (!equals)
            goto fail;
        query->join_left = oasp_mdb_trim_copy(on + 4, equals);
        query->join_right = oasp_mdb_trim_copy(equals + 1, on_end);
        if ((!query->right_table && !query->right_sql) || !query->join_left || !query->join_right)
            goto fail;
    }
    if (where) {
        const char *where_end = end;
        if (group && group < where_end)
            where_end = group;
        if (having && having < where_end)
            where_end = having;
        if (order && order < where_end)
            where_end = order;
        query->where_expression = oasp_mdb_trim_copy(where + 7, where_end);
        if (!query->where_expression
            || !oasp_mdb_parse_conditions(&query->conditions, &query->condition_count, where + 7, where_end)) {
            for (size_t i = 0; i < query->condition_count; i++) {
                free(query->conditions[i].column);
                free(query->conditions[i].value);
                free(query->conditions[i].second_value);
            }
            free(query->conditions);
            query->conditions = NULL;
            query->condition_count = 0;
        }
        if (!query->where_expression)
            goto fail;
    }
    if (group) {
        const char *group_end = end;
        if (having && having < group_end)
            group_end = having;
        if (order && order < group_end)
            group_end = order;
        if (!oasp_mdb_split_list(group + 10, group_end, &query->groups, &query->group_count))
            goto fail;
    }
    if (having) {
        const char *having_end = order && order > having ? order : end;
        query->having_expression = oasp_mdb_trim_copy(having + 8, having_end);
        if (!query->having_expression
            || !oasp_mdb_parse_conditions(&query->having, &query->having_count, having + 8, having_end)) {
            for (size_t i = 0; i < query->having_count; i++) {
                free(query->having[i].column);
                free(query->having[i].value);
                free(query->having[i].second_value);
            }
            free(query->having);
            query->having = NULL;
            query->having_count = 0;
        }
        if (!query->having_expression)
            goto fail;
    }
    if (order) {
        if (!oasp_mdb_parse_orders(query, order + 10, end))
            goto fail;
    }
    return true;
fail:
    oasp_mdb_query_dispose(query);
    return false;
}

static bool oasp_mdb_expand_wildcard(oasp_mdb_query *query, const oasp_mdb_table *left, const oasp_mdb_table *right) {
    if (query->projection_count != 1 || strcmp(query->projections[0], "*"))
        return true;
    size_t count = left->column_count + (right ? right->column_count : 0);
    char **projections = calloc(count, sizeof(*projections));
    if (!projections)
        return false;
    for (size_t index = 0; index < count; index++) {
        const oasp_mdb_table *table = index < left->column_count ? left : right;
        size_t column = index < left->column_count ? index : index - left->column_count;
        projections[index] = oasp_mdb_copy(table->columns[column].name, strlen(table->columns[column].name));
        if (!projections[index]) {
            for (size_t i = 0; i < index; i++)
                free(projections[i]);
            free(projections);
            return false;
        }
    }
    free(query->projections[0]);
    free(query->projections);
    query->projections = projections;
    query->projection_count = count;
    return true;
}

static void oasp_mdb_table_dispose(oasp_mdb_table *table) {
    for (size_t i = 0; i < table->column_count; i++)
        free(table->columns[i].name);
    for (size_t i = 0; i < table->row_count * table->column_count; i++)
        free(table->cells[i].text);
    free(table->columns);
    free(table->cells);
    memset(table, 0, sizeof(*table));
}

static bool oasp_mdb_table_append_null_row(oasp_mdb_table *table) {
    oasp_mdb_source_cell *cells;
    if (!table->column_count || table->row_count == SIZE_MAX / table->column_count)
        return false;
    cells = realloc(table->cells, (table->row_count + 1) * table->column_count * sizeof(*cells));
    if (!cells)
        return false;
    table->cells = cells;
    for (size_t i = 0; i < table->column_count; i++) {
        oasp_mdb_source_cell *cell = &table->cells[table->row_count * table->column_count + i];
        cell->text = oasp_mdb_copy("", 0);
        cell->is_null = true;
        if (!cell->text)
            return false;
    }
    table->row_count++;
    return true;
}

static bool oasp_mdb_load_table(const char *path, const char *name, oasp_mdb_table *result, char **error) {
    MdbHandle *mdb = NULL;
    MdbTableDef *table = NULL;
    char **bindings = NULL;
    int *lengths = NULL;
    size_t row_capacity = 0;
    bool ok = false;
    memset(result, 0, sizeof(*result));
    mdb = mdb_open(path, MDB_NOFLAGS);
    if (!mdb) {
        *error = oasp_mdb_copy("unable to open MDB database", 27);
        goto done;
    }
    mdb_set_bind_size(mdb, MDB_BIND_SIZE);
    table = mdb_read_table_by_name(mdb, (char *)name, MDB_TABLE);
    if (!table || !mdb_read_columns(table)) {
        *error = oasp_mdb_copy("MDB table does not exist", 24);
        goto done;
    }
    result->column_count = table->num_cols;
    result->columns = calloc(result->column_count, sizeof(*result->columns));
    bindings = calloc(result->column_count, sizeof(*bindings));
    lengths = calloc(result->column_count, sizeof(*lengths));
    if (!result->columns || !bindings || !lengths)
        goto done;
    for (size_t i = 0; i < result->column_count; i++) {
        MdbColumn *column = g_ptr_array_index(table->columns, i);
        result->columns[i].name = oasp_mdb_copy(column->name, strlen(column->name));
        result->columns[i].type = column->col_type;
        bindings[i] = calloc(MDB_BIND_SIZE + 1, 1);
        if (!result->columns[i].name || !bindings[i] || mdb_bind_column(table, (int)i + 1, bindings[i], &lengths[i]) == -1)
            goto done;
    }
    mdb_rewind_table(table);
    while (mdb_fetch_row(table)) {
        if (result->row_count == row_capacity) {
            size_t next_capacity = row_capacity ? row_capacity * 2 : 16;
            oasp_mdb_source_cell *next = realloc(result->cells, next_capacity * result->column_count * sizeof(*result->cells));
            if (!next)
                goto done;
            result->cells = next;
            row_capacity = next_capacity;
        }
        memset(&result->cells[result->row_count * result->column_count], 0, result->column_count * sizeof(*result->cells));
        result->row_count++;
        for (size_t i = 0; i < result->column_count; i++) {
            oasp_mdb_source_cell *cell = &result->cells[(result->row_count - 1) * result->column_count + i];
            size_t length = lengths[i] > 0 ? (size_t)lengths[i] : 0;
            cell->text = oasp_mdb_copy(bindings[i], length);
            cell->is_null = lengths[i] <= 0;
            if (!cell->text)
                goto done;
        }
    }
    ok = true;
done:
    if (!ok && !*error)
        *error = oasp_mdb_copy("out of memory while reading MDB", 31);
    if (bindings) {
        for (size_t i = 0; i < result->column_count; i++)
            free(bindings[i]);
    }
    free(bindings);
    free(lengths);
    if (table)
        mdb_free_tabledef(table);
    if (mdb)
        mdb_close(mdb);
    if (!ok)
        oasp_mdb_table_dispose(result);
    return ok;
}

static bool oasp_mdb_load_derived_table(const char *path, const char *sql, oasp_mdb_table *table, char **error) {
    oasp_mdb_result result;
    memset(&result, 0, sizeof(result));
    memset(table, 0, sizeof(*table));
    if (!oasp_mdb_execute(path, sql, &result)) {
        *error = result.error;
        result.error = NULL;
        oasp_mdb_result_dispose(&result);
        return false;
    }
    table->column_count = result.column_count;
    table->row_count = result.row_count;
    table->columns = calloc(table->column_count, sizeof(*table->columns));
    table->cells = calloc(table->row_count * table->column_count, sizeof(*table->cells));
    if (!table->columns || (table->row_count && !table->cells))
        goto fail;
    for (size_t column = 0; column < table->column_count; column++) {
        table->columns[column].name = oasp_mdb_copy(result.columns[column], strlen(result.columns[column]));
        table->columns[column].type = result.row_count && result.cells[column].kind == OASP_MDB_INTEGER
                                      ? MDB_LONGINT : MDB_TEXT;
        if (!table->columns[column].name)
            goto fail;
    }
    for (size_t cell = 0; cell < table->row_count * table->column_count; cell++) {
        table->cells[cell].text = oasp_mdb_copy(result.cells[cell].text, strlen(result.cells[cell].text));
        table->cells[cell].is_null = result.cells[cell].kind == OASP_MDB_NULL;
        if (!table->cells[cell].text)
            goto fail;
    }
    oasp_mdb_result_dispose(&result);
    return true;
fail:
    oasp_mdb_result_dispose(&result);
    oasp_mdb_table_dispose(table);
    *error = oasp_mdb_copy("out of memory while materializing derived table", 46);
    return false;
}

static int oasp_mdb_column_index(const oasp_mdb_table *table, const char *name) {
    const char *suffix = oasp_mdb_column_suffix(name);
    for (size_t i = 0; i < table->column_count; i++)
        if (!strcasecmp(table->columns[i].name, suffix))
            return (int)i;
    return -1;
}

static bool oasp_mdb_targets_table(const char *column, const char *table) {
    const char *dot = strrchr(column, '.');
    if (!dot)
        return false;
    size_t length = (size_t)(dot - column);
    return table && strlen(table) == length && !strncasecmp(column, table, length);
}

static const oasp_mdb_source_cell *oasp_mdb_resolve_cell(const oasp_mdb_query *query, const oasp_mdb_table *left, const oasp_mdb_table *right, size_t left_row, size_t right_row,
                                                         const char *name, int *type) {
    const oasp_mdb_table *table = left;
    size_t row = left_row;
    int index;
    if (right && (oasp_mdb_targets_table(name, query->right_table) ||
                  oasp_mdb_targets_table(name, query->right_alias))) {
        table = right;
        row = right_row;
    } else if (right && !oasp_mdb_targets_table(name, query->left_table) &&
               !oasp_mdb_targets_table(name, query->left_alias) &&
               oasp_mdb_column_index(left, name) < 0) {
        table = right;
        row = right_row;
    }
    index = oasp_mdb_column_index(table, name);
    if (index < 0)
        return NULL;
    if (type)
        *type = table->columns[index].type;
    return &table->cells[row * table->column_count + (size_t)index];
}

static bool oasp_mdb_boolean_equal(const char *actual, const char *expected) {
    bool lhs = !strcasecmp(actual, "true") || !strcmp(actual, "1") || !strcmp(actual, "-1") || !strcasecmp(actual, "yes");
    bool rhs = !strcasecmp(expected, "true") || !strcmp(expected, "1") || !strcmp(expected, "-1") || !strcasecmp(expected, "yes");
    return lhs == rhs;
}

static bool oasp_mdb_number(const char *text, double *number) {
    char *end = NULL;
    errno = 0;
    *number = strtod(text, &end);
    return errno == 0 && end && end != text && *end == '\0';
}

static int oasp_mdb_compare(const char *left, const char *right) {
    double lhs;
    double rhs;
    if (oasp_mdb_number(left, &lhs) && oasp_mdb_number(right, &rhs))
        return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
    return strcasecmp(left, right);
}

static oasp_mdb_value_kind oasp_mdb_kind(int type, bool is_null);

static char *oasp_mdb_unquote_copy(const char *value) {
    size_t length = strlen(value);
    if (length >= 2 && value[0] == '\'' && value[length - 1] == '\'')
        return oasp_mdb_copy(value + 1, length - 2);
    return oasp_mdb_copy(value, length);
}

static bool oasp_mdb_projection_function(const oasp_mdb_query *query, const oasp_mdb_table *left, const oasp_mdb_table *right, oasp_mdb_match match, const char *projection,
                                         char **text, oasp_mdb_value_kind *kind) {
    const char *open = strchr(projection, '(');
    size_t length = strlen(projection);
    char **arguments = NULL;
    size_t argument_count = 0;
    const oasp_mdb_source_cell *cell = NULL;
    char *value = NULL;
    bool result = false;
    if (!open || length < 4 || projection[length - 1] != ')')
        return false;
    if (!oasp_mdb_split_list(open + 1, projection + length - 1, &arguments, &argument_count) || !argument_count)
        goto done;
    cell = oasp_mdb_resolve_cell(query, left, right, match.left, match.right, arguments[0], NULL);
    value = cell ? oasp_mdb_copy(cell->text, strlen(cell->text)) : oasp_mdb_unquote_copy(arguments[0]);
    if (!value)
        goto done;
    if (!strncasecmp(projection, "ucase(", 6) || !strncasecmp(projection, "lcase(", 6)) {
        for (size_t i = 0; value[i]; i++)
            value[i] = !strncasecmp(projection, "ucase(", 6)
                       ? (char)toupper((unsigned char)value[i]) : (char)tolower((unsigned char)value[i]);
        *text = value;
        value = NULL;
        *kind = OASP_MDB_STRING;
        result = true;
    } else if (!strncasecmp(projection, "trim(", 5)) {
        char *start = value;
        char *end = value + strlen(value);
        while (*start && isspace((unsigned char)*start))
            start++;
        while (end > start && isspace((unsigned char)end[-1]))
            end--;
        *text = oasp_mdb_copy(start, (size_t)(end - start));
        *kind = OASP_MDB_STRING;
        result = *text != NULL;
    } else if (!strncasecmp(projection, "len(", 4)) {
        char buffer[32];
        int written = snprintf(buffer, sizeof(buffer), "%zu", strlen(value));
        *text = written > 0 ? oasp_mdb_copy(buffer, (size_t)written) : NULL;
        *kind = OASP_MDB_INTEGER;
        result = *text != NULL;
    } else if (!strncasecmp(projection, "left(", 5) || !strncasecmp(projection, "right(", 6)) {
        long requested;
        char *end;
        size_t take;
        if (argument_count != 2 || (requested = strtol(arguments[1], &end, 10), *end || requested < 0))
            goto done;
        take = (size_t)requested < strlen(value) ? (size_t)requested : strlen(value);
        *text = !strncasecmp(projection, "left(", 5) ? oasp_mdb_copy(value, take)
                                                      : oasp_mdb_copy(value + strlen(value) - take, take);
        *kind = OASP_MDB_STRING;
        result = *text != NULL;
    } else if (!strncasecmp(projection, "mid(", 4)) {
        long start;
        long requested;
        char *end;
        size_t offset;
        size_t take;
        if (argument_count != 3 || (start = strtol(arguments[1], &end, 10), *end || start < 1)
            || (requested = strtol(arguments[2], &end, 10), *end || requested < 0))
            goto done;
        offset = (size_t)(start - 1) < strlen(value) ? (size_t)(start - 1) : strlen(value);
        take = (size_t)requested < strlen(value) - offset ? (size_t)requested : strlen(value) - offset;
        *text = oasp_mdb_copy(value + offset, take);
        *kind = OASP_MDB_STRING;
        result = *text != NULL;
    } else if (!strncasecmp(projection, "nz(", 3) && argument_count == 2) {
        if (cell && cell->is_null) {
            free(value);
            value = oasp_mdb_unquote_copy(arguments[1]);
            if (!value)
                goto done;
        }
        *text = value;
        value = NULL;
        *kind = OASP_MDB_STRING;
        result = true;
    }
done:
    free(value);
    for (size_t i = 0; i < argument_count; i++)
        free(arguments[i]);
    free(arguments);
    return result;
}

static bool oasp_mdb_like(const char *text, const char *pattern) {
    while (*pattern) {
        if (*pattern == '*' || *pattern == '%') {
            while (*pattern == '*' || *pattern == '%')
                pattern++;
            if (!*pattern)
                return true;
            while (*text) {
                if (oasp_mdb_like(text, pattern))
                    return true;
                text++;
            }
            return false;
        }
        if (*pattern == '?' || *pattern == '_') {
            if (!*text)
                return false;
        } else if (tolower((unsigned char)*text) != tolower((unsigned char)*pattern)) {
            return false;
        }
        text++;
        pattern++;
    }
    return !*text;
}

static bool oasp_mdb_list_contains(const char *list, const char *value) {
    char **items = NULL;
    size_t count = 0;
    bool found = false;
    if (!oasp_mdb_split_list(list, list + strlen(list), &items, &count))
        return false;
    for (size_t i = 0; i < count; i++) {
        size_t length = strlen(items[i]);
        const char *item = items[i];
        if (length >= 2 && item[0] == '\'' && item[length - 1] == '\'') {
            item++;
            length -= 2;
        }
        if (strlen(value) == length && !strncasecmp(value, item, length)) {
            found = true;
            break;
        }
    }
    for (size_t i = 0; i < count; i++)
        free(items[i]);
    free(items);
    return found;
}

static bool oasp_mdb_condition_matches_cell(const oasp_mdb_condition *condition, const oasp_mdb_source_cell *cell, int type) {
    int comparison;
    if (condition->kind == OASP_MDB_CONDITION_NULL)
        return cell->is_null;
    if (condition->kind == OASP_MDB_CONDITION_NOT_NULL)
        return !cell->is_null;
    if (cell->is_null)
        return false;
    if (condition->kind == OASP_MDB_CONDITION_LIKE)
        return oasp_mdb_like(cell->text, condition->value);
    if (condition->kind == OASP_MDB_CONDITION_BETWEEN)
        return oasp_mdb_compare(cell->text, condition->value) >= 0 &&
               oasp_mdb_compare(cell->text, condition->second_value) <= 0;
    if (condition->kind == OASP_MDB_CONDITION_IN || condition->kind == OASP_MDB_CONDITION_NOT_IN) {
        bool found = oasp_mdb_list_contains(condition->value, cell->text);
        return condition->kind == OASP_MDB_CONDITION_IN ? found : !found;
    }
    comparison = type == MDB_BOOL
                     ? (oasp_mdb_boolean_equal(cell->text, condition->value) ? 0 : 1)
                     : oasp_mdb_compare(cell->text, condition->value);
    return condition->kind == OASP_MDB_CONDITION_EQUAL ? comparison == 0
         : condition->kind == OASP_MDB_CONDITION_NOT_EQUAL ? comparison != 0
         : condition->kind == OASP_MDB_CONDITION_LESS ? comparison < 0
         : condition->kind == OASP_MDB_CONDITION_LESS_EQUAL ? comparison <= 0
         : condition->kind == OASP_MDB_CONDITION_GREATER ? comparison > 0
         : comparison >= 0;
}

static void oasp_mdb_expression_trim(const char **begin, const char **end) {
    while (*begin < *end && isspace((unsigned char)**begin))
        (*begin)++;
    while (*end > *begin && isspace((unsigned char)(*end)[-1]))
        (*end)--;
}

static void oasp_mdb_expression_strip_parentheses(const char **begin, const char **end) {
    for (;;) {
        const char *cursor;
        int depth = 0;
        bool quoted = false;
        oasp_mdb_expression_trim(begin, end);
        if (*end - *begin < 2 || **begin != '(' || (*end)[-1] != ')')
            return;
        for (cursor = *begin; cursor < *end; cursor++) {
            if (*cursor == '\'' && (!quoted || cursor + 1 == *end || cursor[1] != '\'')) {
                quoted = !quoted;
            } else if (!quoted && *cursor == '(') {
                depth++;
            } else if (!quoted && *cursor == ')' && --depth == 0) {
                if (cursor + 1 != *end)
                    return;
                (*begin)++;
                (*end)--;
                break;
            }
        }
        if (cursor == *end)
            return;
    }
}

static const char *oasp_mdb_expression_separator(const char *begin, const char *end, const char *separator, bool skip_between) {
    size_t length = strlen(separator);
    bool quoted = false;
    bool between_pending = false;
    int depth = 0;
    for (const char *cursor = begin; cursor + length <= end; cursor++) {
        if (*cursor == '\'' && (!quoted || cursor + 1 == end || cursor[1] != '\'')) {
            quoted = !quoted;
            continue;
        }
        if (quoted)
            continue;
        if (*cursor == '(') {
            depth++;
            continue;
        }
        if (*cursor == ')' && depth > 0) {
            depth--;
            continue;
        }
        if (depth)
            continue;
        if (skip_between && cursor + 9 <= end && !strncasecmp(cursor, " between ", 9)) {
            between_pending = true;
            cursor += 8;
            continue;
        }
        if (!strncasecmp(cursor, separator, length)) {
            if (between_pending) {
                between_pending = false;
                cursor += length - 1;
                continue;
            }
            return cursor;
        }
    }
    return NULL;
}

static bool oasp_mdb_expression_matches(const oasp_mdb_query *query, const oasp_mdb_table *left, const oasp_mdb_table *right, size_t left_row, size_t right_row,
                                        const char *begin, const char *end) {
    const char *separator;
    oasp_mdb_condition *conditions = NULL;
    size_t count = 0;
    bool matched = false;
    int type = 0;
    const oasp_mdb_source_cell *cell;
    oasp_mdb_expression_strip_parentheses(&begin, &end);
    if (begin == end)
        return false;
    separator = oasp_mdb_expression_separator(begin, end, " or ", false);
    if (separator)
        return oasp_mdb_expression_matches(query, left, right, left_row, right_row, begin, separator)
            || oasp_mdb_expression_matches(query, left, right, left_row, right_row, separator + 4, end);
    separator = oasp_mdb_expression_separator(begin, end, " and ", true);
    if (separator)
        return oasp_mdb_expression_matches(query, left, right, left_row, right_row, begin, separator)
            && oasp_mdb_expression_matches(query, left, right, left_row, right_row, separator + 5, end);
    if (!oasp_mdb_add_condition(&conditions, &count, begin, end) || count != 1)
        goto done;
    if (conditions[0].kind == OASP_MDB_CONDITION_EXISTS
        || !strncasecmp(conditions[0].value, "select ", 7)) {
        matched = true;
        goto done;
    }
    cell = oasp_mdb_resolve_cell(query, left, right, left_row, right_row, conditions[0].column, &type);
    matched = cell && oasp_mdb_condition_matches_cell(&conditions[0], cell, type);
done:
    for (size_t i = 0; i < count; i++) {
        free(conditions[i].column);
        free(conditions[i].value);
        free(conditions[i].second_value);
    }
    free(conditions);
    return matched;
}

static bool oasp_mdb_row_matches(const oasp_mdb_query *query, const oasp_mdb_table *left, const oasp_mdb_table *right, size_t left_row, size_t right_row) {
    if (query->where_expression) {
        return oasp_mdb_expression_matches(query, left, right, left_row, right_row,
                                           query->where_expression,
                                           query->where_expression + strlen(query->where_expression));
    }
    bool group = true;
    bool result = false;
    for (size_t i = 0; i < query->condition_count; i++) {
        int type = 0;
        const oasp_mdb_condition *condition = &query->conditions[i];
        bool matched;
        if (condition->kind == OASP_MDB_CONDITION_EXISTS || !strncasecmp(condition->value, "select ", 7))
            continue;
        const oasp_mdb_source_cell *cell = oasp_mdb_resolve_cell(query, left, right, left_row, right_row, condition->column, &type);
        if (!cell)
            return false;
        matched = oasp_mdb_condition_matches_cell(condition, cell, type);
        if (condition->disjunction) {
            result = result || group;
            group = matched;
        } else {
            group = group && matched;
        }
    }
    return result || group;
}

static bool oasp_mdb_condition_matches_value(const oasp_mdb_condition *condition, const char *value) {
    oasp_mdb_source_cell cell = {(char *)value, false};
    if (condition->kind == OASP_MDB_CONDITION_NULL || condition->kind == OASP_MDB_CONDITION_NOT_NULL ||
        condition->kind == OASP_MDB_CONDITION_LIKE || condition->kind == OASP_MDB_CONDITION_BETWEEN ||
        condition->kind == OASP_MDB_CONDITION_IN || condition->kind == OASP_MDB_CONDITION_NOT_IN)
        return oasp_mdb_condition_matches_cell(condition, &cell, 0);
    int comparison = oasp_mdb_compare(value, condition->value);
    return condition->kind == OASP_MDB_CONDITION_EQUAL ? comparison == 0
         : condition->kind == OASP_MDB_CONDITION_NOT_EQUAL ? comparison != 0
         : condition->kind == OASP_MDB_CONDITION_LESS ? comparison < 0
         : condition->kind == OASP_MDB_CONDITION_LESS_EQUAL ? comparison <= 0
         : condition->kind == OASP_MDB_CONDITION_GREATER ? comparison > 0
         : condition->kind == OASP_MDB_CONDITION_GREATER_EQUAL ? comparison >= 0
         : false;
}

static bool oasp_mdb_matches_same_group(const oasp_mdb_query *query, const oasp_mdb_table *left, const oasp_mdb_table *right, oasp_mdb_match first, oasp_mdb_match candidate) {
    for (size_t i = 0; i < query->group_count; i++) {
        const oasp_mdb_source_cell *lhs = oasp_mdb_resolve_cell(query, left, right, first.left, first.right, query->groups[i], NULL);
        const oasp_mdb_source_cell *rhs = oasp_mdb_resolve_cell(query, left, right, candidate.left, candidate.right, query->groups[i], NULL);
        if (!lhs || !rhs || lhs->is_null != rhs->is_null || (!lhs->is_null && oasp_mdb_compare(lhs->text, rhs->text)))
            return false;
    }
    return true;
}

static bool oasp_mdb_projection_value(const oasp_mdb_query *query, const oasp_mdb_table *left, const oasp_mdb_table *right, const oasp_mdb_match *matches, size_t first,
                                      size_t end, const char *projection, char **text, oasp_mdb_value_kind *kind) {
    const char *argument = NULL;
    oasp_mdb_aggregate_kind aggregate = oasp_mdb_aggregate(projection, &argument);
    if (!aggregate) {
        int type = 0;
        const oasp_mdb_source_cell *cell = oasp_mdb_resolve_cell(query, left, right, matches[first].left, matches[first].right, projection, &type);
        if (!cell)
            return oasp_mdb_projection_function(query, left, right, matches[first], projection, text, kind);
        *text = oasp_mdb_copy(cell->text, strlen(cell->text));
        *kind = oasp_mdb_kind(type, cell->is_null);
        return *text != NULL;
    }
    if (aggregate == OASP_MDB_AGGREGATE_COUNT) {
        char buffer[32];
        int length = snprintf(buffer, sizeof(buffer), "%zu", end - first);
        *text = length > 0 ? oasp_mdb_copy(buffer, (size_t)length) : NULL;
        *kind = OASP_MDB_INTEGER;
        return *text != NULL;
    }
    char *argument_name = oasp_mdb_trim_copy(argument, projection + strlen(projection) - 1);
    const oasp_mdb_source_cell *chosen = NULL;
    double total = 0.0;
    size_t count = 0;
    if (!argument_name)
        return false;
    for (size_t i = first; i < end; i++) {
        const oasp_mdb_source_cell *cell = oasp_mdb_resolve_cell(query, left, right, matches[i].left, matches[i].right, argument_name, NULL);
        double number;
        if (!cell || cell->is_null)
            continue;
        if (aggregate == OASP_MDB_AGGREGATE_SUM || aggregate == OASP_MDB_AGGREGATE_AVG) {
            if (!oasp_mdb_number(cell->text, &number)) {
                free(argument_name);
                return false;
            }
            total += number;
            count++;
        } else if (!chosen || (aggregate == OASP_MDB_AGGREGATE_MIN ? oasp_mdb_compare(cell->text, chosen->text) < 0
                                                                  : oasp_mdb_compare(cell->text, chosen->text) > 0)) {
            chosen = cell;
        }
    }
    free(argument_name);
    if (aggregate == OASP_MDB_AGGREGATE_SUM || aggregate == OASP_MDB_AGGREGATE_AVG) {
        char buffer[64];
        int length = snprintf(buffer, sizeof(buffer), "%.15g", aggregate == OASP_MDB_AGGREGATE_AVG && count ? total / count : total);
        *text = length > 0 ? oasp_mdb_copy(buffer, (size_t)length) : NULL;
        *kind = OASP_MDB_FLOAT;
    } else {
        *text = oasp_mdb_copy(chosen ? chosen->text : "", chosen ? strlen(chosen->text) : 0);
        *kind = chosen ? OASP_MDB_STRING : OASP_MDB_NULL;
    }
    return *text != NULL;
}

static oasp_mdb_value_kind oasp_mdb_kind(int type, bool is_null) {
    if (is_null)
        return OASP_MDB_NULL;
    if (type == MDB_BOOL)
        return OASP_MDB_BOOL;
    if (type == MDB_BYTE || type == MDB_INT || type == MDB_LONGINT)
        return OASP_MDB_INTEGER;
    if (type == MDB_MONEY || type == MDB_FLOAT || type == MDB_DOUBLE || type == MDB_NUMERIC)
        return OASP_MDB_FLOAT;
    return OASP_MDB_STRING;
}

static char *oasp_mdb_hex_list(const char *const *items, size_t count, bool value_prefix) {
    static const char digits[] = "0123456789abcdef";
    size_t encoded_length = 0;
    for (size_t i = 0; i < count; i++) {
        size_t length = items[i] ? strlen(items[i]) : 0;
        if (!items[i] || length > (SIZE_MAX - encoded_length - 2) / 2)
            return NULL;
        encoded_length += (length + (value_prefix ? 1 : 0)) * 2 + (i ? 1 : 0);
    }
    char *encoded = malloc(encoded_length + 1);
    if (!encoded)
        return NULL;
    size_t offset = 0;
    for (size_t i = 0; i < count; i++) {
        if (i) {
            encoded[offset++] = 31;
        }
        if (value_prefix) {
            encoded[offset++] = '7';
            encoded[offset++] = '3';
        }
        for (const unsigned char *cursor = (const unsigned char *)items[i]; *cursor; cursor++) {
            encoded[offset++] = digits[*cursor >> 4];
            encoded[offset++] = digits[*cursor & 15];
        }
    }
    encoded[offset] = '\0';
    return encoded;
}

static bool oasp_mdb_native_status(char *native_error, char **error) {
    if (native_error && *native_error) {
        if (error)
            *error = native_error;
        else
            free(native_error);
        return false;
    }
    free(native_error);
    return true;
}

static char *oasp_mdb_literal(const char *text) {
    char *value;
    size_t length;
    if (!text)
        return NULL;
    while (isspace((unsigned char)*text))
        text++;
    length = strlen(text);
    while (length && (isspace((unsigned char)text[length - 1]) || text[length - 1] == ';'))
        length--;
    value = oasp_mdb_copy(text, length);
    if (!value)
        return NULL;
    if (length >= 2 && value[0] == '\'' && value[length - 1] == '\'') {
        size_t out = 0;
        for (size_t i = 1; i + 1 < length; i++) {
            value[out++] = value[i];
            if (value[i] == '\'' && i + 2 < length && value[i + 1] == '\'')
                i++;
        }
        value[out] = '\0';
    } else if (!strcasecmp(value, "null")) {
        value[0] = '\0';
    }
    return value;
}

static bool oasp_mdb_parse_assignments(const char *begin, const char *end, char ***columns, char ***values, size_t *count) {
    char **items = NULL;
    size_t item_count = 0;
    bool ok = false;
    if (!oasp_mdb_split_list(begin, end, &items, &item_count))
        goto done;
    for (size_t i = 0; i < item_count; i++) {
        char *equals = strchr(items[i], '=');
        char *column;
        char *value;
        if (!equals)
            goto done;
        column = oasp_mdb_trim_copy(items[i], equals);
        value = oasp_mdb_trim_copy(equals + 1, items[i] + strlen(items[i]));
        char **next_columns;
        char **next_values;
        if (!column || !*column || !value) {
            free(column);
            free(value);
            goto done;
        }
        next_columns = realloc(*columns, (*count + 1) * sizeof(**columns));
        if (!next_columns) {
            free(column);
            free(value);
            goto done;
        }
        *columns = next_columns;
        next_values = realloc(*values, (*count + 1) * sizeof(**values));
        if (!next_values) {
            free(column);
            free(value);
            goto done;
        }
        *values = next_values;
        (*columns)[*count] = column;
        (*values)[*count] = value;
        (*count)++;
    }
    ok = item_count != 0;
done:
    for (size_t i = 0; i < item_count; i++)
        free(items[i]);
    free(items);
    return ok;
}

static void oasp_mdb_free_strings(char **items, size_t count) {
    for (size_t i = 0; i < count; i++)
        free(items[i]);
    free(items);
}

static char *oasp_mdb_typed_result_value(const oasp_mdb_value *value) {
    char prefix;
    size_t length;
    char *encoded;
    if (!value || !value->text)
        return NULL;
    prefix = value->kind == OASP_MDB_NULL ? 'n'
           : value->kind == OASP_MDB_STRING ? 's' : 'v';
    length = prefix == 'n' ? 0 : strlen(value->text);
    encoded = malloc(length + 2);
    if (!encoded)
        return NULL;
    encoded[0] = prefix;
    if (length)
        memcpy(encoded + 1, value->text, length);
    encoded[length + 1] = '\0';
    return encoded;
}

bool oasp_mdb_execute_command(const char *path, const char *sql, char **error) {
    const char *cursor = sql;
    const char *where;
    const char *set;
    const char *values;
    const char *open;
    const char *close;
    char *table = NULL;
    char **columns = NULL;
    char **assigned = NULL;
    size_t count = 0;
    char *encoded_columns = NULL;
    char *encoded_values = NULL;
    char *encoded_where_sql = NULL;
    char *native_error = NULL;
    bool ok = false;
    if (error)
        *error = NULL;
    while (cursor && isspace((unsigned char)*cursor))
        cursor++;
    if (!path || !cursor || !*cursor)
        goto invalid;
    if (!strncasecmp(cursor, "create table", 12)) {
        char ddl_error[512] = "";
        if (!oasp_mdb_create_table_file(path, cursor, ddl_error, sizeof(ddl_error))) {
            if (error)
                *error = oasp_mdb_copy(ddl_error[0] ? ddl_error : "MDB CREATE TABLE failed", strlen(ddl_error[0] ? ddl_error : "MDB CREATE TABLE failed"));
            return false;
        }
        return true;
    }
    if (!strncasecmp(cursor, "create ", 7)) {
        char ddl_error[512] = "";
        if (!oasp_mdb_create_index_file(path, cursor, ddl_error, sizeof(ddl_error))) {
            if (error)
                *error = oasp_mdb_copy(ddl_error[0] ? ddl_error : "MDB CREATE INDEX failed",
                                       strlen(ddl_error[0] ? ddl_error : "MDB CREATE INDEX failed"));
            return false;
        }
        return true;
    }
    if (!strncasecmp(cursor, "update ", 7)) {
        set = oasp_mdb_find_phrase(cursor + 7, " set ");
        where = oasp_mdb_find_phrase(cursor + 7, " where ");
        if (!set || (where && set >= where))
            goto invalid;
        table = oasp_mdb_trim_copy(cursor + 7, set);
        if (!table || !*table || !oasp_mdb_parse_assignments(set + 5, where ? where : cursor + strlen(cursor), &columns, &assigned, &count))
            goto invalid;
        encoded_columns = oasp_mdb_hex_list((const char *const *)columns, count, false);
        encoded_values = oasp_mdb_hex_list((const char *const *)assigned, count, false);
        encoded_where_sql = oasp_mdb_hex_list((const char *const *)&(const char *){where ? where + 7 : ""}, 1, false);
        if (!encoded_columns || !encoded_values || !encoded_where_sql)
            goto oom;
        native_error = oasp_mdb_native_update_where_raw(path, table, encoded_where_sql, encoded_columns, encoded_values);
        ok = oasp_mdb_native_status(native_error, error);
        native_error = NULL;
        goto done;
    }
    if (!strncasecmp(cursor, "insert into ", 12)) {
        open = strchr(cursor + 12, '(');
        close = open ? strchr(open + 1, ')') : NULL;
        values = close ? oasp_mdb_find_phrase(close + 1, " values") : NULL;
        if (!open || !close)
            goto invalid;
        table = oasp_mdb_trim_copy(cursor + 12, open);
        if (!table || !oasp_mdb_split_list(open + 1, close, &columns, &count))
            goto invalid;
        if (!values) {
            const char *select = close + 1;
            oasp_mdb_result result = {0};
            while (isspace((unsigned char)*select))
                select++;
            if (strncasecmp(select, "select ", 7) || !oasp_mdb_execute(path, select, &result)
                || result.column_count != count) {
                if (error && result.error) {
                    *error = result.error, result.error = NULL;
                    oasp_mdb_result_dispose(&result);
                    goto done;
                }
                oasp_mdb_result_dispose(&result);
                goto invalid;
            }
            encoded_columns = oasp_mdb_hex_list((const char *const *)columns, count, false);
            if (!encoded_columns)
                goto oom;
            for (size_t row = 0; row < result.row_count; row++) {
                if (assigned)
                    oasp_mdb_free_strings(assigned, count);
                assigned = calloc(count, sizeof(*assigned));
                if (!assigned)
                    goto oom;
                for (size_t column = 0; column < count; column++) {
                    assigned[column] = oasp_mdb_typed_result_value(
                        &result.cells[row * result.column_count + column]);
                    if (!assigned[column])
                        goto oom;
                }
                free(encoded_values);
                encoded_values = oasp_mdb_hex_list((const char *const *)assigned, count, false);
                if (!encoded_values)
                    goto oom;
                native_error = oasp_mdb_native_insert_raw(path, table, encoded_columns, encoded_values);
                if (!oasp_mdb_native_status(native_error, error)) {
                    native_error = NULL;
                    oasp_mdb_result_dispose(&result);
                    goto done;
                }
                native_error = NULL;
            }
            oasp_mdb_result_dispose(&result);
            ok = true;
            goto done;
        }
        open = strchr(values, '(');
        close = open ? strrchr(open + 1, ')') : NULL;
        {
            size_t value_count = 0;
            if (!open || !close || !oasp_mdb_split_list(open + 1, close, &assigned, &value_count) || value_count != count)
                goto invalid;
        }
        for (size_t i = 0; i < count; i++) {
            char *literal = oasp_mdb_literal(assigned[i]);
            free(assigned[i]);
            assigned[i] = literal;
            if (!literal)
                goto oom;
        }
        encoded_columns = oasp_mdb_hex_list((const char *const *)columns, count, false);
        encoded_values = oasp_mdb_hex_list((const char *const *)assigned, count, true);
        if (!encoded_columns || !encoded_values)
            goto oom;
        native_error = oasp_mdb_native_insert_raw(path, table, encoded_columns, encoded_values);
        ok = oasp_mdb_native_status(native_error, error);
        native_error = NULL;
        goto done;
    }
    if (!strncasecmp(cursor, "delete from ", 12)) {
        where = oasp_mdb_find_phrase(cursor + 12, " where ");
        table = oasp_mdb_trim_copy(cursor + 12, where ? where : cursor + strlen(cursor));
        if (!table || !*table)
            goto invalid;
        encoded_where_sql = oasp_mdb_hex_list((const char *const *)&(const char *){where ? where + 7 : ""}, 1, false);
        if (!encoded_where_sql)
            goto oom;
        native_error = oasp_mdb_native_delete_where_raw(path, table, encoded_where_sql);
        ok = oasp_mdb_native_status(native_error, error);
        native_error = NULL;
        goto done;
    }
    if (!strncasecmp(cursor, "alter table", 11)) {
        char ddl_error[512] = "";
        if (!oasp_mdb_alter_table_file(path, cursor, ddl_error, sizeof(ddl_error))) {
            if (error)
                *error = oasp_mdb_copy(ddl_error[0] ? ddl_error : "MDB ALTER TABLE failed", strlen(ddl_error[0] ? ddl_error : "MDB ALTER TABLE failed"));
            return false;
        }
        return true;
    }
    if (!strncasecmp(cursor, "drop ", 5)) {
        char ddl_error[512] = "";
        int is_index = !strncasecmp(cursor, "drop index", 10);
        if (!(is_index ? oasp_mdb_drop_index_file(path, cursor, ddl_error, sizeof(ddl_error))
                       : oasp_mdb_drop_table_file(path, cursor, ddl_error, sizeof(ddl_error)))) {
            if (error)
                *error = oasp_mdb_copy(ddl_error[0] ? ddl_error : (is_index ? "MDB DROP INDEX failed" : "MDB DROP TABLE failed"),
                                       strlen(ddl_error[0] ? ddl_error : (is_index ? "MDB DROP INDEX failed" : "MDB DROP TABLE failed")));
            return false;
        }
        return true;
    }
invalid:
    if (error)
        *error = oasp_mdb_copy("unsupported MDB command syntax", 29);
    goto done;
oom:
    if (error)
        *error = oasp_mdb_copy("out of memory while parsing MDB command", 39);
done:
    free(native_error);
    free(table);
    oasp_mdb_free_strings(columns, count);
    oasp_mdb_free_strings(assigned, count);
    free(encoded_columns);
    free(encoded_values);
    free(encoded_where_sql);
    return ok;
}

bool oasp_mdb_update_record(const char *path, const char *select_sql, const char *const *columns, const char *const *values, size_t count,
                            char **error) {
    oasp_mdb_query query;
    char *encoded_columns = NULL;
    char *encoded_values = NULL;
    char *encoded_where_sql = NULL;
    char *native_error = NULL;
    bool ok = false;
    if (error)
        *error = NULL;
    if (!path || !select_sql || !columns || !values || !count || !oasp_mdb_parse_query(select_sql, &query) ||
        query.right_table || !query.where_expression || !query.condition_count) {
        if (error)
            *error = oasp_mdb_copy("MDB Recordset.Update requires a single-table SELECT with a WHERE predicate", 74);
        return false;
    }
    for (size_t i = 0; i < query.condition_count; i++) {
        if (query.conditions[i].kind != OASP_MDB_CONDITION_EQUAL || query.conditions[i].disjunction) {
            if (error)
                *error = oasp_mdb_copy("MDB Recordset.Update requires conjunctive equality predicates", 61);
            oasp_mdb_query_dispose(&query);
            return false;
        }
    }
    encoded_columns = oasp_mdb_hex_list(columns, count, false);
    encoded_values = oasp_mdb_hex_list(values, count, true);
    encoded_where_sql = oasp_mdb_hex_list((const char *const *)&query.where_expression, 1, false);
    if (!encoded_columns || !encoded_values || !encoded_where_sql) {
        if (error)
            *error = oasp_mdb_copy("out of memory while encoding MDB update", 39);
        goto done;
    }
    native_error = oasp_mdb_native_update_typed_where_raw(path, query.left_table, encoded_where_sql,
                                                           encoded_columns, encoded_values);
    if (native_error && *native_error) {
        if (error)
            *error = native_error;
        else
            free(native_error);
        native_error = NULL;
        goto done;
    }
    free(native_error);
    ok = true;
done:
    free(encoded_columns);
    free(encoded_values);
    free(encoded_where_sql);
    oasp_mdb_query_dispose(&query);
    return ok;
}

bool oasp_mdb_insert_record(const char *path, const char *select_sql, const char *const *columns, const char *const *values, size_t count,
                            char **error) {
    oasp_mdb_query query;
    char *encoded_columns = NULL;
    char *encoded_values = NULL;
    char *native_error = NULL;
    bool ok = false;
    if (error)
        *error = NULL;
    if (!path || !select_sql || !columns || !values || !count || !oasp_mdb_parse_query(select_sql, &query) ||
        query.right_table) {
        if (error)
            *error = oasp_mdb_copy("MDB Recordset.AddNew requires a single-table SELECT source", 57);
        return false;
    }
    encoded_columns = oasp_mdb_hex_list(columns, count, false);
    encoded_values = oasp_mdb_hex_list(values, count, true);
    if (!encoded_columns || !encoded_values) {
        if (error)
            *error = oasp_mdb_copy("out of memory while encoding MDB insert", 39);
        goto done;
    }
    native_error = oasp_mdb_native_insert_raw(path, query.left_table, encoded_columns, encoded_values);
    if (native_error && *native_error) {
        if (error)
            *error = native_error;
        else
            free(native_error);
        native_error = NULL;
        goto done;
    }
    free(native_error);
    ok = true;
done:
    free(encoded_columns);
    free(encoded_values);
    oasp_mdb_query_dispose(&query);
    return ok;
}

bool oasp_mdb_delete_record(const char *path, const char *select_sql, char **error) {
    oasp_mdb_query query;
    char *encoded_where_column = NULL;
    char *encoded_where = NULL;
    char *native_error = NULL;
    bool ok = false;
    if (error)
        *error = NULL;
    if (!path || !select_sql || !oasp_mdb_parse_query(select_sql, &query) || query.right_table ||
        query.condition_count != 1 || query.conditions[0].kind != OASP_MDB_CONDITION_EQUAL) {
        if (error)
            *error = oasp_mdb_copy("MDB Recordset.Delete requires a single-table SELECT with one equality predicate", 73);
        return false;
    }
    encoded_where_column = oasp_mdb_hex_list((const char *const *)&query.conditions[0].column, 1, false);
    encoded_where = oasp_mdb_hex_list((const char *const *)&query.conditions[0].value, 1, true);
    if (!encoded_where_column || !encoded_where) {
        if (error)
            *error = oasp_mdb_copy("out of memory while encoding MDB delete", 39);
        goto done;
    }
    native_error = oasp_mdb_native_delete_raw(path, query.left_table, encoded_where_column, encoded_where);
    ok = oasp_mdb_native_status(native_error, error);
    native_error = NULL;
done:
    free(native_error);
    free(encoded_where_column);
    free(encoded_where);
    oasp_mdb_query_dispose(&query);
    return ok;
}

void oasp_mdb_result_dispose(oasp_mdb_result *result) {
    if (!result)
        return;
    for (size_t i = 0; i < result->column_count; i++)
        free(result->columns[i]);
    for (size_t i = 0; i < result->row_count * result->column_count; i++)
        free(result->cells[i].text);
    free(result->columns);
    free(result->cells);
    free(result->error);
    memset(result, 0, sizeof(*result));
}

static const char *oasp_mdb_find_top_level_union(const char *sql, bool *all) {
    bool quoted = false;
    int depth = 0;
    for (const char *cursor = sql; *cursor; cursor++) {
        if (*cursor == '\'') {
            if (quoted && cursor[1] == '\'') {
                cursor++;
                continue;
            }
            quoted = !quoted;
        } else if (!quoted && *cursor == '(') {
            depth++;
        } else if (!quoted && *cursor == ')' && depth > 0) {
            depth--;
        } else if (!quoted && !depth && (cursor == sql || isspace((unsigned char)cursor[-1]))
                   && !strncasecmp(cursor, "union", 5)
                   && (isspace((unsigned char)cursor[5]) || !cursor[5])) {
            const char *after = cursor + 5;
            while (isspace((unsigned char)*after))
                after++;
            *all = !strncasecmp(after, "all", 3) && (isspace((unsigned char)after[3]) || !after[3]);
            return cursor;
        }
    }
    return NULL;
}

static bool oasp_mdb_result_row_equal(const oasp_mdb_result *left, size_t left_row, const oasp_mdb_result *right, size_t right_row) {
    for (size_t column = 0; column < left->column_count; column++) {
        const oasp_mdb_value *lhs = &left->cells[left_row * left->column_count + column];
        const oasp_mdb_value *rhs = &right->cells[right_row * right->column_count + column];
        if (lhs->kind != rhs->kind || strcasecmp(lhs->text, rhs->text))
            return false;
    }
    return true;
}

static bool oasp_mdb_append_text(char **output, size_t *length, size_t *capacity, const char *text, size_t text_length) {
    if (*length > SIZE_MAX - text_length - 1)
        return false;
    if (*length + text_length + 1 > *capacity) {
        size_t next = *capacity ? *capacity : 64;
        while (next < *length + text_length + 1) {
            if (next > SIZE_MAX / 2)
                return false;
            next *= 2;
        }
        char *grown = realloc(*output, next);
        if (!grown)
            return false;
        *output = grown;
        *capacity = next;
    }
    memcpy(*output + *length, text, text_length);
    *length += text_length;
    (*output)[*length] = '\0';
    return true;
}

static bool oasp_mdb_append_literal(char **output, size_t *length, size_t *capacity, const oasp_mdb_source_cell *cell) {
    if (!cell->is_null && !oasp_mdb_append_text(output, length, capacity, "'", 1))
        return false;
    for (const char *cursor = cell->text; !cell->is_null && *cursor; cursor++) {
        if (!oasp_mdb_append_text(output, length, capacity, cursor, 1) ||
            (*cursor == '\'' && !oasp_mdb_append_text(output, length, capacity, "'", 1)))
            return false;
    }
    return cell->is_null ? oasp_mdb_append_text(output, length, capacity, "null", 4)
                         : oasp_mdb_append_text(output, length, capacity, "'", 1);
}

static char *oasp_mdb_bind_subquery(const char *sql, const oasp_mdb_query *query, const oasp_mdb_table *left, const oasp_mdb_table *right, oasp_mdb_match match) {
    char *output = NULL;
    size_t length = 0;
    size_t capacity = 0;
    bool quoted = false;
    for (size_t offset = 0; sql[offset];) {
        const oasp_mdb_source_cell *cell = NULL;
        size_t token_length = 0;
        if (sql[offset] == '\'') {
            quoted = !quoted;
        } else if (!quoted) {
            const oasp_mdb_table *tables[] = {left, right};
            const char *aliases[] = {query->left_alias, query->right_alias};
            const char *names[] = {query->left_table, query->right_table};
            for (size_t table = 0; table < 2 && !cell; table++) {
                if (!tables[table])
                    continue;
                for (size_t column = 0; column < tables[table]->column_count && !cell; column++) {
                    const char *prefixes[] = {aliases[table], names[table]};
                    for (size_t prefix = 0; prefix < 2 && !cell; prefix++) {
                        size_t prefix_length = prefixes[prefix] ? strlen(prefixes[prefix]) : 0;
                        const char *column_name = tables[table]->columns[column].name;
                        size_t column_length = strlen(column_name);
                        if (prefix_length && !strncasecmp(sql + offset, prefixes[prefix], prefix_length)
                            && sql[offset + prefix_length] == '.'
                            && !strncasecmp(sql + offset + prefix_length + 1, column_name, column_length)
                            && !isalnum((unsigned char)sql[offset + prefix_length + 1 + column_length])
                            && sql[offset + prefix_length + 1 + column_length] != '_') {
                            size_t row = table == 0 ? match.left : match.right;
                            cell = &tables[table]->cells[row * tables[table]->column_count + column];
                            token_length = prefix_length + 1 + column_length;
                        }
                    }
                }
            }
        }
        if (cell) {
            if (!oasp_mdb_append_literal(&output, &length, &capacity, cell)) {
                free(output);
                return NULL;
            }
            offset += token_length;
        } else if (!oasp_mdb_append_text(&output, &length, &capacity, sql + offset, 1)) {
            free(output);
            return NULL;
        } else {
            offset++;
        }
    }
    return output;
}

static bool oasp_mdb_match_subqueries(const char *path, const oasp_mdb_query *query, const oasp_mdb_table *left, const oasp_mdb_table *right, oasp_mdb_match match) {
    for (size_t i = 0; i < query->condition_count; i++) {
        const oasp_mdb_condition *condition = &query->conditions[i];
        const oasp_mdb_source_cell *cell = NULL;
        oasp_mdb_result values;
        char *bound_sql;
        bool found = false;
        if (strncasecmp(condition->value, "select ", 7))
            continue;
        if (condition->kind != OASP_MDB_CONDITION_EXISTS) {
            cell = oasp_mdb_resolve_cell(query, left, right, match.left, match.right, condition->column, NULL);
        }
        if (condition->kind != OASP_MDB_CONDITION_EXISTS && (!cell || cell->is_null))
            return false;
        memset(&values, 0, sizeof(values));
        bound_sql = oasp_mdb_bind_subquery(condition->value, query, left, right, match);
        if (!bound_sql || !oasp_mdb_execute(path, bound_sql, &values) ||
            (condition->kind != OASP_MDB_CONDITION_EXISTS && values.column_count != 1)) {
            free(bound_sql);
            oasp_mdb_result_dispose(&values);
            return false;
        }
        free(bound_sql);
        for (size_t row = 0; row < values.row_count; row++) {
            if (condition->kind == OASP_MDB_CONDITION_EXISTS ||
                oasp_mdb_compare(cell->text, values.cells[row * values.column_count].text) == 0) {
                found = true;
                break;
            }
        }
        oasp_mdb_result_dispose(&values);
        if (!found)
            return false;
    }
    return true;
}

static bool oasp_mdb_execute_select(const char *path, const char *sql, oasp_mdb_result *result) {
    oasp_mdb_query query;
    oasp_mdb_table left;
    oasp_mdb_table right;
    oasp_mdb_match *matches = NULL;
    size_t *groups = NULL;
    size_t match_count = 0;
    size_t group_count = 0;
    bool ok = false;
    memset(result, 0, sizeof(*result));
    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    if (!oasp_mdb_parse_query(sql, &query)) {
        result->error = oasp_mdb_copy("unsupported MDB SQL statement", 29);
        return false;
    }
    if (!((query.left_sql && oasp_mdb_load_derived_table(path, query.left_sql, &left, &result->error)) ||
          (!query.left_sql && oasp_mdb_load_table(path, query.left_table, &left, &result->error))) ||
        ((query.right_table || query.right_sql) &&
         !((query.right_sql && oasp_mdb_load_derived_table(path, query.right_sql, &right, &result->error)) ||
           (!query.right_sql && oasp_mdb_load_table(path, query.right_table, &right, &result->error)))))
        goto done;
    if (!oasp_mdb_expand_wildcard(&query, &left, (query.right_table || query.right_sql) ? &right : NULL))
        goto done;
    const bool has_right = query.right_table || query.right_sql;
    const size_t actual_right_rows = right.row_count;
    if (has_right && query.left_join && !oasp_mdb_table_append_null_row(&right))
        goto done;
    for (size_t i = 0; i < left.row_count; i++) {
        size_t right_count = has_right ? right.row_count : 1;
        bool join_matched = false;
        for (size_t j = 0; j < right_count; j++) {
            if (has_right) {
                if (j == actual_right_rows) {
                    if (!query.left_join || join_matched)
                        continue;
                } else {
                    const oasp_mdb_source_cell *lhs = oasp_mdb_resolve_cell(&query, &left, &right, i, j, query.join_left, NULL);
                    const oasp_mdb_source_cell *rhs = oasp_mdb_resolve_cell(&query, &left, &right, i, j, query.join_right, NULL);
                    if (!lhs || !rhs || lhs->is_null || rhs->is_null || oasp_mdb_compare(lhs->text, rhs->text) != 0)
                        continue;
                    join_matched = true;
                }
            }
            if (!oasp_mdb_row_matches(&query, &left, has_right ? &right : NULL, i, j))
                continue;
            if (!oasp_mdb_match_subqueries(path, &query, &left, has_right ? &right : NULL,
                                           (oasp_mdb_match){i, j}))
                continue;
            oasp_mdb_match *next = realloc(matches, (match_count + 1) * sizeof(*matches));
            if (!next)
                goto done;
            matches = next;
            matches[match_count++] = (oasp_mdb_match){i, j};
        }
    }
    if (query.order_count) {
        for (size_t i = 1; i < match_count; i++) {
            oasp_mdb_match moving = matches[i];
            size_t j = i;
            while (j > 0) {
                int comparison = 0;
                for (size_t key = 0; key < query.order_count && !comparison; key++) {
                    const oasp_mdb_source_cell *before =
                        oasp_mdb_resolve_cell(&query, &left, has_right ? &right : NULL, matches[j - 1].left,
                                              matches[j - 1].right, query.orders[key].column, NULL);
                    const oasp_mdb_source_cell *current =
                        oasp_mdb_resolve_cell(&query, &left, has_right ? &right : NULL, moving.left, moving.right,
                                              query.orders[key].column, NULL);
                    comparison = before && current ? oasp_mdb_compare(before->text, current->text) : 0;
                    if (query.orders[key].descending)
                        comparison = -comparison;
                }
                if (comparison <= 0)
                    break;
                matches[j] = matches[j - 1];
                j--;
            }
            matches[j] = moving;
        }
    }
    if (query.group_count || (query.projection_count && oasp_mdb_aggregate(query.projections[0], NULL))) {
        for (size_t i = 0; i < match_count; i++) {
            bool found = false;
            for (size_t group = 0; group < group_count; group++) {
                if (oasp_mdb_matches_same_group(&query, &left, has_right ? &right : NULL,
                                                matches[groups[group]], matches[i])) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                size_t *next = realloc(groups, (group_count + 1) * sizeof(*groups));
                if (!next)
                    goto done;
                groups = next;
                groups[group_count++] = i;
            }
        }
        if (!query.group_count && !group_count) {
            groups = calloc(1, sizeof(*groups));
            if (!groups)
                goto done;
            group_count = 1;
        }
        if (query.group_count && match_count) {
            oasp_mdb_match *ordered = malloc(match_count * sizeof(*ordered));
            size_t position = 0;
            if (!ordered)
                goto done;
            for (size_t group = 0; group < group_count; group++) {
                oasp_mdb_match representative = matches[groups[group]];
                groups[group] = position;
                for (size_t i = 0; i < match_count; i++) {
                    if (oasp_mdb_matches_same_group(&query, &left, has_right ? &right : NULL,
                                                    representative, matches[i]))
                        ordered[position++] = matches[i];
                }
            }
            free(matches);
            matches = ordered;
        }
    } else {
        group_count = match_count;
        if (group_count) {
            groups = calloc(group_count, sizeof(*groups));
            if (!groups)
                goto done;
            for (size_t i = 0; i < group_count; i++)
                groups[i] = i;
        }
    }
    result->column_count = query.projection_count;
    result->row_count = query.top_limit && group_count > query.top_limit ? query.top_limit : group_count;
    result->columns = calloc(result->column_count, sizeof(*result->columns));
    result->cells = calloc(result->row_count * result->column_count, sizeof(*result->cells));
    if (!result->columns || (result->row_count && !result->cells))
        goto done;
    for (size_t i = 0; i < result->column_count; i++) {
        const char *name = oasp_mdb_column_suffix(query.projections[i]);
        result->columns[i] = oasp_mdb_copy(oasp_mdb_count_projection(name) ? "count" : name,
                                           oasp_mdb_count_projection(name) ? 5 : strlen(name));
        if (!result->columns[i])
            goto done;
    }
    for (size_t row = 0; row < result->row_count; row++) {
        size_t first = groups[row];
        size_t end = query.group_count && row + 1 < group_count ? groups[row + 1] : match_count;
        for (size_t column = 0; column < result->column_count; column++) {
            oasp_mdb_value *target = &result->cells[row * result->column_count + column];
            if (!oasp_mdb_projection_value(&query, &left, has_right ? &right : NULL, matches,
                                            first, end, query.projections[column], &target->text, &target->kind)) {
                result->error = oasp_mdb_copy("MDB SELECT column not found", 27);
                goto done;
            }
        }
        for (size_t condition = 0; condition < query.having_count; condition++) {
            const oasp_mdb_condition *having = &query.having[condition];
            bool matched = false;
            for (size_t column = 0; column < result->column_count; column++) {
                if (!strcasecmp(having->column, query.projections[column])
                    || (!strcasecmp(having->column, "count") && oasp_mdb_count_projection(query.projections[column]))) {
                    matched = oasp_mdb_condition_matches_value(having, result->cells[row * result->column_count + column].text);
                    break;
                }
            }
            if (!matched) {
                result->row_count = row;
                break;
            }
        }
    }
    if (query.distinct) {
        size_t kept = 0;
        for (size_t row = 0; row < result->row_count; row++) {
            bool duplicate = false;
            for (size_t previous = 0; previous < kept; previous++) {
                if (oasp_mdb_result_row_equal(result, previous, result, row)) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                for (size_t column = 0; column < result->column_count; column++)
                    free(result->cells[row * result->column_count + column].text);
                continue;
            }
            if (kept != row) {
                for (size_t column = 0; column < result->column_count; column++) {
                    oasp_mdb_value *source = &result->cells[row * result->column_count + column];
                    result->cells[kept * result->column_count + column] = *source;
                    source->text = NULL;
                }
            }
            kept++;
        }
        result->row_count = kept;
    }
    ok = true;
done:
    if (!ok && !result->error)
        result->error = oasp_mdb_copy("out of memory while querying MDB", 32);
    free(matches);
    free(groups);
    oasp_mdb_table_dispose(&left);
    oasp_mdb_table_dispose(&right);
    oasp_mdb_query_dispose(&query);
    if (!ok) {
        char *error = result->error;
        result->error = NULL;
        oasp_mdb_result_dispose(result);
        result->error = error;
    }
    return ok;
}

bool oasp_mdb_execute(const char *path, const char *sql, oasp_mdb_result *result) {
    const char *union_at;
    bool all = false;
    char *left_sql = NULL;
    char *right_sql = NULL;
    oasp_mdb_result left;
    oasp_mdb_result right;
    size_t row_count = 0;
    memset(result, 0, sizeof(*result));
    union_at = oasp_mdb_find_top_level_union(sql, &all);
    if (!union_at)
        return oasp_mdb_execute_select(path, sql, result);
    left_sql = oasp_mdb_trim_copy(sql, union_at);
    const char *right_start = union_at + 5;
    while (isspace((unsigned char)*right_start))
        right_start++;
    if (all) {
        right_start += 3;
    }
    right_sql = oasp_mdb_trim_copy(right_start, sql + strlen(sql));
    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    if (!left_sql || !right_sql || !oasp_mdb_execute(path, left_sql, &left)
        || !oasp_mdb_execute(path, right_sql, &right)) {
        result->error = oasp_mdb_copy("invalid MDB UNION query", 23);
        goto done;
    }
    if (left.column_count != right.column_count) {
        result->error = oasp_mdb_copy("MDB UNION requires matching column counts", 40);
        goto done;
    }
    result->column_count = left.column_count;
    result->columns = calloc(result->column_count, sizeof(*result->columns));
    if (!result->columns)
        goto done;
    for (size_t column = 0; column < result->column_count; column++) {
        result->columns[column] = oasp_mdb_copy(left.columns[column], strlen(left.columns[column]));
        if (!result->columns[column])
            goto done;
    }
    row_count = left.row_count + right.row_count;
    result->cells = calloc(row_count * result->column_count, sizeof(*result->cells));
    if (row_count && !result->cells)
        goto done;
    for (size_t source = 0; source < left.row_count + right.row_count; source++) {
        const oasp_mdb_result *input = source < left.row_count ? &left : &right;
        size_t input_row = source < left.row_count ? source : source - left.row_count;
        bool duplicate = false;
        if (!all) {
            for (size_t previous = 0; previous < result->row_count; previous++) {
                if (oasp_mdb_result_row_equal(result, previous, input, input_row)) {
                    duplicate = true;
                    break;
                }
            }
        }
        if (!duplicate) {
            for (size_t column = 0; column < result->column_count; column++) {
                oasp_mdb_value *target = &result->cells[result->row_count * result->column_count + column];
                const oasp_mdb_value *source_value = &input->cells[input_row * input->column_count + column];
                target->text = oasp_mdb_copy(source_value->text, strlen(source_value->text));
                target->kind = source_value->kind;
                if (!target->text)
                    goto done;
            }
            result->row_count++;
        }
    }
    free(left_sql);
    free(right_sql);
    oasp_mdb_result_dispose(&left);
    oasp_mdb_result_dispose(&right);
    return true;
done:
    free(left_sql);
    free(right_sql);
    oasp_mdb_result_dispose(&left);
    oasp_mdb_result_dispose(&right);
    if (!result->error)
        result->error = oasp_mdb_copy("out of memory while merging MDB UNION", 37);
    {
        char *error = result->error;
        result->error = NULL;
        oasp_mdb_result_dispose(result);
        result->error = error;
    }
    return false;
}
