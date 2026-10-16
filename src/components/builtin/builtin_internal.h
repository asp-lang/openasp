// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_BUILTIN_INTERNAL_H
#define OPENASP_BUILTIN_INTERNAL_H

#include "component.h"
#include "component_internal.h"
#include "component_object.h"

#include <netdb.h>

typedef struct oasp_dictionary_entry {
    struct oasp_dictionary_entry *next;
    oasp_value key;
    oasp_value value;
} oasp_dictionary_entry;

typedef struct {
    oasp_dictionary_entry *first;
    oasp_dictionary_entry *last;
    size_t count;
    int compare_mode;
} oasp_dictionary;

typedef struct {
    oasp_slice pattern;
    bool global;
    bool ignore_case;
    bool multiline;
} oasp_regexp;

typedef struct {
    oasp_value *items;
    size_t count;
} oasp_component_collection;

typedef struct {
    oasp_slice value;
    size_t first_index;
    oasp_component_collection submatches;
} oasp_regexp_match;

typedef struct {
    const char *data;
    size_t length;
} oasp_builtin_kv_argument;

bool oasp_builtin_json_parse(oasp_runtime *runtime, const oasp_value *input, oasp_value *result);
bool oasp_builtin_json_call(oasp_runtime *runtime, oasp_slice member, oasp_value *arguments, size_t argument_count, size_t line,
                            oasp_value *result);
bool oasp_builtin_crypto_call(oasp_runtime *runtime, oasp_slice member, oasp_value *arguments, size_t argument_count, size_t line,
                              oasp_value *result);
void oasp_builtin_http_finalize(void *context);
oasp_object *oasp_builtin_http_create(oasp_runtime *runtime, const char *name);
bool oasp_builtin_http_prepare(oasp_runtime *runtime, oasp_http *http, long default_timeout_ms);
void oasp_builtin_http_complete(oasp_runtime *runtime, oasp_http *http, CURLcode status);
void oasp_builtin_http_easy_cleanup(oasp_http *http);
bool oasp_builtin_http_perform(oasp_runtime *runtime, oasp_object *object, oasp_http *http, const char *body, size_t line);
oasp_value oasp_builtin_http_response_header(oasp_runtime *runtime, const oasp_http *http, const char *name);
bool oasp_builtin_http_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line);
bool oasp_builtin_http_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                            size_t line, oasp_value *result);
bool oasp_builtin_http_multi_get(oasp_object *object, oasp_slice member, oasp_value *result);
bool oasp_builtin_http_multi_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line);
bool oasp_builtin_http_multi_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                                  size_t line, oasp_value *result);
char *oasp_builtin_openai_payload(oasp_runtime *runtime, oasp_openai *openai, const oasp_value *input, bool responses);
bool oasp_builtin_openai_request(oasp_runtime *runtime, oasp_object *object, oasp_openai *openai, const char *endpoint, const char *payload,
                                 size_t line, oasp_value *result);
bool oasp_builtin_feature_enabled(const char *name);
bool oasp_builtin_shell_execute(const char *executable, const char *arguments, const char *directory, int *launch_error);
void oasp_builtin_process_finalize(void *context);
bool oasp_builtin_process_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                               size_t line, oasp_value *result);
bool oasp_builtin_process_get(oasp_object *object, oasp_slice member, oasp_value *result);
bool oasp_builtin_process_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line);
bool oasp_builtin_shell_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                             size_t line, oasp_value *result);
void oasp_builtin_socket_finalize(void *context);
oasp_slice oasp_builtin_socket_protocol(oasp_runtime *runtime, const char *value);
void oasp_builtin_socket_timeout_apply(const oasp_socket *socket, int fd);
bool oasp_builtin_socket_attach(oasp_socket *socket, int fd, int state);
bool oasp_builtin_socket_network_address(const char *host, const char *port, int type, bool passive, struct addrinfo **addresses);
bool oasp_builtin_socket_connect_native(oasp_socket *socket);
bool oasp_builtin_socket_listen_native(oasp_runtime *runtime, oasp_socket *socket, int backlog);
bool oasp_builtin_socket_bind_udp(oasp_runtime *runtime, oasp_socket *socket);
void oasp_builtin_socket_raise(oasp_runtime *runtime, oasp_object *object, const char *operation, size_t line);
bool oasp_builtin_socket_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *result);
bool oasp_builtin_socket_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line);
bool oasp_builtin_socket_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                              size_t line, oasp_value *result);
void oasp_builtin_websocket_finalize(void *context);
bool oasp_builtin_websocket_get(oasp_object *object, oasp_slice member, oasp_value *result);
bool oasp_builtin_websocket_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line);
bool oasp_builtin_websocket_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                                 size_t line, oasp_value *result);
void oasp_builtin_redis_finalize(void *context);
bool oasp_builtin_redis_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                             size_t line, oasp_value *result);
bool oasp_builtin_redis_get_property(oasp_object *object, oasp_slice member, oasp_value *result);
bool oasp_builtin_redis_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line);
bool oasp_builtin_redis_command_equal(const oasp_builtin_kv_argument *argument, const char *name);
void *oasp_builtin_redis_multi_create(oasp_runtime *runtime);
void oasp_builtin_redis_multi_gc_trace(oasp_allocator *allocator, const oasp_object *object);
bool oasp_builtin_redis_multi_get(oasp_object *object, oasp_slice member, oasp_value *result);
bool oasp_builtin_redis_multi_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line);
bool oasp_builtin_redis_multi_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                                   size_t line, oasp_value *result);
bool oasp_builtin_kv_write_all(oasp_kv_pool_entry *connection, const void *data, size_t length);
bool oasp_builtin_kv_read_fill(oasp_kv_pool_entry *connection);
bool oasp_builtin_kv_read_exact(oasp_kv_pool_entry *connection, void *data, size_t length);
bool oasp_builtin_kv_read_line(oasp_kv_pool_entry *connection, oasp_slice *line);
size_t oasp_builtin_kv_argument_count(const oasp_value *arguments, size_t count);
bool oasp_builtin_kv_collect_arguments(oasp_runtime *runtime, const oasp_value *arguments, size_t count, oasp_builtin_kv_argument *output, size_t capacity,
                                       size_t *index, size_t *encoded_length);
void oasp_builtin_memcache_finalize(void *context);
bool oasp_builtin_memcache_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                                size_t line, oasp_value *result);
bool oasp_builtin_memcache_get_property(oasp_object *object, oasp_slice member, oasp_value *result);
bool oasp_builtin_memcache_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line);
bool oasp_builtin_memcache_key_valid(const char *key, size_t length);
void *oasp_builtin_memcache_multi_create(oasp_runtime *runtime);
void oasp_builtin_memcache_multi_gc_trace(oasp_allocator *allocator, const oasp_object *object);
bool oasp_builtin_memcache_multi_get(oasp_object *object, oasp_slice member, oasp_value *result);
bool oasp_builtin_memcache_multi_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line);
bool oasp_builtin_memcache_multi_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                                      size_t line, oasp_value *result);

oasp_dictionary *oasp_builtin_dictionary_create(oasp_runtime *runtime, int compare_mode);
bool oasp_builtin_dictionary_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *result);
bool oasp_builtin_dictionary_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line);
bool oasp_builtin_dictionary_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                                  size_t line, oasp_value *result);
bool oasp_builtin_dictionary_assign(oasp_runtime *runtime, oasp_object *object, const oasp_value *key, const oasp_value *value, size_t line);
bool oasp_builtin_dictionary_enumerate(oasp_runtime *runtime, oasp_object *object, oasp_value *result);
bool oasp_builtin_dictionary_persist_copy(oasp_runtime *runtime, const oasp_object *source, oasp_object **result);
bool oasp_builtin_dictionary_restore(oasp_runtime *runtime, int compare_mode, const oasp_value *keys, const oasp_value *values, size_t count,
                                     oasp_object **result);
void oasp_builtin_dictionary_gc_trace(oasp_allocator *allocator, const oasp_dictionary *dictionary);

oasp_regexp *oasp_builtin_regexp_create(oasp_runtime *runtime);
bool oasp_builtin_regexp_get(oasp_object *object, oasp_slice member, oasp_value *result);
bool oasp_builtin_regexp_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value);
bool oasp_builtin_regexp_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                              size_t line, oasp_value *result);
bool oasp_builtin_regexp_match_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *result);
bool oasp_builtin_regexp_match_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                                    size_t line, oasp_value *result);
void oasp_builtin_regexp_gc_trace(oasp_allocator *allocator, const oasp_object *object);

void *oasp_builtin_stream_create(oasp_runtime *runtime);
void oasp_builtin_stream_finalize(void *context);
bool oasp_builtin_stream_get(oasp_object *object, oasp_slice member, oasp_value *result);
bool oasp_builtin_stream_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line);
bool oasp_builtin_stream_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                              size_t line, oasp_value *result);

char *oasp_builtin_host_path(oasp_runtime *runtime, const char *path);
char *oasp_builtin_case_path(oasp_runtime *runtime, const char *path);
bool oasp_builtin_fso_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, size_t line, oasp_value *result);
bool oasp_builtin_fso_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                           size_t line, oasp_value *result);

void *oasp_builtin_xml_create(oasp_runtime *runtime);
bool oasp_builtin_xml_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, size_t line, oasp_value *result);
bool oasp_builtin_xml_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line);
bool oasp_builtin_xml_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                           size_t line, oasp_value *result);

void *oasp_builtin_ado_create(oasp_runtime *runtime, oasp_component_kind kind);
bool oasp_builtin_ado_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, size_t line, oasp_value *result);
bool oasp_builtin_ado_get_id(oasp_runtime *runtime, oasp_object *object, uint16_t member_id, oasp_slice member, size_t line,
                             oasp_value *result);
bool oasp_builtin_ado_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line);
bool oasp_builtin_ado_call(oasp_runtime *runtime, oasp_object *object, uint16_t member_id, oasp_slice member, oasp_value *arguments,
                           size_t argument_count, size_t line, oasp_value *result);
bool oasp_builtin_ado_default(oasp_runtime *runtime, oasp_object *object, oasp_value *arguments, size_t argument_count, size_t line,
                              oasp_value *result);
bool oasp_builtin_ado_assign(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                             const oasp_value *value, size_t line);
bool oasp_builtin_ado_recordset_field_view(oasp_runtime *runtime, oasp_object *object, oasp_slice column, int *ordinal, const void **schema,
                                           bool *copy_text, size_t line, const oasp_value **result);
bool oasp_builtin_ado_recordset_move_next(oasp_runtime *runtime, oasp_object *object, size_t line);
bool oasp_builtin_ado_recordset_eof(oasp_runtime *runtime, oasp_object *object, size_t line, bool *result);

#endif
