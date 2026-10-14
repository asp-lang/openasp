// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Filesystem-backed ASP Session/Application store. Names are derived from a
 * canonical root namespace, values use the validated state_binary format, and
 * lock files serialize updates across FastCGI workers. Temporary-file plus
 * rename publication prevents readers from observing partial state.
 */

#ifndef OPENASP_STATE_STORE_H
#define OPENASP_STATE_STORE_H

#include "runtime.h"

#include <stdbool.h>
#include <time.h>

typedef struct oasp_state_store oasp_state_store;

/*
 * Opens or creates the storage namespace under `directory`. Both path strings
 * are copied; NULL reports invalid paths, allocation failure, or setup failure.
 */
oasp_state_store *oasp_state_store_open(const char *directory, const char *root);
void oasp_state_store_close(oasp_state_store *store);

/*
 * Acquires/releases the process-shared exclusive lock for one validated session
 * id. Every successful lock must be paired with unlock on all error paths.
 */
bool oasp_state_store_lock_session(oasp_state_store *store, const char *id);
void oasp_state_store_unlock_session(oasp_state_store *store, const char *id);

/*
 * Loads non-expired session bindings into `runtime`, returning persisted
 * timeout. Save atomically replaces bytes and records `touched`; delete removes
 * abandoned state. Callers hold the session lock around the transaction.
 */
bool oasp_state_store_load_session(oasp_state_store *store, const char *id, oasp_runtime *runtime, oasp_binding **bindings, int *timeout,
                                   time_t now);
bool oasp_state_store_save_session(oasp_state_store *store, const char *id, const oasp_binding *bindings, int timeout, time_t touched);
bool oasp_state_store_delete_session(oasp_state_store *store, const char *id);

/*
 * Application state is global to the root namespace. Merge compares `before`
 * with `after` and applies only changed bindings while holding the store lock,
 * preserving concurrent updates to unrelated keys.
 */
bool oasp_state_store_load_application(oasp_state_store *store, oasp_runtime *runtime, oasp_binding **bindings);
bool oasp_state_store_merge_application(oasp_state_store *store, const oasp_binding *before, const oasp_binding *after);

#endif
