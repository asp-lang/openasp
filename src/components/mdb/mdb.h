// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`src/components/mdb.h`): Native component boundary. It maps ASP-compatible
 * object operations onto C libraries while preserving runtime error semantics, ownership, and
 * garbage-collector tracing. Result and error variables are initialized before crossing the
 * boundary.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#ifndef OPENASP_MDB_H
#define OPENASP_MDB_H

#include <stdbool.h>
#include <stddef.h>

typedef enum { OASP_MDB_NULL, OASP_MDB_BOOL, OASP_MDB_INTEGER, OASP_MDB_FLOAT, OASP_MDB_STRING } oasp_mdb_value_kind;

typedef struct {
    char *text;
    oasp_mdb_value_kind kind;
} oasp_mdb_value;

typedef struct {
    char **columns;
    oasp_mdb_value *cells;
    size_t column_count;
    size_t row_count;
    char *error;
} oasp_mdb_result;

typedef struct {
    int saved_stdout;
} oasp_mdb_stdout_silence;

bool oasp_mdb_stdout_silence_begin(oasp_mdb_stdout_silence *silence);
void oasp_mdb_stdout_silence_end(oasp_mdb_stdout_silence *silence);
bool oasp_mdb_execute(const char *path, const char *sql, oasp_mdb_result *result);
bool oasp_mdb_execute_command(const char *path, const char *sql, char **error);
bool oasp_mdb_update_record(const char *path, const char *select_sql, const char *const *columns, const char *const *values, size_t count,
                            char **error);
bool oasp_mdb_insert_record(const char *path, const char *select_sql, const char *const *columns, const char *const *values, size_t count,
                            char **error);
bool oasp_mdb_delete_record(const char *path, const char *select_sql, char **error);
void oasp_mdb_result_dispose(oasp_mdb_result *result);

#endif
