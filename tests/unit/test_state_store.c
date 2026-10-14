// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`tests/test_state_store.c`): Native regression test. Local fixtures
 * construct the smallest state needed to exercise ownership, limits, persistence, and failure
 * paths; assertions document the externally observable contract and guard against silent
 * regressions.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#define _XOPEN_SOURCE 700

#include "component.h"
#include "openasp.h"
#include "runtime.h"
#include "state_binary.h"
#include "state_store.h"

#include <assert.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint64_t state_hash(const char *text) {
    uint64_t hash = UINT64_C(1469598103934665603);
    while (*text) {
        hash ^= (unsigned char)*text++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static oasp_binding *find_binding(oasp_binding *bindings, const char *name) {
    size_t length = strlen(name);
    for (oasp_binding *binding = bindings; binding; binding = binding->next)
        if (binding->name.length == length && !memcmp(binding->name.data, name, length))
            return binding;
    return NULL;
}

static void test_binary_codec(void) {
    oasp_request *request = oasp_request_create(".", "", "", "GET");
    assert(request);
    oasp_runtime runtime;
    oasp_runtime_init(&runtime, request);

    oasp_value keys[2] = {oasp_value_string("Alpha", 5), oasp_value_int(42)};
    oasp_value dictionary_values[2] = {oasp_value_string("one", 3), oasp_value_int(2)};
    oasp_object *dictionary = oasp_component_dictionary_restore(&runtime, 1, keys, dictionary_values, 2);
    assert(dictionary);

    int64_t bounds[1] = {2};
    oasp_value array_items[3] = {oasp_value_int(7), oasp_value_string("array", 5), oasp_value_null()};
    oasp_array array = {.rank = 1, .length = 3, .bounds = bounds, .items = array_items};
    oasp_class_def definition = {.name = {"Probe", 5}};
    oasp_binding field = {.name = {"value", 5}, .value = oasp_value_string("class", 5)};
    oasp_object class_object = {.class_def = &definition, .fields = &field};

    oasp_binding class_binding = {.name = {"class", 5}, .value = {.kind = OASP_VALUE_OBJECT, .as.object = &class_object}};
    oasp_binding dictionary_binding = {.next = &class_binding,
                                       .name = {"dictionary", 10},
                                       .value = {.kind = OASP_VALUE_OBJECT, .as.object = dictionary}};
    oasp_binding array_binding = {.next = &dictionary_binding,
                                  .name = {"array", 5},
                                  .value = {.kind = OASP_VALUE_ARRAY, .as.array = &array}};
    oasp_binding scalar_binding = {.next = &array_binding, .name = {"scalar", 6}, .value = oasp_value_int(-123)};

    char *encoded = NULL;
    size_t encoded_length = 0;
    assert(oasp_state_binary_encode_bindings(&scalar_binding, &encoded, &encoded_length));
    assert(encoded_length > 8 && !memcmp(encoded, "OASPS\0\0\1", 8));

    oasp_request *decoded_request = oasp_request_create(".", "", "", "GET");
    assert(decoded_request);
    oasp_runtime decoded_runtime;
    oasp_runtime_init(&decoded_runtime, decoded_request);
    oasp_binding *decoded = NULL;
    assert(oasp_state_binary_decode_bindings(&decoded_runtime, encoded, encoded_length, &decoded));
    assert(find_binding(decoded, "scalar")->value.as.integer == -123);
    assert(find_binding(decoded, "array")->value.as.array->length == 3);
    int mode = 0;
    size_t count = 0;
    assert(oasp_component_dictionary_info(find_binding(decoded, "dictionary")->value.as.object, &mode, &count));
    assert(mode == 1 && count == 2);
    assert(find_binding(decoded, "class")->value.as.object->class_def->name.length == 5);
    encoded[encoded_length - 1] ^= 1;
    encoded_length--;
    assert(!oasp_state_binary_decode_bindings(&decoded_runtime, encoded, encoded_length, &decoded));
    free(encoded);
    oasp_request_destroy(decoded_request);
    oasp_request_destroy(request);
}

static void test_xml_application_value(void) {
    oasp_request *request = oasp_request_create(".", "", "", "GET");
    assert(request);
    oasp_runtime runtime;
    oasp_runtime_init(&runtime, request);
    oasp_value xml = oasp_component_create(&runtime, (oasp_slice){"MSXML2.DOMDocument", 18}, 0);
    assert(xml.kind == OASP_VALUE_OBJECT);
    static const char xml_source[] = "<labels><label id=\"7\">cached</label></labels>";
    oasp_value source = oasp_value_string(xml_source, sizeof(xml_source) - 1);
    oasp_value ignored = oasp_value_empty();
    assert(oasp_component_call(&runtime, xml.as.object, (oasp_slice){"loadxml", 7}, &source, 1, 0, &ignored));

    oasp_binding binding = {.name = {"xml_cache", 9}, .value = xml};
    char *encoded = NULL;
    size_t encoded_length = 0;
    assert(oasp_state_binary_encode_bindings(&binding, &encoded, &encoded_length));

    oasp_request *decoded_request = oasp_request_create(".", "", "", "GET");
    assert(decoded_request);
    oasp_runtime decoded_runtime;
    oasp_runtime_init(&decoded_runtime, decoded_request);
    oasp_binding *decoded = NULL;
    assert(oasp_state_binary_decode_bindings(&decoded_runtime, encoded, encoded_length, &decoded));
    oasp_binding *cached = find_binding(decoded, "xml_cache");
    assert(cached && cached->value.kind == OASP_VALUE_OBJECT);
    oasp_value serialized = oasp_value_empty();
    assert(oasp_component_get(&decoded_runtime, cached->value.as.object, (oasp_slice){"xml", 3}, 0, &serialized));
    assert(serialized.kind == OASP_VALUE_STRING && strstr(serialized.as.string.data, "cached"));

    free(encoded);
    oasp_request_destroy(decoded_request);
    oasp_request_destroy(request);
}

static void test_legacy_migration_and_merge(void) {
    char directory[] = "/tmp/openasp-state-test-XXXXXX";
    int temporary = mkstemp(directory);
    assert(temporary >= 0);
    close(temporary);
    assert(unlink(directory) == 0);
    assert(mkdir(directory, 0700) == 0);
    const char *root = "/openasp-state-test-root";
    char database_path[1024];
    snprintf(database_path, sizeof(database_path), "%s/state-%016llx.sqlite3", directory, (unsigned long long)state_hash(root));

    oasp_state_store *store = oasp_state_store_open(directory, root);
    assert(store);
    oasp_state_store_close(store);

    sqlite3 *database = NULL;
    assert(sqlite3_open(database_path, &database) == SQLITE_OK);
    const char *application =
        "{\"abi\":1,\"bindings\":{\"legacy\":{\"constant\":false,\"value\":{\"type\":5,\"data\":\"migrated\"}}}}";
    const char *session =
        "{\"abi\":1,\"bindings\":{\"token\":{\"constant\":false,\"value\":{\"type\":5,\"data\":\"legacy-session\"}}}}";
    sqlite3_stmt *statement = NULL;
    assert(sqlite3_prepare_v2(database, "INSERT INTO application(id,payload) VALUES(1,?1)", -1, &statement, NULL) == SQLITE_OK);
    sqlite3_bind_text(statement, 1, application, -1, SQLITE_STATIC);
    assert(sqlite3_step(statement) == SQLITE_DONE);
    sqlite3_finalize(statement);
    assert(sqlite3_prepare_v2(database, "INSERT INTO sessions(id,payload,timeout,touched) VALUES('session',?1,30,100)", -1, &statement, NULL) == SQLITE_OK);
    sqlite3_bind_text(statement, 1, session, -1, SQLITE_STATIC);
    assert(sqlite3_step(statement) == SQLITE_DONE);
    sqlite3_finalize(statement);
    assert(sqlite3_prepare_v2(database, "INSERT INTO sessions(id,payload,timeout,touched) VALUES('expired',?1,1,1)", -1, &statement, NULL) == SQLITE_OK);
    sqlite3_bind_text(statement, 1, session, -1, SQLITE_STATIC);
    assert(sqlite3_step(statement) == SQLITE_DONE);
    sqlite3_finalize(statement);
    sqlite3_close(database);

    store = oasp_state_store_open(directory, root);
    assert(store);
    oasp_request *request = oasp_request_create(".", "", "", "GET");
    assert(request);
    oasp_runtime runtime;
    oasp_runtime_init(&runtime, request);
    oasp_binding *application_bindings = NULL;
    assert(oasp_state_store_load_application(store, &runtime, &application_bindings));
    assert(find_binding(application_bindings, "legacy"));
    assert(find_binding(application_bindings, "legacy")->value.as.string.length == 8);

    oasp_binding first = {.next = application_bindings, .name = {"first", 5}, .value = oasp_value_int(1)};
    oasp_binding second = {.next = application_bindings, .name = {"second", 6}, .value = oasp_value_int(2)};
    assert(oasp_state_store_merge_application(store, application_bindings, &first));
    assert(oasp_state_store_merge_application(store, application_bindings, &second));

    oasp_binding *session_bindings = NULL;
    int timeout = 0;
    assert(oasp_state_store_load_session(store, "session", &runtime, &session_bindings, &timeout, 100));
    assert(timeout == 30 && find_binding(session_bindings, "token"));
    assert(oasp_state_store_save_session(store, "session", session_bindings, timeout, 101));
    session_bindings = (oasp_binding *)(uintptr_t)1;
    timeout = 0;
    assert(oasp_state_store_load_session(store, "expired", &runtime, &session_bindings, &timeout, 1000));
    assert(session_bindings == NULL && timeout == 20);
    oasp_state_store_close(store);
    oasp_request_destroy(request);

    assert(sqlite3_open(database_path, &database) == SQLITE_OK);
    assert(sqlite3_prepare_v2(database, "SELECT value FROM state_metadata WHERE key='application_format'", -1, &statement, NULL) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 2);
    sqlite3_finalize(statement);
    assert(sqlite3_prepare_v2(database, "SELECT count(*) FROM application", -1, &statement, NULL) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 0);
    sqlite3_finalize(statement);
    assert(sqlite3_prepare_v2(database, "SELECT count(*),min(typeof(value)) FROM application_v2", -1, &statement, NULL) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 3);
    assert(!strcmp((const char *)sqlite3_column_text(statement, 1), "blob"));
    sqlite3_finalize(statement);
    assert(sqlite3_prepare_v2(database, "SELECT typeof(payload),hex(substr(payload,1,8)) FROM sessions WHERE id='session'", -1, &statement, NULL) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    assert(!strcmp((const char *)sqlite3_column_text(statement, 0), "blob"));
    assert(!strcmp((const char *)sqlite3_column_text(statement, 1), "4F41535053000001"));
    sqlite3_finalize(statement);
    assert(sqlite3_prepare_v2(database, "SELECT count(*) FROM sessions WHERE id='expired'", -1, &statement, NULL) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 0);
    sqlite3_finalize(statement);
    sqlite3_close(database);

    char path[1100];
    unlink(database_path);
    snprintf(path, sizeof(path), "%s-wal", database_path);
    unlink(path);
    snprintf(path, sizeof(path), "%s-shm", database_path);
    unlink(path);
    snprintf(path, sizeof(path), "%s/state-%016llx.lock", directory, (unsigned long long)state_hash(root));
    unlink(path);
    assert(rmdir(directory) == 0);
}

int main(void) {
    test_binary_codec();
    test_xml_application_value();
    test_legacy_migration_and_merge();
    return 0;
}
