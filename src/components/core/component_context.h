// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_COMPONENT_CONTEXT_INTERNAL_H
#define OPENASP_COMPONENT_CONTEXT_INTERNAL_H

#include "component.h"
#include "ext_loader.h"

#include <mysql.h>
#include <sqlite3.h>
#include <sys/types.h>
#include <time.h>

typedef struct oasp_mysql_pool_entry oasp_mysql_pool_entry;
typedef struct oasp_sqlite_pool_entry oasp_sqlite_pool_entry;
typedef struct oasp_kv_pool_entry oasp_kv_pool_entry;

/*
 * Internal Component API only. External modules receive neither this layout
 * nor an oasp_runtime pointer through ext_api.h.
 */
struct oasp_component_context {
    oasp_mysql_pool_entry *mysql_pool;
    oasp_sqlite_pool_entry *sqlite_pool;
    oasp_kv_pool_entry *redis_pool;
    oasp_kv_pool_entry *memcache_pool;
    size_t mysql_pool_size;
    size_t mysql_pool_limit;
    size_t sqlite_pool_size;
    size_t sqlite_pool_limit;
    size_t sqlite_cache_kib;
    size_t redis_pool_size;
    size_t memcache_pool_size;
    size_t kv_pool_limit;
    time_t mysql_idle_timeout;
    pid_t owner_pid;
    oasp_external_registry *external_registry;
};

bool oasp_component_context_external_attach(oasp_component_context *context, const oasp_external_module_config *modules, size_t module_count, bool require_sha256, char *error,
                                            size_t error_size);
bool oasp_component_context_external_start_worker(oasp_component_context *context, char *error, size_t error_size);
void oasp_component_context_external_stop_worker(oasp_component_context *context);
void *oasp_component_context_external_registry_get(const oasp_component_context *context);

oasp_kv_pool_entry *oasp_kv_pool_acquire(oasp_component_context *context, bool redis, const char *endpoint, const char *identity);
void oasp_kv_pool_release(oasp_kv_pool_entry **connection);
void oasp_kv_pool_entry_close(oasp_kv_pool_entry *entry);
void oasp_kv_pool_destroy(oasp_kv_pool_entry *entry);
sqlite3 *oasp_sqlite_pool_take(oasp_component_context *context, const char *path);
void oasp_sqlite_pool_put(oasp_component_context *context, sqlite3 *database, const char *path);
MYSQL *oasp_mysql_pool_take(oasp_component_context *context, const char *key);
void oasp_mysql_pool_put(oasp_component_context *context, MYSQL *mysql, const char *key);

#endif
