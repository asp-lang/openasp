// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/* ADO provider selection and backend connection establishment. */

static char *oasp_ado_mysql_pool_key(oasp_runtime *runtime, const char *host, const char *user, const char *password, const char *database,
                                     unsigned int port, const char *charset, bool disable_tls) {
    size_t required = strlen(host) + strlen(user) + strlen(password) + strlen(database) + strlen(charset) + 160;
    char *key = oasp_runtime_alloc(runtime, required, _Alignof(char));
    if (!key)
        return NULL;
    int length = snprintf(key, required, "h%zu:%s|u%zu:%s|p%zu:%s|d%zu:%s|P%u|c%zu:%s|tls%d", strlen(host), host, strlen(user), user,
                          strlen(password), password, strlen(database), database, port, strlen(charset), charset, disable_tls ? 0 : 1);
    return length >= 0 && (size_t)length < required ? key : NULL;
}

static bool oasp_ado_connection_open(oasp_runtime *runtime, oasp_object *object, const char *connection_text, size_t line) {
    oasp_ado_connection *connection = object->native;
    oasp_ado_connection_finalize(connection);
    connection->connection_string = oasp_ado_string(runtime, connection_text);
    char *provider = oasp_ado_connection_value(runtime, connection_text, "provider");
    if (oasp_ado_contains(provider, "egret.")) {
        oasp_runtime_raise(runtime, 3706, "provider is not registered", "ADODB.Connection", line);
        return false;
    }
    if (oasp_ado_contains(provider, "sqlite")) {
        char *path = oasp_ado_connection_value(runtime, connection_text, "data source");
        char *host = oasp_builtin_host_path(runtime, path);
        if (!host)
            return false;
        connection->component_context = runtime->component_context;
        connection->pool_key = oasp_ado_string(runtime, host);
        connection->database_path = connection->pool_key;
        connection->sqlite = oasp_sqlite_pool_take(connection->component_context, host);
        bool opened_new = connection->sqlite == NULL;
        int status = SQLITE_OK;
        if (opened_new)
            status = sqlite3_open_v2(host, &connection->sqlite, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL);
        if (status != SQLITE_OK) {
            const char *message = connection->sqlite ? sqlite3_errmsg(connection->sqlite) : sqlite3_errstr(status);
            oasp_runtime_raise(runtime, 3709, oasp_runtime_copy(runtime, message, strlen(message)), "ADODB.Connection", line);
            if (connection->sqlite)
                sqlite3_close(connection->sqlite);
            connection->sqlite = NULL;
            return false;
        }
        if (opened_new)
            sqlite3_busy_timeout(connection->sqlite, 5000);
        if (opened_new && connection->component_context && connection->component_context->sqlite_cache_kib) {
            char pragma[64];
            int length = snprintf(pragma, sizeof(pragma), "PRAGMA cache_size=-%zu", connection->component_context->sqlite_cache_kib);
            if (length <= 0 || (size_t)length >= sizeof(pragma) || sqlite3_exec(connection->sqlite, pragma, NULL, NULL, NULL) != SQLITE_OK) {
                const char *message = sqlite3_errmsg(connection->sqlite);
                oasp_runtime_raise(runtime, 3709, oasp_runtime_copy(runtime, message, strlen(message)), "ADODB.Connection", line);
                oasp_sqlite_pool_put(connection->component_context, connection->sqlite, connection->pool_key.data);
                connection->sqlite = NULL;
                return false;
            }
        }
        connection->kind = OASP_DATABASE_SQLITE;
        connection->opened = true;
        return true;
    }
    if (oasp_ado_contains(provider, "jet.oledb") || oasp_ado_contains(provider, "ace.oledb") || oasp_ado_contains(provider, "mdbtools") ||
        oasp_ado_contains(provider, "openasp.mdb") || oasp_ado_contains(provider, "access")) {
        char *path = oasp_ado_connection_value(runtime, connection_text, "data source");
        char *host = oasp_builtin_host_path(runtime, path);
        if (!host || access(host, R_OK) != 0) {
            oasp_runtime_raise(runtime, 3709, "MDB database is not readable", "ADODB.Connection", line);
            return false;
        }
        connection->database_path = oasp_ado_string(runtime, host);
        connection->kind = OASP_DATABASE_MDB;
        connection->opened = true;
        return true;
    }

    char *host = oasp_ado_connection_value(runtime, connection_text, "server");
    if (!*host)
        host = oasp_ado_connection_value(runtime, connection_text, "host");
    if (!*host)
        host = oasp_ado_connection_value(runtime, connection_text, "data source");
    char *user = oasp_ado_connection_value(runtime, connection_text, "user id");
    if (!*user)
        user = oasp_ado_connection_value(runtime, connection_text, "uid");
    char *password = oasp_ado_connection_value(runtime, connection_text, "password");
    char *database = oasp_ado_connection_value(runtime, connection_text, "database");
    char *port_text = oasp_ado_connection_value(runtime, connection_text, "port");
    char *charset = oasp_ado_connection_value(runtime, connection_text, "charset");
    char *timeout_text = oasp_ado_connection_value(runtime, connection_text, "connection timeout");
    char *ssl_mode = oasp_ado_connection_value(runtime, connection_text, "ssl mode");
    char *buffered_result = oasp_ado_connection_value(runtime, connection_text, "bufferedresult");
    if (!*ssl_mode)
        ssl_mode = oasp_ado_connection_value(runtime, connection_text, "sslmode");
    unsigned int port = *port_text ? (unsigned int)strtoul(port_text, NULL, 10) : 3306;
    unsigned int timeout = *timeout_text ? (unsigned int)strtoul(timeout_text, NULL, 10) : 5;
    const char *normalized_host = *host ? host : "127.0.0.1";
    const char *normalized_user = *user ? user : "root";
    bool disable_tls = !strcasecmp(ssl_mode, "disabled") || !strcasecmp(ssl_mode, "disable") || !strcasecmp(ssl_mode, "none") ||
                       !strcasecmp(ssl_mode, "false");
    connection->mysql_store_result = !strcasecmp(buffered_result, "true") || !strcmp(buffered_result, "1") ||
                                     !strcasecmp(buffered_result, "yes") || !strcasecmp(buffered_result, "on");
    char *pool_key = oasp_ado_mysql_pool_key(runtime, normalized_host, normalized_user, password, database, port, charset, disable_tls);
    if (!pool_key)
        return false;
    connection->component_context = runtime->component_context;
    connection->pool_key = (oasp_slice){pool_key, strlen(pool_key)};
    connection->mysql = oasp_mysql_pool_take(connection->component_context, pool_key);
    if (connection->mysql && ((*database && mysql_select_db(connection->mysql, database)) || (*charset && mysql_set_character_set(connection->mysql, charset)))) {
        mysql_close(connection->mysql);
        connection->mysql = NULL;
    }
    if (connection->mysql) {
        connection->kind = OASP_DATABASE_MYSQL;
        connection->opened = true;
        return true;
    }
    connection->mysql = mysql_init(NULL);
    if (!connection->mysql)
        return false;
    mysql_options(connection->mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    if (*charset)
        mysql_options(connection->mysql, MYSQL_SET_CHARSET_NAME, charset);
#if defined(MYSQL_VERSION_ID) && MYSQL_VERSION_ID >= 50711 && !defined(MARIADB_BASE_VERSION)
    if (disable_tls) {
        enum mysql_ssl_mode mode = SSL_MODE_DISABLED;
        mysql_options(connection->mysql, MYSQL_OPT_SSL_MODE, &mode);
    }
#endif
    if (!mysql_real_connect(connection->mysql, normalized_host, normalized_user, password, *database ? database : NULL, port, NULL,
                            CLIENT_MULTI_STATEMENTS)) {
        const char *message = mysql_error(connection->mysql);
        oasp_runtime_raise(runtime, 3709, oasp_runtime_copy(runtime, message, strlen(message)), "ADODB.Connection", line);
        mysql_close(connection->mysql);
        connection->mysql = NULL;
        return false;
    }
    connection->kind = OASP_DATABASE_MYSQL;
    connection->opened = true;
    return true;
}
