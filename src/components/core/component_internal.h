// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_COMPONENT_INTERNAL_H
#define OPENASP_COMPONENT_INTERNAL_H

#include "component.h"
#include "component_context.h"
#include "mdb_write_lock.h"

#include <curl/curl.h>
#include <libxml/tree.h>
#include <mysql.h>
#include <openssl/ssl.h>
#include <sqlite3.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>

oasp_slice oasp_component_string(oasp_runtime *runtime, const char *text);

typedef struct oasp_runtime oasp_runtime;

typedef struct {
    oasp_buffer buffer;
    size_t position;
    int type;
    int mode;
    int state;
    oasp_slice charset;
} oasp_stream;

typedef struct {
    FILE *file;
    int mode;
} oasp_text_file;

typedef struct {
    oasp_slice path;
    time_t modified;
    off_t size;
} oasp_file_info;

typedef struct {
    xmlDocPtr document;
    uint64_t generation;
    int error_code;
    int error_line;
    int error_column;
    oasp_slice error_reason;
} oasp_xml_document;

typedef struct {
    oasp_xml_document *owner;
    xmlNodePtr node;
    uint64_t generation;
    bool owns_node;
} oasp_xml_node;

typedef struct {
    oasp_xml_node *node;
} oasp_xml_attribute_map;

typedef struct oasp_http_multi oasp_http_multi;

typedef struct {
    oasp_buffer response;
    oasp_buffer response_headers;
    struct curl_slist *headers;
    CURL *easy;
    oasp_slice method;
    oasp_slice url;
    oasp_slice body;
    oasp_slice content_type;
    oasp_slice ca_file;
    oasp_object *next_multi_request;
    oasp_http_multi *multi_owner;
    long status;
    long timeout_ms;
    long error_code;
    size_t max_response_bytes;
    int ready_state;
    char error_message[CURL_ERROR_SIZE];
    bool completed;
    bool timeout_explicit;
    bool tls_verify;
} oasp_http;

struct oasp_http_multi {
    oasp_object *first;
    oasp_object *last;
    size_t count;
    long max_concurrency;
    long timeout_ms;
    bool executing;
};

typedef struct {
    oasp_slice api_key;
    oasp_slice base_url;
    oasp_slice model;
    oasp_slice last_response;
    long status;
    long timeout_ms;
    long idle_timeout_ms;
    size_t max_event_bytes;
    size_t max_buffered_bytes;
    size_t max_response_bytes;
} oasp_openai;

typedef enum { OASP_DATABASE_NONE, OASP_DATABASE_SQLITE, OASP_DATABASE_MDB, OASP_DATABASE_MYSQL } oasp_database_kind;

typedef struct oasp_ado_recordset oasp_ado_recordset;
typedef struct oasp_ado_row_segment oasp_ado_row_segment;
typedef bool (*oasp_ado_row_source_fetch)(oasp_runtime *runtime, oasp_ado_recordset *recordset, size_t line);
typedef void (*oasp_ado_row_source_close)(oasp_ado_recordset *recordset);

typedef struct oasp_mysql_pool_entry {
    struct oasp_mysql_pool_entry *next;
    MYSQL *mysql;
    char *key;
    time_t idle_since;
} oasp_mysql_pool_entry;

typedef struct oasp_sqlite_pool_entry {
    struct oasp_sqlite_pool_entry *next;
    sqlite3 *database;
    char *path;
} oasp_sqlite_pool_entry;

#define OASP_PROTOCOL_BUFFER_SIZE 8192

typedef struct {
    unsigned char data[OASP_PROTOCOL_BUFFER_SIZE];
    size_t start;
    size_t end;
} oasp_protocol_reader;

typedef struct oasp_kv_pool_entry {
    struct oasp_kv_pool_entry *next;
    char *endpoint;
    char *identity;
    int fd;
    int redis_database;
    size_t borrowers;
    oasp_protocol_reader reader;
    bool protocol_clean;
} oasp_kv_pool_entry;

typedef struct {
    oasp_database_kind kind;
    sqlite3 *sqlite;
    MYSQL *mysql;
    oasp_component_context *component_context;
    oasp_ado_recordset *active_recordsets;
    oasp_slice pool_key;
    oasp_slice database_path;
    oasp_slice connection_string;
    OaspMdbLock mdb_transaction_lock;
    bool mdb_transaction_active;
    bool mysql_store_result;
    bool opened;
} oasp_ado_connection;

struct oasp_ado_row_segment {
    oasp_ado_row_segment *next;
    oasp_arena *arena;
    oasp_value *cells;
    size_t first_row;
    size_t row_count;
    size_t row_capacity;
};

typedef struct {
    const char *data;
    uint32_t length;
    uint32_t index;
} oasp_ado_column;

_Static_assert(sizeof(oasp_ado_column) == sizeof(oasp_slice),
               "sorted Recordset columns must not increase resident metadata");

struct oasp_ado_recordset {
    oasp_ado_column *columns;
    oasp_ado_row_segment *segments;
    oasp_ado_row_segment *segment_tail;
    oasp_ado_row_segment *current_segment;
    size_t column_count;
    size_t row_count;
    size_t cursor;
    size_t page_size;
    int cursor_type;
    int lock_type;
    bool opened;
    oasp_slice source;
    oasp_object *connection;
    bool *dirty_columns;
    bool adding_new;
    MYSQL_RES *mysql_result;
    void *row_source;
    oasp_ado_row_source_fetch row_source_fetch;
    oasp_ado_row_source_close row_source_close;
    oasp_ado_recordset *next_active;
    bool source_exhausted;
    bool sliding_window;
};

typedef struct {
    oasp_object *recordset;
    size_t index;
} oasp_ado_field;

typedef struct oasp_ado_parameter_link {
    struct oasp_ado_parameter_link *next;
    oasp_object *parameter;
} oasp_ado_parameter_link;

typedef struct {
    oasp_ado_parameter_link *first;
    oasp_ado_parameter_link *last;
    size_t count;
} oasp_ado_parameters;

typedef struct {
    oasp_object *connection;
    oasp_object *parameters;
    oasp_slice command_text;
    int command_type;
    int command_timeout;
    bool prepared;
} oasp_ado_command;

typedef struct {
    oasp_slice name;
    int type;
    int direction;
    int size;
    oasp_value value;
} oasp_ado_parameter;

typedef struct {
    oasp_slice protocol;
    oasp_slice host;
    oasp_slice port;
    oasp_slice peer_host;
    int peer_port;
    int64_t timeout;
    int fd;
    int state;
    bool opened;
} oasp_socket;

typedef struct {
    pid_t pid;
    pid_t last_pid;
    int exit_code;
    int term_signal;
    int last_error;
    int64_t timeout_ms;
    size_t max_output_bytes;
    oasp_slice standard_output;
    oasp_slice standard_error;
    bool timed_out;
} oasp_process;

typedef struct {
    int fd;
    SSL_CTX *tls_context;
    SSL *tls;
    oasp_slice url;
    oasp_slice subprotocol;
    oasp_slice ca_file;
    oasp_slice message_type;
    int64_t timeout_ms;
    int opcode;
    int close_code;
    bool final;
    bool opened;
    bool client;
    bool tls_verify;
} oasp_websocket;

typedef struct {
    oasp_slice host;
    oasp_slice protocol;
    oasp_slice ca_file;
    oasp_component_context *component_context;
    oasp_kv_pool_entry *connection;
    int port;
    int64_t timeout_ms;
    int64_t last_flags;
    int64_t last_cas;
    bool opened;
    bool connection_owned;
    bool tls_enabled;
    bool tls_verify;
} oasp_memcache;

typedef struct {
    oasp_slice host;
    oasp_slice username;
    oasp_slice password;
    oasp_component_context *component_context;
    oasp_kv_pool_entry *connection;
    int port;
    int database;
    int64_t timeout_ms;
    bool opened;
    bool connection_owned;
} oasp_redis;

#endif
