// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/* MySQL ADO backend; included after shared Recordset helpers. */

static oasp_value oasp_ado_mysql_cell(oasp_runtime *runtime, const MYSQL_FIELD *field, const char *data, unsigned long length) {
    if (!data)
        return oasp_value_null();
    switch (field->type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_YEAR: {
        (void)length;
        return oasp_value_int(strtoll(data, NULL, 10));
    }
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE: {
        (void)length;
        return oasp_value_float(strtod(data, NULL));
    }
    default:
        return oasp_ado_slice_value(runtime, data, length);
    }
}

static bool oasp_ado_mysql_fetch_next(oasp_runtime *runtime, oasp_ado_recordset *recordset, size_t line) {
    if (!recordset || !recordset->mysql_result || recordset->source_exhausted)
        return false;
    MYSQL_ROW source_row = mysql_fetch_row(recordset->mysql_result);
    if (!source_row) {
        oasp_ado_connection *connection = recordset->connection ? recordset->connection->native : NULL;
        if (connection && mysql_errno(connection->mysql)) {
            const char *message = mysql_error(connection->mysql);
            oasp_runtime_raise(runtime, 3000, oasp_runtime_copy(runtime, message, strlen(message)), "ADODB.Connection", line);
        }
        return false;
    }
    unsigned long *lengths = mysql_fetch_lengths(recordset->mysql_result);
    MYSQL_FIELD *fields = mysql_fetch_fields(recordset->mysql_result);
    oasp_arena *row_arena = NULL;
    oasp_value *row = lengths && fields ? oasp_ado_recordset_append_row(recordset, &row_arena) : NULL;
    if (!row) {
        oasp_runtime_raise(runtime, 7, "out of memory while fetching MySQL row", "ADODB.Recordset", line);
        return false;
    }
    oasp_runtime row_runtime = *runtime;
    row_runtime.arena = row_arena;
    for (size_t i = 0; i < recordset->column_count; i++)
        row[i] = oasp_ado_mysql_cell(&row_runtime, &fields[i], source_row[i], source_row[i] ? lengths[i] : 0);
    return true;
}

static int oasp_ado_days_in_month(int year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12)
        return 0;
    if (month != 2)
        return days[month - 1];
    return 28 + (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

static bool oasp_ado_normalize_date(const char *text, size_t length, char output[24], size_t *output_length) {
    char local[64];
    if (!length || length >= sizeof(local))
        return false;
    memcpy(local, text, length);
    local[length] = '\0';
    int month = 0;
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int consumed = 0;
    int fields = sscanf(local, "%d/%d/%d %d:%d:%d%n", &month, &day, &year, &hour, &minute, &second, &consumed);
    bool with_time = fields == 6 && consumed == (int)length;
    if (!with_time) {
        consumed = 0;
        fields = sscanf(local, "%d/%d/%d%n", &month, &day, &year, &consumed);
        if (fields != 3 || consumed != (int)length) {
            consumed = 0;
            fields = sscanf(local, "%d-%d-%d %d:%d:%d%n", &year, &month, &day, &hour, &minute, &second, &consumed);
            with_time = fields == 6 && consumed == (int)length;
            if (!with_time) {
                consumed = 0;
                fields = sscanf(local, "%d-%d-%d%n", &year, &month, &day, &consumed);
                if (fields != 3 || consumed != (int)length)
                    return false;
            }
        }
    }
    if (year < 1000 || year > 9999 || day < 1 || day > oasp_ado_days_in_month(year, month) || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59)
        return false;
    int written = with_time ? snprintf(output, 24, "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second)
                            : snprintf(output, 24, "%04d-%02d-%02d", year, month, day);
    if (written < 0 || written >= 24)
        return false;
    *output_length = (size_t)written;
    return true;
}

static char *oasp_ado_mysql_rewrite_dates(oasp_runtime *runtime, const char *sql) {
    oasp_buffer output;
    if (!oasp_buffer_init(&output, strlen(sql) + 32))
        return NULL;
    const char *cursor = sql;
    while (*cursor) {
        bool count_boundary = cursor == sql || (!isalnum((unsigned char)cursor[-1]) && cursor[-1] != '_');
        if (count_boundary && strlen(cursor) >= 5 && oasp_ascii_equal_n(cursor, 5, "count")) {
            const char *parenthesis = cursor + 5;
            while (*parenthesis && isspace((unsigned char)*parenthesis))
                parenthesis++;
            if (*parenthesis == '(') {
                (void)oasp_buffer_append(&output, cursor, 5);
                (void)oasp_buffer_append_byte(&output, '(');
                cursor = parenthesis + 1;
                continue;
            }
        }
        if (*cursor == '[') {
            const char *close = strchr(cursor + 1, ']');
            if (close) {
                (void)oasp_buffer_append_byte(&output, '`');
                (void)oasp_buffer_append(&output, cursor + 1, (size_t)(close - cursor - 1));
                (void)oasp_buffer_append_byte(&output, '`');
                cursor = close + 1;
                continue;
            }
        }
        if (*cursor != '\'' && *cursor != '#') {
            (void)oasp_buffer_append_byte(&output, *cursor++);
            continue;
        }
        char delimiter = *cursor;
        const char *literal = ++cursor;
        while (*cursor) {
            if (*cursor == delimiter) {
                if (delimiter == '\'' && cursor[1] == '\'') {
                    cursor += 2;
                    continue;
                }
                break;
            }
            cursor++;
        }
        size_t literal_length = (size_t)(cursor - literal);
        char normalized[24];
        size_t normalized_length = 0;
        if (oasp_ado_normalize_date(literal, literal_length, normalized, &normalized_length)) {
            (void)oasp_buffer_append_byte(&output, '\'');
            (void)oasp_buffer_append(&output, normalized, normalized_length);
            (void)oasp_buffer_append_byte(&output, '\'');
        } else {
            (void)oasp_buffer_append_byte(&output, delimiter);
            (void)oasp_buffer_append(&output, literal, literal_length);
            if (*cursor)
                (void)oasp_buffer_append_byte(&output, delimiter);
        }
        if (*cursor)
            cursor++;
    }
    char *rewritten = oasp_runtime_copy(runtime, output.data, output.length);
    oasp_buffer_dispose(&output);
    return rewritten;
}

static char *oasp_ado_mysql_rewrite_instr(oasp_runtime *runtime, const char *sql) {
    oasp_buffer output;
    if (!oasp_buffer_init(&output, strlen(sql) + 32))
        return NULL;
    const char *cursor = sql;
    while (*cursor) {
        size_t remaining = strlen(cursor);
        bool boundary = cursor == sql || (!isalnum((unsigned char)cursor[-1]) && cursor[-1] != '_');
        if (remaining < 6 || !boundary || !oasp_ascii_equal_n(cursor, 6, "instr(")) {
            (void)oasp_buffer_append_byte(&output, *cursor++);
            continue;
        }

        const char *arguments[4] = {cursor + 6, NULL, NULL, NULL};
        size_t lengths[4] = {0};
        size_t argument = 0;
        size_t depth = 0;
        bool quoted = false;
        const char *scan = cursor + 6;
        for (; *scan; scan++) {
            if (*scan == '\'') {
                if (quoted && scan[1] == '\'') {
                    scan++;
                    continue;
                }
                quoted = !quoted;
                continue;
            }
            if (quoted)
                continue;
            if (*scan == '(') {
                depth++;
                continue;
            }
            if (*scan == ')' && depth) {
                depth--;
                continue;
            }
            if ((*scan == ',' || *scan == ')') && depth == 0) {
                if (argument >= 4)
                    break;
                const char *begin = arguments[argument];
                const char *end = scan;
                while (begin < end && isspace((unsigned char)*begin))
                    begin++;
                while (end > begin && isspace((unsigned char)end[-1]))
                    end--;
                arguments[argument] = begin;
                lengths[argument] = (size_t)(end - begin);
                argument++;
                if (*scan == ')')
                    break;
                if (argument < 4)
                    arguments[argument] = scan + 1;
            }
        }
        if (*scan != ')' || argument != 4) {
            (void)oasp_buffer_append_byte(&output, *cursor++);
            continue;
        }
        (void)oasp_buffer_append(&output, "LOCATE(", 7);
        (void)oasp_buffer_append(&output, arguments[2], lengths[2]);
        (void)oasp_buffer_append_byte(&output, ',');
        (void)oasp_buffer_append(&output, arguments[1], lengths[1]);
        (void)oasp_buffer_append_byte(&output, ',');
        (void)oasp_buffer_append(&output, arguments[0], lengths[0]);
        (void)oasp_buffer_append_byte(&output, ')');
        cursor = scan + 1;
    }
    char *rewritten = oasp_runtime_copy(runtime, output.data, output.length);
    oasp_buffer_dispose(&output);
    return rewritten;
}

static char *oasp_ado_mysql_rewrite_top(oasp_runtime *runtime, const char *sql) {
    enum { OASP_SQL_MAX_DEPTH = 128 };
    const char *limits[OASP_SQL_MAX_DEPTH] = {0};
    size_t limit_lengths[OASP_SQL_MAX_DEPTH] = {0};
    size_t depth = 0;
    bool changed = false;
    oasp_buffer output;
    if (!oasp_buffer_init(&output, strlen(sql) + 32))
        return NULL;
    const char *cursor = sql;
    while (*cursor) {
        if (*cursor == '\'' || *cursor == '`') {
            char delimiter = *cursor;
            if (!oasp_buffer_append_byte(&output, *cursor++))
                goto out_of_memory;
            while (*cursor) {
                char character = *cursor;
                if (!oasp_buffer_append_byte(&output, *cursor++))
                    goto out_of_memory;
                if (character != delimiter)
                    continue;
                if (*cursor == delimiter) {
                    if (!oasp_buffer_append_byte(&output, *cursor++))
                        goto out_of_memory;
                    continue;
                }
                break;
            }
            continue;
        }
        if (*cursor == '(') {
            if (depth + 1 >= OASP_SQL_MAX_DEPTH)
                goto out_of_memory;
            depth++;
            if (!oasp_buffer_append_byte(&output, *cursor++))
                goto out_of_memory;
            continue;
        }
        if (*cursor == ')') {
            if (limits[depth]) {
                if (!oasp_buffer_append(&output, " LIMIT ", 7) || !oasp_buffer_append(&output, limits[depth], limit_lengths[depth]))
                    goto out_of_memory;
                limits[depth] = NULL;
            }
            if (depth)
                depth--;
            if (!oasp_buffer_append_byte(&output, *cursor++))
                goto out_of_memory;
            continue;
        }

        bool boundary = cursor == sql || (!isalnum((unsigned char)cursor[-1]) && cursor[-1] != '_');
        size_t remaining = strlen(cursor);
        if (boundary && remaining >= 6 && oasp_ascii_equal_n(cursor, 6, "select") && !isalnum((unsigned char)cursor[6]) && cursor[6] != '_') {
            const char *scan = cursor + 6;
            while (isspace((unsigned char)*scan))
                scan++;
            const char *distinct = NULL;
            if (strlen(scan) >= 8 && oasp_ascii_equal_n(scan, 8, "distinct") && !isalnum((unsigned char)scan[8]) && scan[8] != '_') {
                distinct = scan;
                scan += 8;
                while (isspace((unsigned char)*scan))
                    scan++;
            }
            if (strlen(scan) >= 3 && oasp_ascii_equal_n(scan, 3, "top") && !isalnum((unsigned char)scan[3]) && scan[3] != '_') {
                const char *number = scan + 3;
                while (isspace((unsigned char)*number))
                    number++;
                const char *number_end = number;
                while (*number_end >= '0' && *number_end <= '9')
                    number_end++;
                if (number_end > number && !limits[depth]) {
                    const char *rest = number_end;
                    while (isspace((unsigned char)*rest))
                        rest++;
                    if (!oasp_buffer_append(&output, cursor, 6) || !oasp_buffer_append_byte(&output, ' ') ||
                        (distinct && (!oasp_buffer_append(&output, distinct, 8) || !oasp_buffer_append_byte(&output, ' '))))
                        goto out_of_memory;
                    limits[depth] = number;
                    limit_lengths[depth] = (size_t)(number_end - number);
                    cursor = rest;
                    changed = true;
                    continue;
                }
            }
        }
        if (!oasp_buffer_append_byte(&output, *cursor++))
            goto out_of_memory;
    }
    if (limits[0]) {
        if (!oasp_buffer_append(&output, " LIMIT ", 7) || !oasp_buffer_append(&output, limits[0], limit_lengths[0]))
            goto out_of_memory;
    }
    if (!changed) {
        oasp_buffer_dispose(&output);
        return (char *)sql;
    }
    char *rewritten = oasp_runtime_copy(runtime, output.data, output.length);
    oasp_buffer_dispose(&output);
    return rewritten;
out_of_memory:
    oasp_buffer_dispose(&output);
    return NULL;
}

static oasp_value oasp_ado_execute_mysql(oasp_runtime *runtime, oasp_object *owner, oasp_ado_connection *connection, const char *sql, size_t line) {
    if (!connection->opened || !connection->mysql) {
        oasp_runtime_raise(runtime, 3709, "MySQL connection is not open", "ADODB.Connection", line);
        return oasp_value_empty();
    }
    char *dated_sql = oasp_ado_mysql_rewrite_dates(runtime, sql);
    char *instr_sql = dated_sql ? oasp_ado_mysql_rewrite_instr(runtime, dated_sql) : NULL;
    char *mysql_sql = instr_sql ? oasp_ado_mysql_rewrite_top(runtime, instr_sql) : NULL;
    if (!mysql_sql)
        return oasp_value_empty();
    while (connection->active_recordsets)
        oasp_ado_recordset_close_source(connection->active_recordsets);
    if (mysql_real_query(connection->mysql, mysql_sql, (unsigned long)strlen(mysql_sql))) {
        const char *message = mysql_error(connection->mysql);
        oasp_runtime_raise(runtime, 3000, oasp_runtime_copy(runtime, message, strlen(message)), "ADODB.Connection", line);
        return oasp_value_empty();
    }
    oasp_ado_recordset *recordset = oasp_ado_recordset_new(runtime);
    if (!recordset)
        return oasp_value_empty();
    recordset->connection = owner;
    recordset->source = oasp_ado_string(runtime, mysql_sql);
    bool buffered = connection->mysql_store_result;
    MYSQL_RES *query_result = buffered ? mysql_store_result(connection->mysql) : mysql_use_result(connection->mysql);
    if (query_result) {
        size_t column_count = mysql_num_fields(query_result);
        MYSQL_FIELD *fields = mysql_fetch_fields(query_result);
        oasp_ado_column *columns =
            column_count ? oasp_arena_alloc(runtime->arena, column_count * sizeof(*columns),
                                            _Alignof(oasp_ado_column)) : NULL;
        if (column_count && !columns) {
            mysql_free_result(query_result);
            return oasp_value_empty();
        }
        for (size_t i = 0; i < column_count; i++) {
            oasp_slice name = oasp_ado_string(runtime, fields[i].name);
            columns[i].data = name.data;
            columns[i].length = name.length <= UINT32_MAX ? (uint32_t)name.length : UINT32_MAX;
            columns[i].index = i <= UINT32_MAX ? (uint32_t)i : UINT32_MAX;
        }
        (void)oasp_ado_recordset_begin(recordset, columns, column_count);
        recordset->mysql_result = query_result;
        recordset->row_source = query_result;
        recordset->row_source_fetch = oasp_ado_mysql_fetch_next;
        recordset->row_source_close = oasp_ado_recordset_close_mysql_source;
        recordset->source_exhausted = false;
        recordset->sliding_window = true;
        if (buffered) {
            while (oasp_ado_mysql_fetch_next(runtime, recordset, line))
                ;
            oasp_ado_recordset_close_source(recordset);
        } else {
            oasp_ado_recordset_attach_active(recordset);
            (void)oasp_ado_recordset_fetch_next(runtime, recordset, line);
        }
        if (runtime->error_pending)
            return oasp_value_empty();
    } else if (mysql_field_count(connection->mysql)) {
        const char *message = mysql_error(connection->mysql);
        oasp_runtime_raise(runtime, 3000, oasp_runtime_copy(runtime, message, strlen(message)), "ADODB.Connection", line);
        return oasp_value_empty();
    } else {
        recordset->source_exhausted = true;
        oasp_ado_recordset_close_source(recordset);
    }
    recordset->opened = true;
    return oasp_ado_recordset_value(runtime, recordset);
}
