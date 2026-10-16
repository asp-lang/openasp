// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "component_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static size_t component_env_size(const char *name, size_t fallback, size_t maximum) {
    const char *text = getenv(name);
    if (!text || !*text)
        return fallback;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    return end && !*end && value <= maximum ? (size_t)value : fallback;
}

static void mysql_pool_expire(oasp_component_context *context, time_t now);

void oasp_kv_pool_entry_close(oasp_kv_pool_entry *entry) {
    if (!entry)
        return;
    if (entry->fd >= 0)
        close(entry->fd);
    entry->fd = -1;
    entry->redis_database = 0;
    entry->reader.start = 0;
    entry->reader.end = 0;
    entry->protocol_clean = true;
}

void oasp_kv_pool_destroy(oasp_kv_pool_entry *entry) {
    while (entry) {
        oasp_kv_pool_entry *next = entry->next;
        oasp_kv_pool_entry_close(entry);
        free(entry->endpoint);
        free(entry->identity);
        free(entry);
        entry = next;
    }
}

static void component_context_check_pid(oasp_component_context *context) {
    if (!context || context->owner_pid == getpid())
        return;
    for (oasp_kv_pool_entry *entry = context->redis_pool; entry; entry = entry->next) {
        oasp_kv_pool_entry_close(entry);
        entry->borrowers = 0;
    }
    for (oasp_kv_pool_entry *entry = context->memcache_pool; entry; entry = entry->next) {
        oasp_kv_pool_entry_close(entry);
        entry->borrowers = 0;
    }
    context->owner_pid = getpid();
}

oasp_component_context *oasp_component_context_create(void) {
    oasp_component_context *context = calloc(1, sizeof(*context));
    if (!context)
        return NULL;
    context->mysql_pool_limit = component_env_size("OPENASP_MYSQL_POOL_SIZE", 4, 64);
    context->mysql_idle_timeout = (time_t)component_env_size("OPENASP_MYSQL_POOL_IDLE_SECONDS", 30, 3600);
    context->sqlite_pool_limit = component_env_size("OPENASP_SQLITE_POOL_PATHS", 8, 64);
    context->sqlite_cache_kib = component_env_size("OPENASP_SQLITE_CACHE_KIB", 256, 65536);
    context->kv_pool_limit = component_env_size("OPENASP_KV_POOL_ENDPOINTS", 8, 64);
    context->owner_pid = getpid();
    return context;
}

void oasp_component_context_destroy(oasp_component_context *context) {
    if (!context)
        return;
    while (context->mysql_pool) {
        oasp_mysql_pool_entry *entry = context->mysql_pool;
        context->mysql_pool = entry->next;
        mysql_close(entry->mysql);
        free(entry->key);
        free(entry);
    }
    while (context->sqlite_pool) {
        oasp_sqlite_pool_entry *entry = context->sqlite_pool;
        context->sqlite_pool = entry->next;
        sqlite3_close(entry->database);
        free(entry->path);
        free(entry);
    }
    oasp_kv_pool_destroy(context->redis_pool);
    oasp_kv_pool_destroy(context->memcache_pool);
    oasp_external_registry_destroy(context->external_registry);
    free(context);
}

bool oasp_component_context_load_external(oasp_component_context *context, const oasp_external_module_config *modules, size_t module_count, bool require_sha256, char *error,
                                          size_t error_size) {
    return oasp_component_context_external_attach(context, modules, module_count, require_sha256, error, error_size);
}

bool oasp_component_context_external_worker_init(oasp_component_context *context, char *error, size_t error_size) {
    return oasp_component_context_external_start_worker(context, error, error_size);
}

void oasp_component_context_external_worker_shutdown(oasp_component_context *context) {
    oasp_component_context_external_stop_worker(context);
}

void *oasp_component_context_external_registry(const oasp_component_context *context) {
    return oasp_component_context_external_registry_get(context);
}

void oasp_component_context_release_memory(oasp_component_context *context) {
    if (!context)
        return;
    component_context_check_pid(context);
    for (oasp_sqlite_pool_entry *entry = context->sqlite_pool; entry; entry = entry->next)
        (void)sqlite3_db_release_memory(entry->database);
    mysql_pool_expire(context, time(NULL));
}

oasp_kv_pool_entry *oasp_kv_pool_acquire(oasp_component_context *context, bool redis, const char *endpoint, const char *identity) {
    if (!endpoint || !identity) {
        errno = EINVAL;
        return NULL;
    }
    oasp_kv_pool_entry **pool = NULL;
    size_t *size = NULL;
    if (context) {
        component_context_check_pid(context);
        pool = redis ? &context->redis_pool : &context->memcache_pool;
        size = redis ? &context->redis_pool_size : &context->memcache_pool_size;
        oasp_kv_pool_entry *replaceable = NULL;
        for (oasp_kv_pool_entry *entry = *pool; entry; entry = entry->next) {
            if (strcmp(entry->endpoint, endpoint) || entry->borrowers)
                continue;
            if (strcmp(entry->identity, identity)) {
                if (!replaceable)
                    replaceable = entry;
                continue;
            }
            if (entry->fd >= 0) {
                unsigned char byte;
                ssize_t peek = recv(entry->fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
                if (peek == 0 || peek > 0 || (peek < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                    oasp_kv_pool_entry_close(entry);
            }
            entry->borrowers = 1;
            return entry;
        }
        if (replaceable) {
            char *replacement = strdup(identity);
            if (!replacement)
                return NULL;
            free(replaceable->identity);
            replaceable->identity = replacement;
            oasp_kv_pool_entry_close(replaceable);
            replaceable->borrowers = 1;
            return replaceable;
        }
        if (!context->kv_pool_limit || *size >= context->kv_pool_limit) {
            errno = EMFILE;
            return NULL;
        }
    }
    oasp_kv_pool_entry *entry = calloc(1, sizeof(*entry));
    if (entry) {
        entry->endpoint = strdup(endpoint);
        entry->identity = strdup(identity);
    }
    if (!entry || !entry->endpoint || !entry->identity) {
        if (entry) {
            free(entry->endpoint);
            free(entry->identity);
        }
        free(entry);
        return NULL;
    }
    entry->fd = -1;
    entry->protocol_clean = true;
    entry->borrowers = 1;
    if (pool) {
        entry->next = *pool;
        *pool = entry;
        (*size)++;
    }
    return entry;
}

void oasp_kv_pool_release(oasp_kv_pool_entry **connection) {
    if (!connection || !*connection)
        return;
    oasp_kv_pool_entry *entry = *connection;
    if (!entry->protocol_clean || entry->reader.start != entry->reader.end)
        oasp_kv_pool_entry_close(entry);
    if (entry->borrowers)
        entry->borrowers--;
    *connection = NULL;
}

sqlite3 *oasp_sqlite_pool_take(oasp_component_context *context, const char *path) {
    if (!context || !path)
        return NULL;
    oasp_sqlite_pool_entry **link = &context->sqlite_pool;
    while (*link) {
        oasp_sqlite_pool_entry *entry = *link;
        if (!strcmp(entry->path, path)) {
            *link = entry->next;
            context->sqlite_pool_size--;
            sqlite3 *database = entry->database;
            free(entry->path);
            free(entry);
            return database;
        }
        link = &entry->next;
    }
    return NULL;
}

void oasp_sqlite_pool_put(oasp_component_context *context, sqlite3 *database, const char *path) {
    if (!database)
        return;
    sqlite3_stmt *statement = sqlite3_next_stmt(database, NULL);
    while (statement) {
        sqlite3_stmt *next = sqlite3_next_stmt(database, statement);
        sqlite3_finalize(statement);
        statement = next;
    }
    if (!sqlite3_get_autocommit(database) && sqlite3_exec(database, "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(database);
        return;
    }
    if (!context || !path || !context->sqlite_pool_limit || context->sqlite_pool_size >= context->sqlite_pool_limit) {
        sqlite3_close(database);
        return;
    }
    for (oasp_sqlite_pool_entry *entry = context->sqlite_pool; entry; entry = entry->next) {
        if (!strcmp(entry->path, path)) {
            sqlite3_close(database);
            return;
        }
    }
    oasp_sqlite_pool_entry *entry = calloc(1, sizeof(*entry));
    if (entry)
        entry->path = strdup(path);
    if (!entry || !entry->path) {
        free(entry);
        sqlite3_close(database);
        return;
    }
    entry->database = database;
    entry->next = context->sqlite_pool;
    context->sqlite_pool = entry;
    context->sqlite_pool_size++;
}

static void mysql_pool_expire(oasp_component_context *context, time_t now) {
    if (!context)
        return;
    oasp_mysql_pool_entry **link = &context->mysql_pool;
    while (*link) {
        oasp_mysql_pool_entry *entry = *link;
        if (context->mysql_idle_timeout > 0 && now - entry->idle_since >= context->mysql_idle_timeout) {
            *link = entry->next;
            mysql_close(entry->mysql);
            free(entry->key);
            free(entry);
            context->mysql_pool_size--;
        } else {
            link = &entry->next;
        }
    }
}

MYSQL *oasp_mysql_pool_take(oasp_component_context *context, const char *key) {
    if (!context || !key)
        return NULL;
    mysql_pool_expire(context, time(NULL));
    oasp_mysql_pool_entry **link = &context->mysql_pool;
    while (*link) {
        oasp_mysql_pool_entry *entry = *link;
        if (!strcmp(entry->key, key)) {
            *link = entry->next;
            context->mysql_pool_size--;
            MYSQL *mysql = entry->mysql;
            free(entry->key);
            free(entry);
            if (!mysql_ping(mysql))
                return mysql;
            mysql_close(mysql);
            return NULL;
        }
        link = &entry->next;
    }
    return NULL;
}

void oasp_mysql_pool_put(oasp_component_context *context, MYSQL *mysql, const char *key) {
    if (!mysql)
        return;
    if (!context || !key || !context->mysql_pool_limit || mysql_reset_connection(mysql)) {
        mysql_close(mysql);
        return;
    }
    mysql_pool_expire(context, time(NULL));
    if (context->mysql_pool_size >= context->mysql_pool_limit) {
        mysql_close(mysql);
        return;
    }
    oasp_mysql_pool_entry *entry = calloc(1, sizeof(*entry));
    if (entry)
        entry->key = strdup(key);
    if (!entry || !entry->key) {
        free(entry);
        mysql_close(mysql);
        return;
    }
    entry->mysql = mysql;
    entry->idle_since = time(NULL);
    entry->next = context->mysql_pool;
    context->mysql_pool = entry;
    context->mysql_pool_size++;
}

bool oasp_component_context_external_attach(oasp_component_context *context, const oasp_external_module_config *modules, size_t module_count, bool require_sha256, char *error,
                                            size_t error_size) {
    if (!context || context->external_registry)
        return false;
    return oasp_external_registry_create(&context->external_registry, modules, module_count, require_sha256, error, error_size);
}

bool oasp_component_context_external_start_worker(oasp_component_context *context, char *error, size_t error_size) {
    return context && oasp_external_registry_worker_init(context->external_registry, error, error_size);
}

void oasp_component_context_external_stop_worker(oasp_component_context *context) {
    if (context)
        oasp_external_registry_worker_shutdown(context->external_registry);
}

void *oasp_component_context_external_registry_get(const oasp_component_context *context) {
    return context ? context->external_registry : NULL;
}
