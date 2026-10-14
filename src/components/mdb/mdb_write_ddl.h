// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OASP_MDB_DDL_H
#define OASP_MDB_DDL_H

#include <stddef.h>

typedef struct S_MdbTableDef MdbTableDef;
typedef struct mdbindex MdbIndex;

/*
 * Native Jet 4 DDL surface. CREATE TABLE owns file creation and transaction
 * setup; page append helpers operate on an already-open mdbtools table and
 * return diagnostics through the caller-provided bounded buffer.
 */
int oasp_mdb_create_table_file(const char* db_path, const char* sql, char* error, size_t error_capacity);
int oasp_mdb_alter_table_file(const char* db_path, const char* sql, char* error, size_t error_capacity);
int oasp_mdb_drop_table_file(const char* db_path, const char* sql, char* error, size_t error_capacity);
int oasp_mdb_create_index_file(const char* db_path, const char* sql, char* error, size_t error_capacity);
int oasp_mdb_drop_index_file(const char* db_path, const char* sql, char* error, size_t error_capacity);
int oasp_mdb_append_table_data_page(MdbTableDef* table, char* error, size_t error_capacity);
int oasp_mdb_append_index_page(MdbTableDef* table, MdbIndex* index, unsigned int* page_number, char* error, size_t error_capacity);
int oasp_mdb_store_long_value(MdbTableDef* table, size_t column_index, const unsigned char* value, size_t length, unsigned char descriptor[12],
                              char* error, size_t error_capacity);

#endif
