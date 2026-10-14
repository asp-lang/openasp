// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "builtin_internal.h"
#include "component_internal.h"
#include "component_object.h"
#include "mdb.h"
#include "symbol.h"

#include <ctype.h>
#include <string.h>
#include <unistd.h>

static oasp_value oasp_ado_slice_value(oasp_runtime *runtime, const char *data, size_t length) {
    char *copy = oasp_runtime_copy(runtime, data, length);
    return copy ? oasp_value_string(copy, length) : oasp_value_empty();
}

static void oasp_ado_recordset_close_mysql_source(oasp_ado_recordset *recordset) {
    if (!recordset)
        return;
    oasp_ado_connection *connection = recordset->connection ? recordset->connection->native : NULL;
    if (recordset->mysql_result) {
        while (mysql_fetch_row(recordset->mysql_result))
            ;
        mysql_free_result(recordset->mysql_result);
        recordset->mysql_result = NULL;
    }
    if (connection && connection->mysql) {
        while (mysql_next_result(connection->mysql) == 0) {
            MYSQL_RES *result = mysql_use_result(connection->mysql);
            if (result) {
                while (mysql_fetch_row(result))
                    ;
                mysql_free_result(result);
            }
        }
    }
}

static void oasp_ado_recordset_detach_active(oasp_ado_recordset *recordset) {
    oasp_ado_connection *connection = recordset && recordset->connection ? recordset->connection->native : NULL;
    if (!connection)
        return;
    oasp_ado_recordset **link = &connection->active_recordsets;
    while (*link && *link != recordset)
        link = &(*link)->next_active;
    if (*link == recordset)
        *link = recordset->next_active;
    recordset->next_active = NULL;
}

static void oasp_ado_recordset_attach_active(oasp_ado_recordset *recordset) {
    oasp_ado_connection *connection = recordset && recordset->connection ? recordset->connection->native : NULL;
    if (!connection)
        return;
    oasp_ado_recordset_detach_active(recordset);
    recordset->next_active = connection->active_recordsets;
    connection->active_recordsets = recordset;
}

static void oasp_ado_recordset_close_source(oasp_ado_recordset *recordset) {
    if (!recordset)
        return;
    if (recordset->row_source_close)
        recordset->row_source_close(recordset);
    recordset->row_source = NULL;
    recordset->row_source_fetch = NULL;
    recordset->row_source_close = NULL;
    recordset->source_exhausted = true;
    oasp_ado_recordset_detach_active(recordset);
}

static bool oasp_ado_recordset_fetch_next(oasp_runtime *runtime, oasp_ado_recordset *recordset, size_t line) {
    if (!recordset || recordset->source_exhausted || !recordset->row_source_fetch)
        return false;
    if (recordset->row_source_fetch(runtime, recordset, line))
        return true;
    oasp_ado_recordset_close_source(recordset);
    return false;
}

bool oasp_ado_recordset_materialize(oasp_runtime *runtime, oasp_ado_recordset *recordset, size_t limit, size_t line) {
    while (!recordset->source_exhausted && recordset->row_count < limit)
        if (!oasp_ado_recordset_fetch_next(runtime, recordset, line) && runtime->error_pending)
            return false;
    return !runtime->error_pending;
}

static void oasp_ado_recordset_finalize(void *context) {
    oasp_ado_recordset *recordset = context;
    oasp_ado_recordset_close_source(recordset);
    while (recordset->segments) {
        oasp_ado_row_segment *segment = recordset->segments;
        oasp_arena *arena = segment->arena;
        recordset->segments = segment->next;
        oasp_arena_destroy(arena);
    }
    recordset->segment_tail = NULL;
    recordset->current_segment = NULL;
}

static bool oasp_ado_mdb_transaction_finish(oasp_ado_connection *connection, bool rollback) {
    char error[256] = "";
    if (!connection || !connection->mdb_transaction_active)
        return false;
    bool result = oasp_mdb_transaction_scope_finish(&connection->mdb_transaction_lock, rollback,
                                                    error, sizeof(error)) != 0;
    connection->mdb_transaction_active = false;
    return result;
}

static bool oasp_ado_mdb_transaction_begin(oasp_ado_connection *connection) {
    char *path;
    char error[256] = "";
    if (!connection || connection->kind != OASP_DATABASE_MDB || connection->mdb_transaction_active)
        return false;
    path = strndup(connection->database_path.data, connection->database_path.length);
    if (!path)
        return false;
    if (!oasp_mdb_transaction_scope_begin(path, &connection->mdb_transaction_lock, error, sizeof(error))) {
        free(path);
        return false;
    }
    free(path);
    connection->mdb_transaction_active = true;
    return true;
}

static void oasp_ado_connection_finalize(void *context) {
    oasp_ado_connection *connection = context;
    oasp_ado_mdb_transaction_finish(connection, true);
    while (connection->active_recordsets) {
        oasp_ado_recordset *recordset = connection->active_recordsets;
        connection->active_recordsets = recordset->next_active;
        recordset->next_active = NULL;
        oasp_ado_recordset_finalize(recordset);
    }
    if (connection->sqlite)
        oasp_sqlite_pool_put(connection->component_context, connection->sqlite, connection->pool_key.data);
    if (connection->mysql)
        oasp_mysql_pool_put(connection->component_context, connection->mysql, connection->pool_key.data);
    connection->sqlite = NULL;
    connection->mysql = NULL;
    connection->active_recordsets = NULL;
    connection->opened = false;
}

#define oasp_ado_string oasp_component_string

static char *oasp_ado_connection_value(oasp_runtime *runtime, const char *text, const char *wanted) {
    const char *cursor = text ? text : "";
    while (*cursor) {
        while (*cursor == ';' || *cursor == ' ' || *cursor == '\t')
            cursor++;
        const char *end = strchr(cursor, ';');
        if (!end)
            end = cursor + strlen(cursor);
        const char *equals = memchr(cursor, '=', (size_t)(end - cursor));
        if (equals) {
            const char *key_end = equals;
            while (key_end > cursor && (key_end[-1] == ' ' || key_end[-1] == '\t'))
                key_end--;
            if (oasp_ascii_equal_n(cursor, (size_t)(key_end - cursor), wanted)) {
                const char *value = equals + 1;
                while (value < end && (*value == ' ' || *value == '\t'))
                    value++;
                while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
                    end--;
                return oasp_runtime_copy(runtime, value, (size_t)(end - value));
            }
        }
        cursor = *end ? end + 1 : end;
    }
    return oasp_runtime_copy(runtime, "", 0);
}

static bool oasp_ado_contains(const char *text, const char *needle) {
    size_t text_length = strlen(text);
    size_t needle_length = strlen(needle);
    if (needle_length > text_length)
        return false;
    for (size_t i = 0; i + needle_length <= text_length; i++)
        if (oasp_ascii_equal_n(text + i, needle_length, needle))
            return true;
    return false;
}

static oasp_ado_recordset *oasp_ado_recordset_new(oasp_runtime *runtime) {
    oasp_ado_recordset *recordset = oasp_runtime_alloc(runtime, sizeof(*recordset), _Alignof(oasp_ado_recordset));
    if (!recordset)
        return NULL;
    memset(recordset, 0, sizeof(*recordset));
    if (!oasp_runtime_defer(runtime, recordset, oasp_ado_recordset_finalize, recordset))
        return NULL;
    recordset->page_size = 10;
    recordset->source_exhausted = true;
    return recordset;
}

static oasp_value oasp_ado_recordset_value(oasp_runtime *runtime, oasp_ado_recordset *recordset) {
    oasp_object *object = oasp_component_object(runtime, OASP_COMPONENT_ADO_RECORDSET, "ADODB.Recordset", recordset);
    return object ? oasp_component_object_value(object) : oasp_value_empty();
}

static int oasp_ado_column_compare(const void *left_pointer, const void *right_pointer) {
    const oasp_ado_column *left = left_pointer;
    const oasp_ado_column *right = right_pointer;
    size_t common = left->length < right->length ? left->length : right->length;
    for (size_t i = 0; i < common; i++) {
        unsigned char a = (unsigned char)tolower((unsigned char)left->data[i]);
        unsigned char b = (unsigned char)tolower((unsigned char)right->data[i]);
        if (a != b)
            return a < b ? -1 : 1;
    }
    if (left->length != right->length)
        return left->length < right->length ? -1 : 1;
    return left->index < right->index ? -1 : left->index > right->index ? 1 : 0;
}

static int oasp_ado_column_compare_name(const oasp_ado_column *column, oasp_slice name) {
    size_t common = column->length < name.length ? column->length : name.length;
    for (size_t i = 0; i < common; i++) {
        unsigned char a = (unsigned char)tolower((unsigned char)column->data[i]);
        unsigned char b = (unsigned char)tolower((unsigned char)name.data[i]);
        if (a != b)
            return a < b ? -1 : 1;
    }
    return column->length < name.length ? -1 : column->length > name.length ? 1 : 0;
}

static bool oasp_ado_recordset_begin(oasp_ado_recordset *recordset, oasp_ado_column *columns, size_t column_count) {
    while (recordset->segments) {
        oasp_ado_row_segment *segment = recordset->segments;
        oasp_arena *arena = segment->arena;
        recordset->segments = segment->next;
        oasp_arena_destroy(arena);
    }
    recordset->segment_tail = NULL;
    recordset->current_segment = NULL;
    recordset->columns = columns;
    recordset->column_count = column_count;
    if (column_count > 1)
        qsort(columns, column_count, sizeof(*columns), oasp_ado_column_compare);
    recordset->row_count = 0;
    recordset->cursor = 0;
    recordset->opened = true;
    recordset->source_exhausted = true;
    return true;
}

static oasp_value *oasp_ado_recordset_append_row(oasp_ado_recordset *recordset, oasp_arena **row_arena) {
    if (!recordset || !recordset->column_count || recordset->row_count == SIZE_MAX)
        return NULL;
    oasp_ado_row_segment *segment = recordset->segment_tail;
    if (!segment || segment->row_count == segment->row_capacity) {
        if (recordset->column_count > SIZE_MAX / sizeof(oasp_value))
            return NULL;
        size_t row_bytes = recordset->column_count * sizeof(oasp_value);
        size_t row_capacity = row_bytes ? (16 * 1024) / row_bytes : 0;
        if (!row_capacity)
            row_capacity = 1;
        if (row_capacity > 64)
            row_capacity = 64;
        if (row_capacity > SIZE_MAX / row_bytes)
            return NULL;
        oasp_arena *arena = oasp_arena_create_mapped(16 * 1024);
        segment = arena ? oasp_arena_alloc(arena, sizeof(*segment), _Alignof(oasp_ado_row_segment)) : NULL;
        if (!segment) {
            oasp_arena_destroy(arena);
            return NULL;
        }
        memset(segment, 0, sizeof(*segment));
        segment->arena = arena;
        segment->first_row = recordset->row_count;
        segment->row_capacity = row_capacity;
        segment->cells = oasp_arena_alloc(arena, row_capacity * row_bytes, _Alignof(oasp_value));
        if (!segment->cells) {
            oasp_arena_destroy(arena);
            return NULL;
        }
        if (recordset->segment_tail)
            recordset->segment_tail->next = segment;
        else
            recordset->segments = segment;
        recordset->segment_tail = segment;
    }
    oasp_value *row = segment->cells + segment->row_count * recordset->column_count;
    memset(row, 0, recordset->column_count * sizeof(*row));
    segment->row_count++;
    recordset->row_count++;
    if (row_arena)
        *row_arena = segment->arena;
    return row;
}

static oasp_ado_row_segment *oasp_ado_recordset_segment(oasp_ado_recordset *recordset, size_t row) {
    if (!recordset)
        return NULL;
    oasp_ado_row_segment *segment = recordset->current_segment;
    if (segment && row >= segment->first_row && row - segment->first_row < segment->row_count)
        return segment;
    if (!segment || row < segment->first_row)
        segment = recordset->segments;
    else
        segment = segment->next;
    for (; segment; segment = segment->next) {
        if (row >= segment->first_row && row - segment->first_row < segment->row_count)
            break;
    }
    recordset->current_segment = segment;
    return segment;
}

static oasp_value *oasp_ado_recordset_row(oasp_ado_recordset *recordset, size_t row) {
    oasp_ado_row_segment *segment = oasp_ado_recordset_segment(recordset, row);
    if (segment)
        return segment->cells + (row - segment->first_row) * recordset->column_count;
    return NULL;
}

static void oasp_ado_recordset_release_consumed(oasp_ado_recordset *recordset) {
    if (!recordset || !recordset->sliding_window)
        return;
    while (recordset->segments && recordset->segments != recordset->segment_tail &&
           recordset->segments->first_row + recordset->segments->row_count <= recordset->cursor) {
        oasp_ado_row_segment *segment = recordset->segments;
        oasp_arena *arena = segment->arena;
        recordset->segments = segment->next;
        if (recordset->current_segment == segment)
            recordset->current_segment = NULL;
        oasp_arena_destroy(arena);
    }
}

static int oasp_ado_column_index_name(const oasp_ado_recordset *recordset, oasp_slice name) {
    size_t low = 0;
    size_t high = recordset->column_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int comparison = oasp_ado_column_compare_name(&recordset->columns[middle], name);
        if (comparison < 0)
            low = middle + 1;
        else
            high = middle;
    }
    if (low < recordset->column_count &&
        !oasp_ado_column_compare_name(&recordset->columns[low], name))
        return (int)recordset->columns[low].index;
    return -1;
}

static const oasp_ado_column *oasp_ado_column_by_index(const oasp_ado_recordset *recordset, size_t index) {
    for (size_t i = 0; i < recordset->column_count; i++)
        if (recordset->columns[i].index == index)
            return &recordset->columns[i];
    return NULL;
}

static int oasp_ado_column_index(oasp_runtime *runtime, const oasp_ado_recordset *recordset, const oasp_value *selector) {
    int64_t index = -1;
    if ((selector->kind == OASP_VALUE_INT || selector->kind == OASP_VALUE_FLOAT) && oasp_value_to_int(selector, &index))
        return index >= 0 && (size_t)index < recordset->column_count ? (int)index : -1;
    char buffer[64];
    oasp_slice name;
    if (!oasp_value_text(selector, buffer, sizeof(buffer), &name))
        return -1;
    (void)runtime;
    return oasp_ado_column_index_name(recordset, name);
}

static oasp_value oasp_ado_current_field(oasp_runtime *runtime, oasp_ado_recordset *recordset, const oasp_value *selector, size_t line) {
    if (recordset->cursor >= recordset->row_count && !recordset->source_exhausted)
        (void)oasp_ado_recordset_fetch_next(runtime, recordset, line);
    if (recordset->cursor >= recordset->row_count)
        return oasp_value_empty();
    int index = oasp_ado_column_index(runtime, recordset, selector);
    if (index < 0) {
        oasp_runtime_raise(runtime, 3265, "recordset field not found", "ADODB.Recordset", line);
        return oasp_value_empty();
    }
    oasp_value *row = oasp_ado_recordset_row(recordset, recordset->cursor);
    if (!row) {
        oasp_runtime_raise(runtime, 3021, "current record is outside the forward-only window", "ADODB.Recordset", line);
        return oasp_value_empty();
    }
    return recordset->sliding_window ? oasp_runtime_value_copy(runtime, &row[index], true) : row[index];
}

static oasp_value oasp_ado_fields(oasp_runtime *runtime, oasp_object *recordset_object) {
    oasp_ado_recordset *recordset = recordset_object->native;
    oasp_component_collection *collection = oasp_runtime_alloc(runtime, sizeof(*collection), _Alignof(oasp_component_collection));
    if (!collection)
        return oasp_value_empty();
    memset(collection, 0, sizeof(*collection));
    collection->count = recordset->column_count;
    collection->items = collection->count ? oasp_runtime_alloc_values(runtime, collection->count) : NULL;
    if (collection->count && !collection->items)
        return oasp_value_empty();
    for (size_t index = 0; index < collection->count; index++) {
        oasp_ado_field *field = oasp_runtime_alloc(runtime, sizeof(*field), _Alignof(oasp_ado_field));
        if (!field)
            return oasp_value_empty();
        field->recordset = recordset_object;
        field->index = index;
        oasp_object *field_object = oasp_component_object(runtime, OASP_COMPONENT_ADO_FIELD, "ADODB.Field", field);
        if (!field_object)
            return oasp_value_empty();
        collection->items[index] = oasp_component_object_value(field_object);
    }
    oasp_object *fields = oasp_component_object(runtime, OASP_COMPONENT_ADO_FIELDS, "ADODB.Fields", collection);
    return fields ? oasp_component_object_value(fields) : oasp_value_empty();
}

#include "ado_sqlite.c"

#include "ado_mysql.c"

#include "ado_mdb.c"

static oasp_value oasp_ado_execute(oasp_runtime *runtime, oasp_object *owner, const char *sql, size_t line) {
    oasp_ado_connection *connection = owner ? owner->native : NULL;
    if (!connection || !sql)
        return oasp_value_empty();
    if (connection->kind == OASP_DATABASE_SQLITE)
        return oasp_ado_execute_sqlite(runtime, owner, connection, sql, line);
    if (connection->kind == OASP_DATABASE_MDB)
        return oasp_ado_execute_mdb(runtime, owner, connection, sql, line);
    return oasp_ado_execute_mysql(runtime, owner, connection, sql, line);
}

#include "ado_connection.c"

static char *oasp_ado_command_sql(oasp_runtime *runtime, const oasp_ado_command *command) {
    oasp_buffer output;
    if (!oasp_buffer_init(&output, command->command_text.length + 64))
        return NULL;
    const char *cursor = command->command_text.data;
    const char *end = cursor + command->command_text.length;
    oasp_ado_parameters *parameters = command->parameters ? command->parameters->native : NULL;
    oasp_ado_parameter_link *parameter_link = parameters ? parameters->first : NULL;
    while (cursor < end) {
        if (*cursor != '?' || !parameter_link) {
            (void)oasp_buffer_append_byte(&output, *cursor++);
            continue;
        }
        oasp_ado_parameter *parameter = parameter_link->parameter->native;
        if (parameter->value.kind == OASP_VALUE_NULL) {
            (void)oasp_buffer_append(&output, "NULL", 4);
        } else if (parameter->type == 2 || parameter->type == 3 || parameter->type == 4 || parameter->type == 5 || parameter->type == 6 ||
                   parameter->type == 16 || parameter->type == 17 || parameter->type == 18 || parameter->type == 19 || parameter->type == 20 ||
                   parameter->type == 21) {
            char *value = oasp_value_format(runtime->arena, &parameter->value);
            (void)oasp_buffer_append(&output, value, strlen(value));
        } else {
            char *value = oasp_value_format(runtime->arena, &parameter->value);
            (void)oasp_buffer_append_byte(&output, '\'');
            for (size_t i = 0; value && value[i]; i++) {
                (void)oasp_buffer_append_byte(&output, value[i]);
                if (value[i] == '\'')
                    (void)oasp_buffer_append_byte(&output, '\'');
            }
            (void)oasp_buffer_append_byte(&output, '\'');
        }
        cursor++;
        parameter_link = parameter_link->next;
    }
    char *sql = oasp_runtime_copy(runtime, output.data, output.length);
    oasp_buffer_dispose(&output);
    return sql;
}


void *oasp_builtin_ado_create(oasp_runtime *runtime, oasp_component_kind kind) {
    if (kind == OASP_COMPONENT_ADO_CONNECTION) {
        oasp_ado_connection *connection = oasp_runtime_alloc(runtime, sizeof(*connection), _Alignof(oasp_ado_connection));
        if (!connection)
            return NULL;
        memset(connection, 0, sizeof(*connection));
        connection->component_context = runtime->component_context;
        return oasp_runtime_defer(runtime, connection, oasp_ado_connection_finalize, connection) ? connection : NULL;
    }
    if (kind == OASP_COMPONENT_ADO_RECORDSET)
        return oasp_ado_recordset_new(runtime);
    if (kind == OASP_COMPONENT_ADO_COMMAND) {
        oasp_ado_command *command = oasp_runtime_alloc(runtime, sizeof(*command), _Alignof(oasp_ado_command));
        if (!command)
            return NULL;
        memset(command, 0, sizeof(*command));
        command->command_timeout = 30;
        return command;
    }
    return NULL;
}

static bool oasp_ado_member_id(uint16_t actual, oasp_member_id expected, oasp_slice member, const char *name) {
    return actual ? actual == (uint16_t)expected : oasp_ascii_equal(member, name);
}

static bool oasp_ado_kind(oasp_component_kind kind) {
    return kind >= OASP_COMPONENT_ADO_CONNECTION && kind <= OASP_COMPONENT_ADO_PARAMETERS;
}

bool oasp_builtin_ado_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, size_t line, oasp_value *result) {
    if (!object || !oasp_ado_kind(object->component_kind))
        return false;
    if (object->component_kind == OASP_COMPONENT_ADO_CONNECTION && oasp_ascii_equal(member, "state")) {
        *result = oasp_value_int(((oasp_ado_connection *)object->native)->opened ? 1 : 0);
        return true;
    }
    if (object->component_kind == OASP_COMPONENT_ADO_RECORDSET) {
        oasp_ado_recordset *recordset = object->native;
        if (oasp_ascii_equal(member, "fields"))
            *result = oasp_ado_fields(runtime, object);
        else if (oasp_ascii_equal(member, "eof")) {
            if (recordset->cursor >= recordset->row_count && !recordset->source_exhausted)
                (void)oasp_ado_recordset_fetch_next(runtime, recordset, line);
            *result = oasp_value_bool(recordset->cursor >= recordset->row_count);
        } else if (oasp_ascii_equal(member, "bof")) {
            if (!recordset->row_count && !recordset->source_exhausted)
                (void)oasp_ado_recordset_fetch_next(runtime, recordset, line);
            *result = oasp_value_bool(!recordset->row_count);
        }
        else if (oasp_ascii_equal(member, "recordcount"))
            *result = oasp_value_int((int64_t)recordset->row_count);
        else if (oasp_ascii_equal(member, "pagesize"))
            *result = oasp_value_int((int64_t)recordset->page_size);
        else if (oasp_ascii_equal(member, "absolutepage"))
            *result = oasp_value_int((int64_t)(recordset->page_size ? recordset->cursor / recordset->page_size + 1 : 1));
        else if (oasp_ascii_equal(member, "pagecount")) {
            if (!oasp_ado_recordset_materialize(runtime, recordset, SIZE_MAX, line))
                return true;
            size_t pages = recordset->row_count / recordset->page_size;
            if (recordset->row_count % recordset->page_size)
                pages++;
            *result = oasp_value_int((int64_t)pages);
        }
        else if (oasp_ascii_equal(member, "state"))
            *result = oasp_value_int(recordset->opened ? 1 : 0);
        else if (oasp_ascii_equal(member, "cursortype"))
            *result = oasp_value_int(recordset->cursor_type);
        else if (oasp_ascii_equal(member, "locktype"))
            *result = oasp_value_int(recordset->lock_type);
        else if (oasp_ascii_equal(member, "activeconnection"))
            *result = recordset->connection ? oasp_component_object_value(recordset->connection) : oasp_value_nothing();
        else if (oasp_ascii_equal(member, "source"))
            *result = oasp_value_string(recordset->source.data, recordset->source.length);
        else
            return false;
        return true;
    }
    if (object->component_kind == OASP_COMPONENT_ADO_FIELD) {
        oasp_ado_field *field = object->native;
        oasp_ado_recordset *recordset = field->recordset ? field->recordset->native : NULL;
        if (!recordset || field->index >= recordset->column_count) {
            oasp_runtime_raise(runtime, 3265, "recordset field not found", object->component_name, line);
            *result = oasp_value_empty();
        } else if (oasp_ascii_equal(member, "name")) {
            const oasp_ado_column *column = oasp_ado_column_by_index(recordset, field->index);
            *result = column ? oasp_value_string(column->data, column->length) : oasp_value_empty();
        } else if (oasp_ascii_equal(member, "value")) {
            oasp_value index = oasp_value_int((int64_t)field->index);
            *result = oasp_ado_current_field(runtime, recordset, &index, line);
        } else
            return false;
        return true;
    }
    if (object->component_kind == OASP_COMPONENT_ADO_COMMAND && oasp_ascii_equal(member, "parameters")) {
        oasp_ado_command *command = object->native;
        if (!command->parameters) {
            oasp_ado_parameters *parameters = oasp_runtime_alloc(runtime, sizeof(*parameters), _Alignof(oasp_ado_parameters));
            if (!parameters)
                return true;
            memset(parameters, 0, sizeof(*parameters));
            command->parameters = oasp_component_object(runtime, OASP_COMPONENT_ADO_PARAMETERS, "ADODB.Parameters", parameters);
        }
        *result = command->parameters ? oasp_component_object_value(command->parameters) : oasp_value_empty();
        return true;
    }
    if (object->component_kind == OASP_COMPONENT_ADO_PARAMETERS && oasp_ascii_equal(member, "count")) {
        *result = oasp_value_int((int64_t)((oasp_ado_parameters *)object->native)->count);
        return true;
    }
    return false;
}

bool oasp_builtin_ado_get_id(oasp_runtime *runtime, oasp_object *object, uint16_t member_id, oasp_slice member, size_t line,
                             oasp_value *result) {
    if (object && object->component_kind == OASP_COMPONENT_ADO_RECORDSET && member_id == (uint16_t)OASP_MEMBER_EOF) {
        oasp_ado_recordset *recordset = object->native;
        if (recordset->cursor >= recordset->row_count && !recordset->source_exhausted)
            (void)oasp_ado_recordset_fetch_next(runtime, recordset, line);
        *result = oasp_value_bool(recordset->cursor >= recordset->row_count);
        return true;
    }
    return oasp_builtin_ado_get(runtime, object, member, line, result);
}

bool oasp_builtin_ado_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line) {
    if (!object || !oasp_ado_kind(object->component_kind))
        return false;
    int64_t integer = 0;
    if (object->component_kind == OASP_COMPONENT_ADO_CONNECTION && oasp_ascii_equal(member, "connectionstring")) {
        ((oasp_ado_connection *)object->native)->connection_string = oasp_component_string(runtime, oasp_value_format(runtime->arena, value));
        return true;
    }
    if (object->component_kind == OASP_COMPONENT_ADO_RECORDSET) {
        oasp_ado_recordset *recordset = object->native;
        if (oasp_ascii_equal(member, "activeconnection")) {
            oasp_object *connection = value->kind == OASP_VALUE_OBJECT &&
                                              value->as.object->component_kind == OASP_COMPONENT_ADO_CONNECTION
                                          ? value->as.object
                                          : NULL;
            if (value->kind != OASP_VALUE_NOTHING && !connection) {
                oasp_runtime_raise(runtime, 13, "ActiveConnection must be an ADODB.Connection object", object->component_name, line);
            } else {
                oasp_ado_recordset_detach_active(recordset);
                recordset->connection = connection;
                if (connection && recordset->opened)
                    oasp_ado_recordset_attach_active(recordset);
            }
        } else if (oasp_ascii_equal(member, "source"))
            recordset->source = oasp_component_string(runtime, oasp_value_format(runtime->arena, value));
        else if (oasp_ascii_equal(member, "cursortype") || oasp_ascii_equal(member, "locktype")) {
            if (!oasp_value_to_int(value, &integer)) oasp_runtime_raise(runtime, 13, "type mismatch", object->component_name, line);
            else if (oasp_ascii_equal(member, "cursortype")) { recordset->cursor_type = (int)integer; if (integer) recordset->sliding_window = false; }
            else recordset->lock_type = (int)integer;
        } else if (oasp_ascii_equal(member, "pagesize")) {
            if (!oasp_value_to_int(value, &integer) || integer <= 0) oasp_runtime_raise(runtime, 3001, "PageSize must be greater than zero", object->component_name, line);
            else recordset->page_size = (size_t)integer;
        } else if (oasp_ascii_equal(member, "absolutepage")) {
            if (!oasp_value_to_int(value, &integer) || integer <= 0) {
                oasp_runtime_raise(runtime, 3001, "AbsolutePage must be greater than zero", object->component_name, line);
            } else {
                size_t page = (size_t)integer - 1;
                size_t offset = page > SIZE_MAX / recordset->page_size ? SIZE_MAX : page * recordset->page_size;
                if (recordset->segments && offset < recordset->segments->first_row) {
                    oasp_runtime_raise(runtime, 3219, "AbsolutePage is outside the forward-only row window", object->component_name, line);
                } else {
                    recordset->sliding_window = false;
                    size_t target = offset == SIZE_MAX ? SIZE_MAX : offset + 1;
                    (void)oasp_ado_recordset_materialize(runtime, recordset, target, line);
                    recordset->cursor = offset < recordset->row_count ? offset : recordset->row_count;
                }
            }
        } else return false;
        return true;
    }
    if (object->component_kind == OASP_COMPONENT_ADO_COMMAND) {
        oasp_ado_command *command = object->native;
        if (oasp_ascii_equal(member, "activeconnection")) {
            if (value->kind == OASP_VALUE_OBJECT && value->as.object->component_kind == OASP_COMPONENT_ADO_CONNECTION) command->connection = value->as.object;
            else oasp_runtime_raise(runtime, 13, "ActiveConnection must be an ADODB.Connection object", object->component_name, line);
        } else if (oasp_ascii_equal(member, "commandtext")) command->command_text = oasp_component_string(runtime, oasp_value_format(runtime->arena, value));
        else if (oasp_ascii_equal(member, "commandtype") && oasp_value_to_int(value, &integer)) command->command_type = (int)integer;
        else if (oasp_ascii_equal(member, "commandtimeout") && oasp_value_to_int(value, &integer)) command->command_timeout = (int)integer;
        else if (oasp_ascii_equal(member, "prepared")) command->prepared = oasp_value_truthy(value);
        else return false;
        return true;
    }
    if (object->component_kind == OASP_COMPONENT_ADO_PARAMETER) {
        oasp_ado_parameter *parameter = object->native;
        if (oasp_ascii_equal(member, "name")) parameter->name = oasp_component_string(runtime, oasp_value_format(runtime->arena, value));
        else if (oasp_ascii_equal(member, "value")) parameter->value = oasp_runtime_value_copy(runtime, value, false);
        else if ((oasp_ascii_equal(member, "type") || oasp_ascii_equal(member, "direction") || oasp_ascii_equal(member, "size")) && oasp_value_to_int(value, &integer)) {
            if (oasp_ascii_equal(member, "type")) parameter->type = (int)integer;
            else if (oasp_ascii_equal(member, "direction")) parameter->direction = (int)integer;
            else parameter->size = (int)integer;
        } else return false;
        return true;
    }
    return false;
}

static bool oasp_ado_recordset_open(oasp_runtime *runtime, oasp_object *object, oasp_value *arguments, size_t argument_count, size_t line) {
    oasp_ado_recordset *recordset = object->native;
    char *sql = argument_count ? oasp_value_format(runtime->arena, &arguments[0])
                               : oasp_runtime_copy(runtime, recordset->source.data, recordset->source.length);
    oasp_object *connection = recordset->connection;
    if (argument_count > 1 && arguments[1].kind == OASP_VALUE_OBJECT &&
        arguments[1].as.object->component_kind == OASP_COMPONENT_ADO_CONNECTION)
        connection = arguments[1].as.object;
    if (argument_count > 1 && arguments[1].kind == OASP_VALUE_STRING) {
        oasp_value created = oasp_component_create(runtime, (oasp_slice){"ADODB.Connection", 16}, line);
        if (created.kind == OASP_VALUE_OBJECT &&
            oasp_ado_connection_open(runtime, created.as.object, oasp_value_format(runtime->arena, &arguments[1]), line))
            connection = created.as.object;
    }
    if (!connection) {
        oasp_runtime_raise(runtime, 91, "ActiveConnection is not set", object->component_name, line);
        return true;
    }
    oasp_value opened = oasp_ado_execute(runtime, connection, sql ? sql : "", line);
    if (opened.kind != OASP_VALUE_OBJECT || opened.as.object->component_kind != OASP_COMPONENT_ADO_RECORDSET)
        return true;
    oasp_ado_recordset *source = opened.as.object->native;
    int cursor_type = recordset->cursor_type;
    int lock_type = recordset->lock_type;
    oasp_ado_recordset_finalize(recordset);
    *recordset = *source;
    oasp_ado_recordset_detach_active(source);
    source->segments = NULL;
    source->segment_tail = NULL;
    source->mysql_result = NULL;
    source->row_source = NULL;
    source->row_source_fetch = NULL;
    source->row_source_close = NULL;
    source->next_active = NULL;
    source->source_exhausted = true;
    recordset->cursor_type = cursor_type;
    recordset->lock_type = lock_type;
    recordset->sliding_window = cursor_type == 0 && recordset->row_source;
    recordset->connection = connection;
    oasp_ado_recordset_attach_active(recordset);
    return true;
}

static bool oasp_ado_recordset_get_rows(oasp_runtime *runtime, oasp_object *object, oasp_value *arguments, size_t argument_count, size_t line,
                                        oasp_value *result) {
    oasp_ado_recordset *recordset = object->native;
    size_t requested_rows = SIZE_MAX;
    if (argument_count) {
        int64_t requested = -1;
        if (!oasp_value_to_int(&arguments[0], &requested)) {
            oasp_runtime_raise(runtime, 13, "type mismatch", object->component_name, line);
            return true;
        }
        if (requested >= 0)
            requested_rows = (size_t)requested;
    }
    size_t target = requested_rows == SIZE_MAX || requested_rows > SIZE_MAX - recordset->cursor
                        ? SIZE_MAX : recordset->cursor + requested_rows;
    if (!oasp_ado_recordset_materialize(runtime, recordset, target, line))
        return true;
    size_t rows = recordset->cursor < recordset->row_count ? recordset->row_count - recordset->cursor : 0;
    if (requested_rows < rows)
        rows = requested_rows;
    oasp_array *array = oasp_runtime_alloc_array(runtime);
    if (!array)
        return true;
    memset(array, 0, sizeof(*array));
    array->rank = 2;
    array->bounds = oasp_runtime_alloc(runtime, 2 * sizeof(*array->bounds), _Alignof(int64_t));
    array->length = recordset->column_count * rows;
    array->items = array->length ? oasp_runtime_alloc_values(runtime, array->length) : NULL;
    if (!array->bounds || (array->length && !array->items))
        return true;
    array->bounds[0] = recordset->column_count ? (int64_t)recordset->column_count - 1 : -1;
    array->bounds[1] = rows ? (int64_t)rows - 1 : -1;
    for (size_t column = 0; column < recordset->column_count; column++) {
        for (size_t row = 0; row < rows; row++) {
            oasp_value *source = oasp_ado_recordset_row(recordset, recordset->cursor + row);
            if (!source) {
                oasp_runtime_raise(runtime, 3021, "requested rows are outside the forward-only window", object->component_name, line);
                return true;
            }
            array->items[column * rows + row] = oasp_runtime_value_copy(runtime, &source[column], true);
        }
    }
    recordset->cursor += rows;
    oasp_ado_recordset_release_consumed(recordset);
    result->kind = OASP_VALUE_ARRAY;
    result->as.array = array;
    return true;
}

bool oasp_builtin_ado_call(oasp_runtime *runtime, oasp_object *object, uint16_t member_id, oasp_slice member, oasp_value *arguments,
                           size_t argument_count, size_t line, oasp_value *result) {
    if (!object || !oasp_ado_kind(object->component_kind))
        return false;
    *result = oasp_value_empty();
    if (object->component_kind == OASP_COMPONENT_ADO_CONNECTION) {
        oasp_ado_connection *connection = object->native;
        if (oasp_ado_member_id(member_id, OASP_MEMBER_OPEN, member, "open")) {
            char *text = argument_count ? oasp_value_format(runtime->arena, &arguments[0])
                                        : oasp_runtime_copy(runtime, connection->connection_string.data, connection->connection_string.length);
            (void)oasp_ado_connection_open(runtime, object, text ? text : "", line);
        } else if (oasp_ado_member_id(member_id, OASP_MEMBER_CLOSE, member, "close")) {
            oasp_ado_connection_finalize(connection);
        } else if (oasp_ado_member_id(member_id, OASP_MEMBER_EXECUTE, member, "execute")) {
            char *sql = argument_count ? oasp_value_format(runtime->arena, &arguments[0]) : NULL;
            *result = sql ? oasp_ado_execute(runtime, object, sql, line) : oasp_value_empty();
        } else if (oasp_ascii_equal(member, "begintrans") || oasp_ascii_equal(member, "committrans") || oasp_ascii_equal(member, "rollbacktrans")) {
            if (connection->kind == OASP_DATABASE_MDB) {
                bool begin = oasp_ascii_equal(member, "begintrans");
                bool ok = begin ? oasp_ado_mdb_transaction_begin(connection)
                                : oasp_ado_mdb_transaction_finish(connection, oasp_ascii_equal(member, "rollbacktrans"));
                if (!ok) oasp_runtime_raise(runtime, 3000, begin ? "unable to begin MDB transaction" : "unable to finish MDB transaction", object->component_name, line);
                *result = oasp_value_int(begin && connection->mdb_transaction_active ? 1 : 0);
            } else {
                const char *sql = oasp_ascii_equal(member, "begintrans") ? "BEGIN" : oasp_ascii_equal(member, "committrans") ? "COMMIT" : "ROLLBACK";
                (void)oasp_ado_execute(runtime, object, sql, line);
                *result = oasp_value_int(oasp_ascii_equal(member, "begintrans") ? 1 : 0);
            }
        } else return false;
        return true;
    }
    if (object->component_kind == OASP_COMPONENT_ADO_RECORDSET) {
        oasp_ado_recordset *recordset = object->native;
        if (oasp_ado_member_id(member_id, OASP_MEMBER_FIELDS, member, "fields") || oasp_ado_member_id(member_id, OASP_MEMBER_ITEM, member, "item"))
            *result = argument_count ? oasp_ado_current_field(runtime, recordset, &arguments[0], line) : oasp_value_empty();
        else if (oasp_ado_member_id(member_id, OASP_MEMBER_MOVENEXT, member, "movenext")) {
            if (recordset->cursor < recordset->row_count) recordset->cursor++;
            if (recordset->cursor >= recordset->row_count && !recordset->source_exhausted) (void)oasp_ado_recordset_fetch_next(runtime, recordset, line);
            oasp_ado_recordset_release_consumed(recordset);
        } else if (oasp_ascii_equal(member, "movefirst")) {
            if (recordset->segments && recordset->segments->first_row) oasp_runtime_raise(runtime, 3219, "MoveFirst is not supported after a forward-only row window was released", object->component_name, line);
            else { recordset->sliding_window = false; recordset->cursor = 0; }
        } else if (oasp_ado_member_id(member_id, OASP_MEMBER_CLOSE, member, "close")) {
            oasp_ado_recordset_close_source(recordset); recordset->opened = false;
        } else if (oasp_ado_member_id(member_id, OASP_MEMBER_OPEN, member, "open")) {
            (void)oasp_ado_recordset_open(runtime, object, arguments, argument_count, line);
        } else if (oasp_ascii_equal(member, "getrows")) {
            (void)oasp_ado_recordset_get_rows(runtime, object, arguments, argument_count, line, result);
        } else if (oasp_ascii_equal(member, "addnew")) {
            oasp_arena *arena = NULL;
            if (oasp_ado_recordset_append_row(recordset, &arena)) { recordset->cursor = recordset->row_count - 1; recordset->adding_new = true; }
            else oasp_runtime_raise(runtime, 7, "out of memory", object->component_name, line);
        } else if (oasp_ascii_equal(member, "update") || oasp_ascii_equal(member, "cancelupdate")) {
            if (!recordset->opened) oasp_runtime_raise(runtime, 3704, "operation is not allowed when the object is closed", object->component_name, line);
            else if (oasp_ascii_equal(member, "update")) (void)oasp_ado_recordset_update_mdb(runtime, object, recordset, line);
        } else if (oasp_ascii_equal(member, "delete")) {
            if (!recordset->opened) oasp_runtime_raise(runtime, 3704, "operation is not allowed when the object is closed", object->component_name, line);
            else (void)oasp_ado_recordset_delete_mdb(runtime, object, recordset, line);
        } else return false;
        return true;
    }
    if (object->component_kind == OASP_COMPONENT_ADO_COMMAND) {
        oasp_ado_command *command = object->native;
        if (oasp_ascii_equal(member, "createparameter")) {
            oasp_ado_parameter *parameter = oasp_runtime_alloc(runtime, sizeof(*parameter), _Alignof(oasp_ado_parameter));
            if (!parameter) return true;
            memset(parameter, 0, sizeof(*parameter));
            int64_t number = 0;
            if (argument_count) parameter->name = oasp_component_string(runtime, oasp_value_format(runtime->arena, &arguments[0]));
            if (argument_count > 1 && oasp_value_to_int(&arguments[1], &number)) parameter->type = (int)number;
            if (argument_count > 2 && oasp_value_to_int(&arguments[2], &number)) parameter->direction = (int)number;
            if (argument_count > 3 && oasp_value_to_int(&arguments[3], &number)) parameter->size = (int)number;
            if (argument_count > 4) parameter->value = oasp_runtime_value_copy(runtime, &arguments[4], false);
            oasp_object *created = oasp_component_object(runtime, OASP_COMPONENT_ADO_PARAMETER, "ADODB.Parameter", parameter);
            *result = created ? oasp_component_object_value(created) : oasp_value_empty();
        } else if (oasp_ado_member_id(member_id, OASP_MEMBER_EXECUTE, member, "execute")) {
            if (!command->connection) oasp_runtime_raise(runtime, 91, "ActiveConnection is not set", object->component_name, line);
            else { char *sql = oasp_ado_command_sql(runtime, command); *result = sql ? oasp_ado_execute(runtime, command->connection, sql, line) : oasp_value_empty(); }
        } else return false;
        return true;
    }
    if (object->component_kind == OASP_COMPONENT_ADO_PARAMETERS) {
        oasp_ado_parameters *parameters = object->native;
        if (oasp_ascii_equal(member, "append") && argument_count && arguments[0].kind == OASP_VALUE_OBJECT && arguments[0].as.object->component_kind == OASP_COMPONENT_ADO_PARAMETER) {
            oasp_ado_parameter_link *link = oasp_runtime_alloc(runtime, sizeof(*link), _Alignof(oasp_ado_parameter_link));
            if (link) { link->next = NULL; link->parameter = arguments[0].as.object; if (parameters->last) parameters->last->next = link; else parameters->first = link; parameters->last = link; parameters->count++; }
        } else if (oasp_ascii_equal(member, "delete") && argument_count) {
            int64_t index = -1;
            (void)oasp_value_to_int(&arguments[0], &index);
            oasp_ado_parameter_link *previous = NULL;
            oasp_ado_parameter_link *link = parameters->first;
            for (int64_t i = 0; link && i < index; i++) {
                previous = link;
                link = link->next;
            }
            if (link && index >= 0) {
                if (previous) previous->next = link->next;
                else parameters->first = link->next;
                if (parameters->last == link) parameters->last = previous;
                parameters->count--;
            }
        } else if (oasp_ascii_equal(member, "item") && argument_count) {
            int64_t index = -1; (void)oasp_value_to_int(&arguments[0], &index);
            oasp_ado_parameter_link *link = parameters->first; for (int64_t i = 0; link && i < index; i++) link = link->next;
            *result = link && index >= 0 ? oasp_component_object_value(link->parameter) : oasp_value_empty();
        } else return false;
        return true;
    }
    return false;
}

bool oasp_builtin_ado_default(oasp_runtime *runtime, oasp_object *object, oasp_value *arguments, size_t argument_count, size_t line,
                              oasp_value *result) {
    if (!object || !oasp_ado_kind(object->component_kind)) return false;
    if (object->component_kind == OASP_COMPONENT_ADO_RECORDSET) {
        *result = argument_count ? oasp_ado_current_field(runtime, object->native, &arguments[0], line) : oasp_value_empty();
        return true;
    }
    if (object->component_kind == OASP_COMPONENT_ADO_PARAMETERS)
        return oasp_builtin_ado_call(runtime, object, 0, (oasp_slice){"item", 4}, arguments, argument_count, line, result);
    if (object->component_kind == OASP_COMPONENT_ADO_PARAMETER && !argument_count) { *result = ((oasp_ado_parameter *)object->native)->value; return true; }
    if (object->component_kind == OASP_COMPONENT_ADO_FIELD && !argument_count) {
        oasp_ado_field *field = object->native; oasp_ado_recordset *recordset = field->recordset ? field->recordset->native : NULL;
        oasp_value index = oasp_value_int((int64_t)field->index); *result = recordset ? oasp_ado_current_field(runtime, recordset, &index, line) : oasp_value_empty(); return true;
    }
    return false;
}

bool oasp_builtin_ado_assign(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                             const oasp_value *value, size_t line) {
    if (!object || object->component_kind != OASP_COMPONENT_ADO_RECORDSET ||
        (member.length && !oasp_ascii_equal(member, "fields") && !oasp_ascii_equal(member, "item"))) return false;
    oasp_ado_recordset *recordset = object->native;
    if (!argument_count || recordset->cursor >= recordset->row_count) { oasp_runtime_raise(runtime, 3021, "no current record", object->component_name, line); return true; }
    int index = oasp_ado_column_index(runtime, recordset, &arguments[0]);
    oasp_value *row = index >= 0 ? oasp_ado_recordset_row(recordset, recordset->cursor) : NULL;
    if (index < 0) oasp_runtime_raise(runtime, 3265, "recordset field not found", object->component_name, line);
    else if (!row) oasp_runtime_raise(runtime, 3021, "current record is outside the forward-only window", object->component_name, line);
    else {
        oasp_ado_row_segment *segment = oasp_ado_recordset_segment(recordset, recordset->cursor);
        oasp_runtime row_runtime = *runtime;
        row_runtime.arena = segment ? segment->arena : runtime->arena;
        row[index] = oasp_runtime_persist_copy(&row_runtime, value);
        if (recordset->dirty_columns)
            recordset->dirty_columns[index] = true;
    }
    return true;
}

bool oasp_builtin_ado_recordset_field_view(oasp_runtime *runtime, oasp_object *object, oasp_slice column, int *ordinal, const void **schema,
                                           bool *copy_text, size_t line, const oasp_value **result) {
    if (!runtime || !object || object->component_kind != OASP_COMPONENT_ADO_RECORDSET || !result)
        return false;
    oasp_ado_recordset *recordset = object->native;
    if (copy_text)
        *copy_text = recordset->sliding_window;
    if (schema && *schema != recordset->columns) {
        *schema = recordset->columns;
        if (ordinal)
            *ordinal = -1;
    }
    if (recordset->cursor >= recordset->row_count && !recordset->source_exhausted)
        (void)oasp_ado_recordset_fetch_next(runtime, recordset, line);
    if (recordset->cursor >= recordset->row_count) { *result = NULL; return true; }
    int index = ordinal && *ordinal >= 0 ? *ordinal : oasp_ado_column_index_name(recordset, column);
    if (index < 0) { oasp_runtime_raise(runtime, 3265, "recordset field not found", "ADODB.Recordset", line); *result = NULL; return true; }
    if (ordinal) *ordinal = index;
    oasp_value *row = oasp_ado_recordset_row(recordset, recordset->cursor);
    if (!row) { oasp_runtime_raise(runtime, 3021, "current record is outside the forward-only window", "ADODB.Recordset", line); *result = NULL; return true; }
    *result = &row[index];
    return true;
}

bool oasp_builtin_ado_recordset_move_next(oasp_runtime *runtime, oasp_object *object, size_t line) {
    if (!runtime || !object || object->component_kind != OASP_COMPONENT_ADO_RECORDSET) return false;
    oasp_ado_recordset *recordset = object->native;
    if (recordset->cursor < recordset->row_count) recordset->cursor++;
    if (recordset->cursor >= recordset->row_count && !recordset->source_exhausted) (void)oasp_ado_recordset_fetch_next(runtime, recordset, line);
    oasp_ado_recordset_release_consumed(recordset);
    return true;
}

bool oasp_builtin_ado_recordset_eof(oasp_runtime *runtime, oasp_object *object, size_t line, bool *result) {
    if (!runtime || !object || object->component_kind != OASP_COMPONENT_ADO_RECORDSET || !result) return false;
    oasp_ado_recordset *recordset = object->native;
    if (recordset->cursor >= recordset->row_count && !recordset->source_exhausted) (void)oasp_ado_recordset_fetch_next(runtime, recordset, line);
    *result = recordset->cursor >= recordset->row_count;
    return true;
}
