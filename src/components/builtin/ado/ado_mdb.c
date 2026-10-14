// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/* MDB ADO backend; included after shared Recordset helpers. */

static oasp_value oasp_ado_execute_mdb(oasp_runtime *runtime, oasp_object *owner, oasp_ado_connection *connection, const char *sql, size_t line) {
    oasp_mdb_result source;
    const char *statement = sql;
    oasp_mdb_stdout_silence silence;
    bool command;
    bool executed;
    memset(&source, 0, sizeof(source));
    while (isspace((unsigned char)*statement))
        statement++;
    char *path = oasp_runtime_copy(runtime, connection->database_path.data, connection->database_path.length);
    command = !strncasecmp(statement, "update ", 7) || !strncasecmp(statement, "insert ", 7) ||
              !strncasecmp(statement, "delete ", 7) || !strncasecmp(statement, "create ", 7) ||
              !strncasecmp(statement, "alter ", 6) || !strncasecmp(statement, "drop ", 5);
    if (command)
        (void)oasp_mdb_stdout_silence_begin(&silence);
    executed = path && (command ? oasp_mdb_execute_command(path, statement, &source.error)
                                : oasp_mdb_execute(path, statement, &source));
    if (command)
        oasp_mdb_stdout_silence_end(&silence);
    if (!executed) {
        const char *message = source.error ? source.error : "MDB query failed";
        char *copy = oasp_runtime_copy(runtime, message, strlen(message));
        oasp_mdb_result_dispose(&source);
        oasp_runtime_raise(runtime, 3000, copy ? copy : "MDB query failed", "ADODB.Connection", line);
        return oasp_value_empty();
    }
    oasp_ado_recordset *recordset = oasp_ado_recordset_new(runtime);
    if (!recordset) {
        oasp_mdb_result_dispose(&source);
        return oasp_value_empty();
    }
    recordset->connection = owner;
    recordset->source = oasp_ado_string(runtime, sql);
    recordset->columns =
        source.column_count ? oasp_arena_alloc(runtime->arena,
                                               source.column_count * sizeof(*recordset->columns),
                                               _Alignof(oasp_ado_column)) : NULL;
    if (source.column_count && !recordset->columns) {
        oasp_mdb_result_dispose(&source);
        return oasp_value_empty();
    }
    for (size_t i = 0; i < source.column_count; i++) {
        oasp_slice name = oasp_ado_string(runtime, source.columns[i]);
        recordset->columns[i].data = name.data;
        recordset->columns[i].length =
            name.length <= UINT32_MAX ? (uint32_t)name.length : UINT32_MAX;
        recordset->columns[i].index = i <= UINT32_MAX ? (uint32_t)i : UINT32_MAX;
    }
    (void)oasp_ado_recordset_begin(recordset, recordset->columns, source.column_count);
    recordset->dirty_columns = source.column_count
                                  ? oasp_runtime_alloc(runtime, source.column_count * sizeof(*recordset->dirty_columns),
                                                       _Alignof(bool))
                                  : NULL;
    if (source.column_count && !recordset->dirty_columns) {
        oasp_mdb_result_dispose(&source);
        return oasp_value_empty();
    }
    if (recordset->dirty_columns)
        memset(recordset->dirty_columns, 0, source.column_count * sizeof(*recordset->dirty_columns));
    for (size_t row_index = 0; row_index < source.row_count; row_index++) {
        oasp_value *row = oasp_ado_recordset_append_row(recordset, NULL);
        if (!row) {
            oasp_mdb_result_dispose(&source);
            return oasp_value_empty();
        }
        for (size_t column = 0; column < source.column_count; column++) {
            oasp_mdb_value *cell = &source.cells[row_index * source.column_count + column];
            switch (cell->kind) {
            case OASP_MDB_NULL:
                row[column] = oasp_value_null();
                break;
            case OASP_MDB_BOOL:
                row[column] = oasp_value_bool(!strcasecmp(cell->text, "true") || !strcmp(cell->text, "1") || !strcmp(cell->text, "-1") ||
                                              !strcasecmp(cell->text, "yes"));
                break;
            case OASP_MDB_INTEGER:
                row[column] = oasp_value_int(strtoll(cell->text, NULL, 10));
                break;
            case OASP_MDB_FLOAT:
                row[column] = oasp_value_float(strtod(cell->text, NULL));
                break;
            case OASP_MDB_STRING:
                row[column] = oasp_ado_slice_value(runtime, cell->text, strlen(cell->text));
                break;
            }
        }
    }
    oasp_mdb_result_dispose(&source);
    return oasp_ado_recordset_value(runtime, recordset);
}

static bool oasp_ado_recordset_delete_mdb(oasp_runtime *runtime, oasp_object *object, oasp_ado_recordset *recordset, size_t line) {
    oasp_ado_connection *connection = recordset->connection ? recordset->connection->native : NULL;
    char *path;
    char *source;
    char *error = NULL;
    bool ok;
    oasp_mdb_stdout_silence silence;
    if (!connection || connection->kind != OASP_DATABASE_MDB)
        return true;
    if (!oasp_ado_recordset_row(recordset, recordset->cursor)) {
        oasp_runtime_raise(runtime, 3021, "no current record", object->component_name, line);
        return false;
    }
    path = oasp_runtime_copy(runtime, connection->database_path.data, connection->database_path.length);
    source = oasp_runtime_copy(runtime, recordset->source.data, recordset->source.length);
    (void)oasp_mdb_stdout_silence_begin(&silence);
    ok = path && source && oasp_mdb_delete_record(path, source, &error);
    oasp_mdb_stdout_silence_end(&silence);
    if (!ok) {
        const char *message = error ? error : "MDB Recordset.Delete failed";
        oasp_runtime_raise(runtime, 3000, oasp_runtime_copy(runtime, message, strlen(message)), object->component_name, line);
    }
    free(error);
    return ok;
}

static bool oasp_ado_recordset_update_mdb(oasp_runtime *runtime, oasp_object *object, oasp_ado_recordset *recordset, size_t line) {
    oasp_ado_connection *connection = recordset->connection ? recordset->connection->native : NULL;
    oasp_value *row = oasp_ado_recordset_row(recordset, recordset->cursor);
    size_t count = 0;
    char **columns = NULL;
    char **values = NULL;
    char *path = NULL;
    char *source = NULL;
    char *error = NULL;
    bool ok = false;
    oasp_mdb_stdout_silence silence;
    if (!connection || connection->kind != OASP_DATABASE_MDB)
        return true;
    if (!row || !recordset->dirty_columns) {
        oasp_runtime_raise(runtime, 3021, "no current record", object->component_name, line);
        return false;
    }
    for (size_t i = 0; i < recordset->column_count; i++)
        count += recordset->dirty_columns[i] ? 1 : 0;
    if (!count)
        return true;
    columns = calloc(count, sizeof(*columns));
    values = calloc(count, sizeof(*values));
    path = oasp_runtime_copy(runtime, connection->database_path.data, connection->database_path.length);
    source = oasp_runtime_copy(runtime, recordset->source.data, recordset->source.length);
    if (!columns || !values || !path || !source)
        goto done;
    for (size_t index = 0, output = 0; index < recordset->column_count; index++) {
        const oasp_ado_column *column;
        if (!recordset->dirty_columns[index])
            continue;
        column = oasp_ado_column_by_index(recordset, index);
        columns[output] = column ? oasp_runtime_copy(runtime, column->data, column->length) : NULL;
        values[output] = oasp_value_format(runtime->arena, &row[index]);
        if (!columns[output] || !values[output])
            goto done;
        output++;
    }
    (void)oasp_mdb_stdout_silence_begin(&silence);
    ok = recordset->adding_new
             ? oasp_mdb_insert_record(path, source, (const char *const *)columns, (const char *const *)values, count, &error)
             : oasp_mdb_update_record(path, source, (const char *const *)columns, (const char *const *)values, count, &error);
    oasp_mdb_stdout_silence_end(&silence);
    if (!ok) {
        const char *message = error ? error : "MDB Recordset.Update failed";
        oasp_runtime_raise(runtime, 3000, oasp_runtime_copy(runtime, message, strlen(message)), object->component_name, line);
        goto done;
    }
    memset(recordset->dirty_columns, 0, recordset->column_count * sizeof(*recordset->dirty_columns));
    recordset->adding_new = false;
done:
    free(columns);
    free(values);
    free(error);
    if (!ok && !runtime->error_pending)
        oasp_runtime_raise(runtime, 7, "out of memory", object->component_name, line);
    return ok;
}
