// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#ifndef OPENASP_MDB_WRITE_H
#define OPENASP_MDB_WRITE_H

#include <mdbtools.h>

typedef struct S_MdbTableDef MdbTableDef;

char *oasp_mdb_native_update_raw(const char *db_path, const char *table_name, const char *where_column, const char *where_value, const char *set_columns,
                                 const char *set_values);
char *oasp_mdb_native_update_typed_where_raw(const char *db_path, const char *table_name, const char *where_sql, const char *set_columns, const char *set_values);
char *oasp_mdb_native_insert_raw(const char *db_path, const char *table_name, const char *set_columns, const char *set_values);
char *oasp_mdb_native_delete_raw(const char *db_path, const char *table_name, const char *where_column, const char *where_value);
char *oasp_mdb_native_update_where_raw(const char *db_path, const char *table_name, const char *where_sql, const char *set_columns, const char *set_expressions);
char *oasp_mdb_native_delete_where_raw(const char *db_path, const char *table_name, const char *where_sql);

int oasp_mdb_convert_literal_field(MdbHandle* mdb, MdbColumn* column, const char* literal, MdbField* field, char** owned_value);
int oasp_mdb_replace_fields(MdbTableDef* table, int row_number, int num_fields, MdbField* fields);
int oasp_mdb_decode_jet4_row(MdbTableDef* table, int row_start, size_t row_size, MdbField* fields);
int oasp_mdb_backfill_indexes(MdbTableDef* table);

#endif
