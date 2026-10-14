// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/storage/state_store.c`): Persistent/shared storage. It validates binary
 * formats and versions before exposing borrowed views, and coordinates process-safe publication
 * or state updates. Offset, size, generation, and lock variables protect consistency across
 * workers.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#define _POSIX_C_SOURCE 200809L

#include "state_store.h"

#include "component.h"
#include "json_compat.h"
#include "state_binary.h"

#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define OASP_STATE_ABI 1
#define OASP_STATE_MAX_DEPTH 128

struct oasp_state_store {
    sqlite3 *database;
    int lock_fd;
};

static bool oasp_state_lock_region(oasp_state_store *store, off_t start, short type) {
    if (!store || store->lock_fd < 0)
        return false;
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = type;
    lock.l_whence = SEEK_SET;
    lock.l_start = start;
    lock.l_len = 1;
    for (;;) {
        if (fcntl(store->lock_fd, F_SETLKW, &lock) == 0)
            return true;
        if (errno != EINTR)
            return false;
    }
}

static uint64_t oasp_state_hash(const char *text) {
    uint64_t hash = UINT64_C(1469598103934665603);
    while (*text) {
        hash ^= (unsigned char)*text++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool oasp_state_lock(oasp_state_store *store, const char *id, short type) {
    return oasp_state_lock_region(store, (off_t)(1 + oasp_state_hash(id) % 65535u), type);
}

bool oasp_state_store_lock_session(oasp_state_store *store, const char *id) { return store && id && oasp_state_lock(store, id, F_WRLCK); }

void oasp_state_store_unlock_session(oasp_state_store *store, const char *id) {
    if (store && id)
        (void)oasp_state_lock(store, id, F_UNLCK);
}

static bool oasp_state_decode_value(oasp_runtime *runtime, struct json_object *encoded, oasp_value *value, size_t depth);

static bool oasp_state_decode_bindings(oasp_runtime *runtime, struct json_object *encoded, oasp_binding **bindings, size_t depth) {
    if (!runtime || !encoded || !bindings || depth > OASP_STATE_MAX_DEPTH || !json_object_is_type(encoded, json_type_object))
        return false;
    oasp_binding *first = NULL;
    json_object_object_foreach(encoded, name, entry) {
        struct json_object *constant = NULL;
        struct json_object *encoded_value = NULL;
        if (!json_object_is_type(entry, json_type_object) || !json_object_object_get_ex(entry, "constant", &constant) ||
            !json_object_object_get_ex(entry, "value", &encoded_value))
            return false;
        oasp_binding *binding = oasp_arena_alloc(runtime->arena, sizeof(*binding), _Alignof(oasp_binding));
        if (!binding)
            return false;
        memset(binding, 0, sizeof(*binding));
        binding->name.length = strlen(name);
        binding->name.data = oasp_arena_copy(runtime->arena, name, binding->name.length);
        binding->constant = json_object_get_boolean(constant);
        if (!binding->name.data || !oasp_state_decode_value(runtime, encoded_value, &binding->value, depth + 1))
            return false;
        binding->next = first;
        first = binding;
    }
    *bindings = first;
    return true;
}

static bool oasp_state_decode_value(oasp_runtime *runtime, struct json_object *encoded, oasp_value *value, size_t depth) {
    struct json_object *type_object = NULL;
    if (!runtime || !encoded || !value || depth > OASP_STATE_MAX_DEPTH || !json_object_object_get_ex(encoded, "type", &type_object))
        return false;
    int type = json_object_get_int(type_object);
    if (type < OASP_VALUE_EMPTY || type > OASP_VALUE_DATE)
        return false;
    *value = oasp_value_empty();
    value->kind = (oasp_value_kind)type;
    if (type == OASP_VALUE_EMPTY || type == OASP_VALUE_NULL || type == OASP_VALUE_NOTHING)
        return true;
    struct json_object *data = NULL;
    if (!json_object_object_get_ex(encoded, "data", &data))
        return false;
    if (type == OASP_VALUE_BOOL) {
        value->as.boolean = json_object_get_boolean(data);
        return true;
    }
    if (type == OASP_VALUE_INT) {
        value->as.integer = json_object_get_int64(data);
        return true;
    }
    if (type == OASP_VALUE_FLOAT) {
        value->as.floating = json_object_get_double(data);
        return true;
    }
    if (type == OASP_VALUE_STRING || type == OASP_VALUE_DATE) {
        size_t length = (size_t)json_object_get_string_len(data);
        char *copy = oasp_arena_copy(runtime->arena, json_object_get_string(data), length);
        if (!copy)
            return false;
        oasp_slice slice = {copy, length};
        if (type == OASP_VALUE_STRING)
            value->as.string = slice;
        else
            value->as.date = slice;
        return true;
    }
    if (type == OASP_VALUE_ARRAY) {
        struct json_object *bounds = NULL;
        struct json_object *items = NULL;
        if (!json_object_object_get_ex(data, "bounds", &bounds) || !json_object_object_get_ex(data, "items", &items) ||
            !json_object_is_type(bounds, json_type_array) || !json_object_is_type(items, json_type_array))
            return false;
        size_t rank = json_object_array_length(bounds);
        size_t length = json_object_array_length(items);
        oasp_array *array = oasp_arena_alloc(runtime->arena, sizeof(*array), _Alignof(oasp_array));
        if (!array)
            return false;
        memset(array, 0, sizeof(*array));
        array->rank = rank;
        array->length = length;
        if (rank) {
            array->bounds = oasp_arena_alloc(runtime->arena, rank * sizeof(*array->bounds), _Alignof(int64_t));
            if (!array->bounds)
                return false;
            for (size_t i = 0; i < rank; i++)
                array->bounds[i] = json_object_get_int64(json_object_array_get_idx(bounds, i));
        }
        if (length) {
            array->items = oasp_arena_alloc(runtime->arena, length * sizeof(*array->items), _Alignof(oasp_value));
            if (!array->items)
                return false;
            for (size_t i = 0; i < length; i++)
                if (!oasp_state_decode_value(runtime, json_object_array_get_idx(items, i), &array->items[i], depth + 1))
                    return false;
        }
        value->as.array = array;
        return true;
    }
    if (type == OASP_VALUE_OBJECT) {
        struct json_object *kind = NULL;
        if (!json_object_object_get_ex(data, "kind", &kind) || !json_object_is_type(kind, json_type_string))
            return false;
        const char *kind_name = json_object_get_string(kind);
        if (!kind_name)
            return false;
        if (!strcmp(kind_name, "dictionary")) {
            struct json_object *mode = NULL;
            struct json_object *entries = NULL;
            if (!json_object_object_get_ex(data, "compare_mode", &mode) || !json_object_object_get_ex(data, "entries", &entries) ||
                !json_object_is_type(entries, json_type_array))
                return false;
            size_t count = json_object_array_length(entries);
            oasp_value *keys = count ? oasp_arena_alloc(runtime->arena, count * sizeof(*keys), _Alignof(oasp_value)) : NULL;
            oasp_value *values = count ? oasp_arena_alloc(runtime->arena, count * sizeof(*values), _Alignof(oasp_value)) : NULL;
            if (count && (!keys || !values))
                return false;
            for (size_t i = 0; i < count; i++) {
                struct json_object *pair = json_object_array_get_idx(entries, i);
                if (!pair || json_object_array_length(pair) != 2 ||
                    !oasp_state_decode_value(runtime, json_object_array_get_idx(pair, 0), &keys[i], depth + 1) ||
                    !oasp_state_decode_value(runtime, json_object_array_get_idx(pair, 1), &values[i], depth + 1))
                    return false;
            }
            value->as.object = oasp_component_dictionary_restore(runtime, json_object_get_int(mode), keys, values, count);
            return value->as.object != NULL;
        }
        if (!strcmp(kind_name, "class")) {
            struct json_object *name = NULL;
            struct json_object *fields = NULL;
            if (!json_object_object_get_ex(data, "name", &name) || !json_object_object_get_ex(data, "fields", &fields))
                return false;
            oasp_object *object = oasp_arena_alloc(runtime->arena, sizeof(*object), _Alignof(oasp_object));
            oasp_class_def *definition = oasp_arena_alloc(runtime->arena, sizeof(*definition), _Alignof(oasp_class_def));
            if (!object || !definition)
                return false;
            memset(object, 0, sizeof(*object));
            memset(definition, 0, sizeof(*definition));
            definition->name.length = (size_t)json_object_get_string_len(name);
            definition->name.data = oasp_arena_copy(runtime->arena, json_object_get_string(name), definition->name.length);
            if (!definition->name.data || !oasp_state_decode_bindings(runtime, fields, &object->fields, depth + 1))
                return false;
            object->class_def = definition;
            value->as.object = object;
            return true;
        }
    }
    return false;
}

static struct json_object *oasp_state_parse_store(const char *text) {
    if (!text)
        return json_object_new_object();
    struct json_object *root = json_tokener_parse(text);
    struct json_object *abi = NULL;
    struct json_object *bindings = NULL;
    if (!root || !json_object_object_get_ex(root, "abi", &abi) || json_object_get_int(abi) != OASP_STATE_ABI ||
        !json_object_object_get_ex(root, "bindings", &bindings) || !json_object_is_type(bindings, json_type_object)) {
        if (root)
            oasp_json_object_put(root);
        return NULL;
    }
    json_object_get(bindings);
    json_object_object_del(root, "bindings");
    oasp_json_object_put(root);
    return bindings;
}

static bool oasp_state_exec(sqlite3 *database, const char *sql) { return sqlite3_exec(database, sql, NULL, NULL, NULL) == SQLITE_OK; }

static bool oasp_state_finish_transaction(sqlite3 *database, bool result) {
    if (result && oasp_state_exec(database, "COMMIT"))
        return true;
    (void)oasp_state_exec(database, "ROLLBACK");
    return false;
}

oasp_state_store *oasp_state_store_open(const char *directory, const char *root) {
    if (!directory || !*directory || !root || !*root)
        return NULL;
    if (mkdir(directory, 0700) && errno != EEXIST)
        return NULL;
    uint64_t hash = oasp_state_hash(root);
    char database_path[4096];
    char lock_path[4096];
    int database_length = snprintf(database_path, sizeof(database_path), "%s/state-%016llx.sqlite3", directory, (unsigned long long)hash);
    int lock_length = snprintf(lock_path, sizeof(lock_path), "%s/state-%016llx.lock", directory, (unsigned long long)hash);
    if (database_length <= 0 || (size_t)database_length >= sizeof(database_path) || lock_length <= 0 || (size_t)lock_length >= sizeof(lock_path))
        return NULL;
    oasp_state_store *store = calloc(1, sizeof(*store));
    if (!store)
        return NULL;
    store->lock_fd = -1;
    if (sqlite3_open_v2(database_path, &store->database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK)
        goto failed;
    sqlite3_busy_timeout(store->database, 5000);
    store->lock_fd = open(lock_path, O_RDWR | O_CREAT, 0600);
    if (store->lock_fd < 0 || !oasp_state_lock_region(store, 0, F_WRLCK))
        goto failed;
    bool initialized =
        oasp_state_exec(store->database, "PRAGMA journal_mode=WAL") &&
        oasp_state_exec(store->database, "CREATE TABLE IF NOT EXISTS sessions("
                                         "id TEXT PRIMARY KEY,payload TEXT NOT NULL,"
                                         "timeout INTEGER NOT NULL,touched INTEGER NOT NULL)") &&
        oasp_state_exec(store->database, "CREATE TABLE IF NOT EXISTS application("
                                         "id INTEGER PRIMARY KEY CHECK(id=1),payload TEXT NOT NULL)") &&
        oasp_state_exec(store->database, "CREATE TABLE IF NOT EXISTS state_metadata("
                                         "key TEXT PRIMARY KEY,value INTEGER NOT NULL)") &&
        oasp_state_exec(store->database, "CREATE TABLE IF NOT EXISTS application_v2("
                                         "name BLOB PRIMARY KEY,value BLOB NOT NULL,constant INTEGER NOT NULL)") &&
        oasp_state_exec(store->database, "DELETE FROM application WHERE EXISTS("
                                         "SELECT 1 FROM state_metadata WHERE key='application_format' AND value=2)");
    bool unlocked = oasp_state_lock_region(store, 0, F_UNLCK);
    if (!initialized || !unlocked)
        goto failed;
    return store;
failed:
    oasp_state_store_close(store);
    return NULL;
}

void oasp_state_store_close(oasp_state_store *store) {
    if (!store)
        return;
    if (store->database)
        sqlite3_close(store->database);
    if (store->lock_fd >= 0)
        close(store->lock_fd);
    free(store);
}

static bool oasp_state_decode_store(oasp_runtime *runtime, const char *payload, oasp_binding **bindings) {
    if (!payload) {
        *bindings = NULL;
        return true;
    }
    struct json_object *encoded = oasp_state_parse_store(payload);
    if (!encoded)
        return false;
    bool result = oasp_state_decode_bindings(runtime, encoded, bindings, 0);
    oasp_json_object_put(encoded);
    return result;
}

bool oasp_state_store_load_session(oasp_state_store *store, const char *id, oasp_runtime *runtime, oasp_binding **bindings, int *timeout,
                                   time_t now) {
    static const char sql[] = "SELECT payload,timeout,touched FROM sessions WHERE id=?1";
    sqlite3_stmt *statement = NULL;
    if (!store || !id || !runtime || !bindings || !timeout || sqlite3_prepare_v2(store->database, sql, -1, &statement, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(statement, 1, id, -1, SQLITE_STATIC);
    bool result = true;
    bool expired = false;
    int step = sqlite3_step(statement);
    if (step == SQLITE_ROW) {
        int stored_timeout = sqlite3_column_int(statement, 1);
        time_t touched = (time_t)sqlite3_column_int64(statement, 2);
        if (stored_timeout > 0 && now - touched > (time_t)stored_timeout * 60) {
            *bindings = NULL;
            *timeout = 20;
            expired = true;
        } else {
            const void *payload = sqlite3_column_blob(statement, 0);
            int payload_length = sqlite3_column_bytes(statement, 0);
            if (payload_length >= 8 && payload && !memcmp(payload, "OASPS\0\0\1", 8))
                result = oasp_state_binary_decode_bindings(runtime, payload, (size_t)payload_length, bindings);
            else
                result = oasp_state_decode_store(runtime, (const char *)sqlite3_column_text(statement, 0), bindings);
            *timeout = stored_timeout > 0 ? stored_timeout : 20;
        }
    } else if (step == SQLITE_DONE) {
        *bindings = NULL;
        *timeout = 20;
    } else {
        result = false;
    }
    sqlite3_finalize(statement);
    if (result && expired)
        result = oasp_state_store_delete_session(store, id);
    return result;
}

bool oasp_state_store_save_session(oasp_state_store *store, const char *id, const oasp_binding *bindings, int timeout, time_t touched) {
    static const char sql[] = "INSERT INTO sessions(id,payload,timeout,touched) VALUES(?1,?2,?3,?4)"
                              " ON CONFLICT(id) DO UPDATE SET payload=excluded.payload,"
                              "timeout=excluded.timeout,touched=excluded.touched";
    char *payload = NULL;
    size_t payload_length = 0;
    sqlite3_stmt *statement = NULL;
    if (!store || !id || !oasp_state_binary_encode_bindings(bindings, &payload, &payload_length) ||
        sqlite3_prepare_v2(store->database, sql, -1, &statement, NULL) != SQLITE_OK) {
        free(payload);
        return false;
    }
    sqlite3_bind_text(statement, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_blob64(statement, 2, payload, (sqlite3_uint64)payload_length, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 3, timeout);
    sqlite3_bind_int64(statement, 4, (sqlite3_int64)touched);
    bool result = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    free(payload);
    return result;
}

bool oasp_state_store_delete_session(oasp_state_store *store, const char *id) {
    sqlite3_stmt *statement = NULL;
    if (!store || !id || sqlite3_prepare_v2(store->database, "DELETE FROM sessions WHERE id=?1", -1, &statement, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(statement, 1, id, -1, SQLITE_STATIC);
    bool result = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return result;
}

static char *oasp_state_application_payload(oasp_state_store *store) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(store->database, "SELECT payload FROM application WHERE id=1", -1, &statement, NULL) != SQLITE_OK)
        return NULL;
    char *payload = NULL;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const char *text = (const char *)sqlite3_column_text(statement, 0);
        if (text)
            payload = strdup(text);
    }
    sqlite3_finalize(statement);
    return payload;
}

static bool oasp_state_application_is_v2(oasp_state_store *store) {
    sqlite3_stmt *statement = NULL;
    bool result = sqlite3_prepare_v2(store->database, "SELECT value FROM state_metadata WHERE key='application_format'", -1, &statement, NULL) == SQLITE_OK &&
                  sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 2;
    sqlite3_finalize(statement);
    return result;
}

static bool oasp_state_application_write_binding(oasp_state_store *store, const oasp_binding *binding) {
    static const char sql[] = "INSERT INTO application_v2(name,value,constant) VALUES(?1,?2,?3)"
                              " ON CONFLICT(name) DO UPDATE SET value=excluded.value,constant=excluded.constant";
    char *encoded = NULL;
    size_t encoded_length = 0;
    sqlite3_stmt *statement = NULL;
    bool encoded_ok = binding && binding->name.length <= UINT32_MAX &&
                      oasp_state_binary_encode_value(&binding->value, &encoded, &encoded_length);
    bool prepared = encoded_ok && sqlite3_prepare_v2(store->database, sql, -1, &statement, NULL) == SQLITE_OK;
    if (!prepared) {
        if (getenv("OPENASP_TRACE_STATE"))
            fprintf(stderr,
                    "OpenASP state trace: application encode/write setup failed key=%.*s kind=%d encoded=%d sqlite=%s\n",
                    binding ? (int)binding->name.length : 0, binding ? binding->name.data : "",
                    binding ? (int)binding->value.kind : -1, encoded_ok ? 1 : 0,
                    store && store->database ? sqlite3_errmsg(store->database) : "state store unavailable");
        free(encoded);
        return false;
    }
    sqlite3_bind_blob64(statement, 1, binding->name.data, (sqlite3_uint64)binding->name.length, SQLITE_STATIC);
    sqlite3_bind_blob64(statement, 2, encoded, (sqlite3_uint64)encoded_length, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 3, binding->constant ? 1 : 0);
    bool result = sqlite3_step(statement) == SQLITE_DONE;
    if (!result && getenv("OPENASP_TRACE_STATE"))
        fprintf(stderr, "OpenASP state trace: application write failed key=%.*s kind=%d sqlite=%s\n",
                (int)binding->name.length, binding->name.data, (int)binding->value.kind,
                sqlite3_errmsg(store->database));
    sqlite3_finalize(statement);
    free(encoded);
    return result;
}

static bool oasp_state_application_delete_binding(oasp_state_store *store, oasp_slice name) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(store->database, "DELETE FROM application_v2 WHERE name=?1", -1, &statement, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_blob64(statement, 1, name.data, (sqlite3_uint64)name.length, SQLITE_STATIC);
    bool result = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return result;
}

static bool oasp_state_application_mark_v2(oasp_state_store *store) {
    return oasp_state_exec(store->database, "INSERT INTO state_metadata(key,value) VALUES('application_format',2)"
                                           " ON CONFLICT(key) DO UPDATE SET value=excluded.value");
}

static bool oasp_state_application_load_v2(oasp_state_store *store, oasp_runtime *runtime, oasp_binding **bindings) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(store->database, "SELECT name,value,constant FROM application_v2 ORDER BY rowid", -1, &statement, NULL) != SQLITE_OK)
        return false;
    bool result = true;
    oasp_binding *first = NULL;
    oasp_binding **tail = &first;
    int step;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
        const void *name = sqlite3_column_blob(statement, 0);
        int name_length = sqlite3_column_bytes(statement, 0);
        const void *encoded = sqlite3_column_blob(statement, 1);
        int encoded_length = sqlite3_column_bytes(statement, 1);
        int constant = sqlite3_column_int(statement, 2);
        oasp_binding *binding = oasp_arena_alloc(runtime->arena, sizeof(*binding), _Alignof(oasp_binding));
        if (name_length < 0 || encoded_length < 0 || (name_length && !name) || !encoded || (constant != 0 && constant != 1) || !binding) {
            result = false;
            break;
        }
        memset(binding, 0, sizeof(*binding));
        binding->name.data = oasp_arena_copy(runtime->arena, name, (size_t)name_length);
        binding->name.length = (size_t)name_length;
        binding->constant = constant != 0;
        bool decoded = binding->name.data &&
                       oasp_state_binary_decode_value(runtime, encoded, (size_t)encoded_length, &binding->value);
        if (!decoded) {
            if (getenv("OPENASP_TRACE_STATE"))
                fprintf(stderr, "OpenASP state trace: application decode failed key=%.*s value_length=%d\n",
                        name_length, name ? (const char *)name : "", encoded_length);
            result = false;
            break;
        }
        *tail = binding;
        tail = &binding->next;
    }
    if (step != SQLITE_DONE)
        result = false;
    sqlite3_finalize(statement);
    if (result)
        *bindings = first;
    return result;
}

static bool oasp_state_application_migrate(oasp_state_store *store, oasp_runtime *runtime, oasp_binding **bindings) {
    bool result = oasp_state_exec(store->database, "BEGIN IMMEDIATE");
    if (!result)
        return false;
    if (oasp_state_application_is_v2(store)) {
        result = oasp_state_application_load_v2(store, runtime, bindings);
    } else {
        char *payload = oasp_state_application_payload(store);
        result = oasp_state_decode_store(runtime, payload, bindings);
        free(payload);
        if (result)
            result = oasp_state_exec(store->database, "DELETE FROM application_v2");
        for (const oasp_binding *binding = result ? *bindings : NULL; binding && result; binding = binding->next)
            result = oasp_state_application_write_binding(store, binding);
        if (result)
            result = oasp_state_application_mark_v2(store);
        if (result)
            result = oasp_state_exec(store->database, "DELETE FROM application");
    }
    return oasp_state_finish_transaction(store->database, result);
}

bool oasp_state_store_load_application(oasp_state_store *store, oasp_runtime *runtime, oasp_binding **bindings) {
    if (!store || !runtime || !bindings)
        return false;
    return oasp_state_application_is_v2(store) ? oasp_state_application_load_v2(store, runtime, bindings)
                                               : oasp_state_application_migrate(store, runtime, bindings);
}

static bool oasp_state_values_equal(const oasp_value *left, const oasp_value *right, size_t depth);

static bool oasp_state_slices_equal(oasp_slice left, oasp_slice right) {
    return left.length == right.length && (!left.length || !memcmp(left.data, right.data, left.length));
}

static bool oasp_state_names_equal(oasp_slice left, oasp_slice right) {
    if (left.length != right.length)
        return false;
    for (size_t i = 0; i < left.length; i++) {
        unsigned char a = (unsigned char)left.data[i];
        unsigned char b = (unsigned char)right.data[i];
        if (a >= 'A' && a <= 'Z')
            a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = (unsigned char)(b + ('a' - 'A'));
        if (a != b)
            return false;
    }
    return true;
}

static const oasp_binding *oasp_state_find_binding(const oasp_binding *bindings, oasp_slice name) {
    for (const oasp_binding *binding = bindings; binding; binding = binding->next)
        if (oasp_state_names_equal(binding->name, name))
            return binding;
    return NULL;
}

static bool oasp_state_bindings_equal(const oasp_binding *left, const oasp_binding *right, size_t depth) {
    if (depth > OASP_STATE_MAX_DEPTH)
        return false;
    while (left && right) {
        if (left->constant != right->constant || !oasp_state_slices_equal(left->name, right->name) ||
            !oasp_state_values_equal(&left->value, &right->value, depth + 1))
            return false;
        left = left->next;
        right = right->next;
    }
    return !left && !right;
}

static bool oasp_state_objects_equal(const oasp_object *left, const oasp_object *right, size_t depth) {
    if (left == right)
        return true;
    if (!left || !right || depth > OASP_STATE_MAX_DEPTH || left->component_kind != right->component_kind)
        return false;
    int left_mode = 0;
    int right_mode = 0;
    size_t left_count = 0;
    size_t right_count = 0;
    bool left_dictionary = oasp_component_dictionary_info(left, &left_mode, &left_count);
    bool right_dictionary = oasp_component_dictionary_info(right, &right_mode, &right_count);
    if (left_dictionary || right_dictionary) {
        if (!left_dictionary || !right_dictionary || left_mode != right_mode || left_count != right_count)
            return false;
        for (size_t i = 0; i < left_count; i++) {
            const oasp_value *left_key = NULL;
            const oasp_value *left_value = NULL;
            const oasp_value *right_key = NULL;
            const oasp_value *right_value = NULL;
            if (!oasp_component_dictionary_item(left, i, &left_key, &left_value) ||
                !oasp_component_dictionary_item(right, i, &right_key, &right_value) ||
                !oasp_state_values_equal(left_key, right_key, depth + 1) ||
                !oasp_state_values_equal(left_value, right_value, depth + 1))
                return false;
        }
        return true;
    }
    if (!left->class_def || !right->class_def || !oasp_state_slices_equal(left->class_def->name, right->class_def->name))
        return false;
    return oasp_state_bindings_equal(left->fields, right->fields, depth + 1);
}

static bool oasp_state_values_equal(const oasp_value *left, const oasp_value *right, size_t depth) {
    if (left == right)
        return true;
    if (!left || !right || depth > OASP_STATE_MAX_DEPTH || left->kind != right->kind)
        return false;
    switch (left->kind) {
    case OASP_VALUE_EMPTY:
    case OASP_VALUE_NULL:
    case OASP_VALUE_NOTHING:
        return true;
    case OASP_VALUE_BOOL:
        return left->as.boolean == right->as.boolean;
    case OASP_VALUE_INT:
        return left->as.integer == right->as.integer;
    case OASP_VALUE_FLOAT:
        return left->as.floating == right->as.floating;
    case OASP_VALUE_STRING:
        return oasp_state_slices_equal(left->as.string, right->as.string);
    case OASP_VALUE_DATE:
        return oasp_state_slices_equal(left->as.date, right->as.date);
    case OASP_VALUE_OBJECT:
        return oasp_state_objects_equal(left->as.object, right->as.object, depth + 1);
    case OASP_VALUE_ARRAY:
        if (left->as.array == right->as.array)
            return true;
        if (!left->as.array || !right->as.array || left->as.array->rank != right->as.array->rank ||
            left->as.array->length != right->as.array->length)
            return false;
        for (size_t i = 0; i < left->as.array->rank; i++)
            if (left->as.array->bounds[i] != right->as.array->bounds[i])
                return false;
        for (size_t i = 0; i < left->as.array->length; i++)
            if (!oasp_state_values_equal(&left->as.array->items[i], &right->as.array->items[i], depth + 1))
                return false;
        return true;
    }
    return false;
}

bool oasp_state_store_merge_application(oasp_state_store *store, const oasp_binding *before, const oasp_binding *after) {
    if (!store)
        return false;
    if (oasp_state_bindings_equal(before, after, 0))
        return true;
    bool result = oasp_state_exec(store->database, "BEGIN IMMEDIATE");
    for (const oasp_binding *old = result ? before : NULL; old && result; old = old->next) {
        const oasp_binding *current = oasp_state_find_binding(after, old->name);
        if (!current)
            result = oasp_state_application_delete_binding(store, old->name);
        else if (old->constant != current->constant || !oasp_state_values_equal(&old->value, &current->value, 0))
            result = oasp_state_application_write_binding(store, current);
    }
    for (const oasp_binding *current = result ? after : NULL; current && result; current = current->next)
        if (!oasp_state_find_binding(before, current->name))
            result = oasp_state_application_write_binding(store, current);
    return oasp_state_finish_transaction(store->database, result);
}
