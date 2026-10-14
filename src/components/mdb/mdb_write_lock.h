// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OASP_MDB_LOCK_H
#define OASP_MDB_LOCK_H

#include <stddef.h>

/*
 * One acquired database lock. `entry` is an opaque process-local path entry,
 * `exclusive` records shared versus writer mode, and `transaction_active`
 * means a before-image journal must be committed or rolled back before release.
 * Callers initialize ownership only through oasp_mdb_lock_acquire().
 */
typedef struct OaspMdbLock {
    void* entry;
    int exclusive;
    int transaction_active;
    int borrowed;
} OaspMdbLock;

/*
 * All cooperating readers lock <database>.eglock in shared mode; writers use
 * exclusive mode. Transactions snapshot the database, fsync commits, and
 * restore the journal on rollback or crash recovery. Every successful acquire
 * must have exactly one release, including error paths.
 */
int oasp_mdb_lock_acquire(const char* db_path, int exclusive, OaspMdbLock* lock, char* error, size_t error_capacity);
int oasp_mdb_transaction_begin(OaspMdbLock* lock, char* error, size_t error_capacity);
int oasp_mdb_transaction_commit(OaspMdbLock* lock, char* error, size_t error_capacity);
int oasp_mdb_transaction_rollback(OaspMdbLock* lock, char* error, size_t error_capacity);
void oasp_mdb_lock_release(OaspMdbLock* lock);

/*
 * ADO uses a connection-scoped write lock so all statements in an explicit
 * transaction share one durable before-image and remain isolated.
 */
int oasp_mdb_transaction_scope_begin(const char* db_path, OaspMdbLock* lock, char* error, size_t error_capacity);
int oasp_mdb_transaction_scope_finish(OaspMdbLock* lock, int rollback, char* error, size_t error_capacity);

#endif
