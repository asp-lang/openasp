// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/* SQLite ADO backend; included after shared Recordset helpers. */

static oasp_value oasp_ado_sqlite_cell(oasp_runtime *runtime, sqlite3_stmt *statement, int column) {
    int type = sqlite3_column_type(statement, column);
    if (type == SQLITE_NULL)
        return oasp_value_null();
    if (type == SQLITE_INTEGER)
        return oasp_value_int(sqlite3_column_int64(statement, column));
    if (type == SQLITE_FLOAT)
        return oasp_value_float(sqlite3_column_double(statement, column));
    const unsigned char *text = sqlite3_column_text(statement, column);
    int length = sqlite3_column_bytes(statement, column);
    return oasp_ado_slice_value(runtime, text ? (const char *)text : "", length > 0 ? (size_t)length : 0);
}

static void oasp_ado_sqlite_close_source(oasp_ado_recordset *recordset) {
    sqlite3_stmt *statement = recordset ? recordset->row_source : NULL;
    if (statement)
        sqlite3_finalize(statement);
}

static bool oasp_ado_sqlite_fetch_next(oasp_runtime *runtime, oasp_ado_recordset *recordset, size_t line) {
    sqlite3_stmt *statement = recordset ? recordset->row_source : NULL;
    if (!statement)
        return false;
    int status = sqlite3_step(statement);
    if (status == SQLITE_DONE)
        return false;
    if (status != SQLITE_ROW) {
        sqlite3 *database = sqlite3_db_handle(statement);
        const char *message = sqlite3_errmsg(database);
        oasp_runtime_raise(runtime, 3000, oasp_runtime_copy(runtime, message, strlen(message)), "ADODB.Connection", line);
        return false;
    }
    oasp_arena *row_arena = NULL;
    oasp_value *row = oasp_ado_recordset_append_row(recordset, &row_arena);
    if (!row) {
        oasp_runtime_raise(runtime, 7, "out of memory while fetching SQLite row", "ADODB.Recordset", line);
        return false;
    }
    oasp_runtime row_runtime = *runtime;
    row_runtime.arena = row_arena;
    for (size_t i = 0; i < recordset->column_count; i++)
        row[i] = oasp_ado_sqlite_cell(&row_runtime, statement, (int)i);
    return true;
}

static bool oasp_ado_sqlite_has_more_sql(const char *cursor) {
    while (*cursor == ';' || isspace((unsigned char)*cursor))
        cursor++;
    return *cursor != '\0';
}

static oasp_value oasp_ado_execute_sqlite(oasp_runtime *runtime, oasp_object *owner, oasp_ado_connection *connection, const char *sql, size_t line) {
    if (!connection->opened || !connection->sqlite) {
        oasp_runtime_raise(runtime, 3709, "SQLite connection is not open", "ADODB.Connection", line);
        return oasp_value_empty();
    }
    oasp_ado_recordset *recordset = oasp_ado_recordset_new(runtime);
    if (!recordset)
        return oasp_value_empty();
    recordset->connection = owner;
    recordset->source = oasp_ado_string(runtime, sql);
    const char *cursor = sql;
    while (*cursor) {
        sqlite3_stmt *statement = NULL;
        const char *tail = NULL;
        int status = sqlite3_prepare_v2(connection->sqlite, cursor, -1, &statement, &tail);
        if (status != SQLITE_OK) {
            oasp_runtime_raise(runtime, 3000, oasp_runtime_copy(runtime, sqlite3_errmsg(connection->sqlite), strlen(sqlite3_errmsg(connection->sqlite))),
                               "ADODB.Connection", line);
            if (statement)
                sqlite3_finalize(statement);
            return oasp_value_empty();
        }
        cursor = tail ? tail : cursor + strlen(cursor);
        if (!statement)
            continue;
        size_t column_count = (size_t)sqlite3_column_count(statement);
        oasp_ado_column *columns =
            column_count ? oasp_arena_alloc(runtime->arena, column_count * sizeof(*columns),
                                            _Alignof(oasp_ado_column)) : NULL;
        if (column_count && !columns) {
            sqlite3_finalize(statement);
            return oasp_value_empty();
        }
        for (size_t i = 0; i < column_count; i++) {
            oasp_slice name = oasp_ado_string(runtime, sqlite3_column_name(statement, (int)i));
            columns[i].data = name.data;
            columns[i].length = name.length <= UINT32_MAX ? (uint32_t)name.length : UINT32_MAX;
            columns[i].index = i <= UINT32_MAX ? (uint32_t)i : UINT32_MAX;
        }
        (void)oasp_ado_recordset_begin(recordset, columns, column_count);
        if (column_count && !oasp_ado_sqlite_has_more_sql(cursor)) {
            recordset->row_source = statement;
            recordset->row_source_fetch = oasp_ado_sqlite_fetch_next;
            recordset->row_source_close = oasp_ado_sqlite_close_source;
            recordset->source_exhausted = false;
            recordset->sliding_window = true;
            oasp_ado_recordset_attach_active(recordset);
            (void)oasp_ado_recordset_fetch_next(runtime, recordset, line);
            if (runtime->error_pending)
                return oasp_value_empty();
            break;
        }
        while ((status = sqlite3_step(statement)) == SQLITE_ROW) {
            oasp_arena *row_arena = NULL;
            oasp_value *row = oasp_ado_recordset_append_row(recordset, &row_arena);
            if (!row) {
                sqlite3_finalize(statement);
                return oasp_value_empty();
            }
            oasp_runtime row_runtime = *runtime;
            row_runtime.arena = row_arena;
            for (size_t i = 0; i < column_count; i++)
                row[i] = oasp_ado_sqlite_cell(&row_runtime, statement, (int)i);
        }
        if (status != SQLITE_DONE) {
            const char *message = sqlite3_errmsg(connection->sqlite);
            oasp_runtime_raise(runtime, 3000, oasp_runtime_copy(runtime, message, strlen(message)), "ADODB.Connection", line);
            sqlite3_finalize(statement);
            return oasp_value_empty();
        }
        sqlite3_finalize(statement);
    }
    recordset->opened = true;
    return oasp_ado_recordset_value(runtime, recordset);
}
