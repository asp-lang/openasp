// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "mdb_write.h"
#include "mdb.h"
#include "mdb_write_ddl.h"
#include "mdb_write_lock.h"
#include "oasp_string.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mdbtools.h>
#include <mdbsql.h>
#define OASP_MDB_BIND_SIZE MDB_BIND_SIZE
#define OASP_MDB_SEP 31
#define OASP_MDB_DELETED_ROW 0x4000
#define OASP_MDB_OFFSET_MASK 0x1fff
#define OASP_MDB_INDEX_DATA_OFFSET 480
#define OASP_MDB_INDEX_MASK_OFFSET 27
#define OASP_MDB_INDEX_MASK_SIZE 453
#define OASP_MDB_INDEX_LEAF_ENTRY_SIZE 9
#define OASP_MDB_INDEX_NODE_ENTRY_SIZE 13

extern ssize_t mdb_write_pg(MdbHandle* mdb, unsigned long pg);

/*
 * Native MDB adapter.
 *
 * Public functions translate Egret strings to owned C strings and return one
 * tabular wire format. Query paths hold a shared sidecar lock; mutation paths
 * hold an exclusive lock and a before-image transaction. No command-line tool
 * or JVM participates in database access.
 */
typedef struct EgMdbStringBuilder {
    /* Always NUL-terminated after successful append; take transfers ownership. */
    char* data;
    size_t len;
    size_t cap;
} EgMdbStringBuilder;

typedef struct EgMdbSplitList {
    /* Unit-separator fields decoded from the typed Egret/native ABI payload. */
    char** items;
    size_t len;
} EgMdbSplitList;

typedef struct EgMdbQueryRow {
    /* Owned projected values plus a deterministic key for random ordering. */
    char** values;
    uint64_t random_key;
} EgMdbQueryRow;

/*
 * MDBTools 1.0.1 computes IS NULL/IS NOT NULL from MdbField::is_null, but
 * then falls through to the type-specific comparator and overwrites that
 * result. Keep this implementation in the wrapper object so the static
 * linker does not pull the defective sargs.o from libmdb.
 */
void mdb_sql_walk_tree(MdbSargNode* node, MdbSargTreeFunc func, gpointer data) {
    if (func(node, data)) {
        return;
    }
    if (node->left) {
        mdb_sql_walk_tree(node->left, func, data);
    }
    if (node->right) {
        mdb_sql_walk_tree(node->right, func, data);
    }
}

int mdb_test_string(MdbSargNode* node, char* value) {
    int comparison = 0;
    if (node->op == MDB_LIKE) {
        return mdb_like_cmp(value, node->value.s);
    }
    if (node->op == MDB_ILIKE) {
        return mdb_ilike_cmp(value, node->value.s);
    }
    comparison = strcoll(node->value.s, value);
    if (node->op == MDB_EQUAL) {
        return comparison == 0;
    }
    if (node->op == MDB_GT) {
        return comparison < 0;
    }
    if (node->op == MDB_LT) {
        return comparison > 0;
    }
    if (node->op == MDB_GTEQ) {
        return comparison <= 0;
    }
    if (node->op == MDB_LTEQ) {
        return comparison >= 0;
    }
    if (node->op == MDB_NEQ) {
        return comparison != 0;
    }
    return 0;
}

int mdb_test_int(MdbSargNode* node, gint32 value) {
    gint32 expected = node->val_type == MDB_INT ? node->value.i : (gint32)node->value.d;
    if (node->op == MDB_EQUAL) {
        return expected == value;
    }
    if (node->op == MDB_GT) {
        return expected < value;
    }
    if (node->op == MDB_LT) {
        return expected > value;
    }
    if (node->op == MDB_GTEQ) {
        return expected <= value;
    }
    if (node->op == MDB_LTEQ) {
        return expected >= value;
    }
    if (node->op == MDB_NEQ) {
        return expected != value;
    }
    return 0;
}

int mdb_test_double(int op, double expected, double value) {
    if (op == MDB_EQUAL) {
        return expected == value;
    }
    if (op == MDB_GT) {
        return expected < value;
    }
    if (op == MDB_LT) {
        return expected > value;
    }
    if (op == MDB_GTEQ) {
        return expected <= value;
    }
    if (op == MDB_LTEQ) {
        return expected >= value;
    }
    if (op == MDB_NEQ) {
        return expected != value;
    }
    return 0;
}

static double oasp_mdb_truncate_date(double value) {
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%.6f", value);
    sscanf(buffer, "%lf", &value);
    return value;
}

int mdb_find_indexable_sargs(MdbSargNode* node, gpointer data) {
    MdbSarg sarg;
    (void)data;
    if (node->op == MDB_OR || node->op == MDB_NOT) {
        return 1;
    }
    if (mdb_is_relational_op(node->op) && node->col) {
        sarg.op = node->op;
        sarg.value = node->value;
        mdb_add_sarg(node->col, &sarg);
    }
    return 0;
}

int mdb_test_sarg(MdbHandle* mdb, MdbColumn* col, MdbSargNode* node, MdbField* field) {
    char buffer[256];
    char* value = NULL;
    if (node->op == MDB_ISNULL) {
        return field->is_null;
    }
    if (node->op == MDB_NOTNULL) {
        return !field->is_null;
    }
    if (col->col_type == MDB_BOOL) {
        return mdb_test_int(node, !field->is_null);
    }
    if (col->col_type == MDB_BYTE) {
        return mdb_test_int(node, (gint32)((char*)field->value)[0]);
    }
    if (col->col_type == MDB_INT) {
        return mdb_test_int(node, (gint32)mdb_get_int16(field->value, 0));
    }
    if (col->col_type == MDB_LONGINT) {
        return mdb_test_int(node, (gint32)mdb_get_int32(field->value, 0));
    }
    if (col->col_type == MDB_FLOAT) {
        return mdb_test_double(node->op, node->val_type == MDB_INT ? node->value.i : node->value.d, mdb_get_single(field->value, 0));
    }
    if (col->col_type == MDB_DOUBLE) {
        return mdb_test_double(node->op, node->val_type == MDB_INT ? node->value.i : node->value.d, mdb_get_double(field->value, 0));
    }
    if (col->col_type == MDB_TEXT) {
        mdb_unicode2ascii(mdb, field->value, field->siz, buffer, sizeof(buffer));
        return mdb_test_string(node, buffer);
    }
    if (col->col_type == MDB_MEMO || col->col_type == MDB_REPID) {
        value = mdb_col_to_string(mdb, mdb->pg_buf, field->start, col->col_type, (gint32)mdb_get_int32(field->value, 0));
        int result = mdb_test_string(node, value);
        g_free(value);
        return result;
    }
    if (col->col_type == MDB_DATETIME) {
        return mdb_test_double(node->op, oasp_mdb_truncate_date(node->value.d), oasp_mdb_truncate_date(mdb_get_double(field->value, 0)));
    }
    return 1;
}

int mdb_find_field(int col_num, MdbField* fields, int num_fields) {
    int i = 0;
    while (i < num_fields) {
        if (fields[i].colnum == col_num) {
            return i;
        }
        i++;
    }
    return -1;
}

int mdb_test_sarg_node(MdbHandle* mdb, MdbSargNode* node, MdbField* fields, int num_fields) {
    if (mdb_is_relational_op(node->op)) {
        if (!node->col) {
            return node->value.i;
        }
        int field_index = mdb_find_field(node->col->col_num, fields, num_fields);
        return field_index >= 0 && mdb_test_sarg(mdb, node->col, node, &fields[field_index]);
    }
    if (node->op == MDB_NOT) {
        return !mdb_test_sarg_node(mdb, node->left, fields, num_fields);
    }
    if (node->op == MDB_AND) {
        return mdb_test_sarg_node(mdb, node->left, fields, num_fields)
            && mdb_test_sarg_node(mdb, node->right, fields, num_fields);
    }
    if (node->op == MDB_OR) {
        return mdb_test_sarg_node(mdb, node->left, fields, num_fields)
            || mdb_test_sarg_node(mdb, node->right, fields, num_fields);
    }
    return 1;
}

int mdb_test_sargs(MdbTableDef* table, MdbField* fields, int num_fields) {
    if (!table->sarg_tree) {
        return 1;
    }
    return mdb_test_sarg_node(table->entry->mdb, table->sarg_tree, fields, num_fields);
}

int mdb_add_sarg(MdbColumn* col, MdbSarg* input) {
    if (!col->sargs) {
        col->sargs = g_ptr_array_new();
    }
    MdbSarg* sarg = g_memdup2(input, sizeof(MdbSarg));
    g_ptr_array_add(col->sargs, sarg);
    col->num_sargs++;
    return 1;
}

int mdb_add_sarg_by_name(MdbTableDef* table, char* column_name, MdbSarg* input) {
    unsigned int i = 0;
    while (i < table->num_cols) {
        MdbColumn* col = g_ptr_array_index(table->columns, i);
        if (!g_ascii_strcasecmp(col->name, column_name)) {
            return mdb_add_sarg(col, input);
        }
        i++;
    }
    return 0;
}

static int oasp_asp_hex_nibble(unsigned char ch) {
    if (ch >= '0' && ch <= '9') {
        return (int)(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return (int)(ch - 'a') + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return (int)(ch - 'A') + 10;
    }
    return -1;
}

oasp_string_t* oasp_asp_hex_encode(oasp_string_t* input) {
    static const char digits[] = "0123456789abcdef";
    const unsigned char* source = (const unsigned char*)oasp_string_cstr(input);
    int64_t source_len = oasp_string_len(input);
    char* output = NULL;
    int64_t i = 0;
    if (!source || source_len <= 0) {
        return oasp_string_from_bytes("", 0);
    }
    if (source_len > INT64_MAX / 2) {
        return oasp_string_from_bytes("", 0);
    }
    output = (char*)malloc((size_t)source_len * 2);
    if (!output) {
        return oasp_string_from_bytes("", 0);
    }
    while (i < source_len) {
        unsigned char value = source[i];
        output[i * 2] = digits[value >> 4];
        output[i * 2 + 1] = digits[value & 15];
        i++;
    }
    oasp_string_t* result = oasp_string_from_bytes(output, source_len * 2);
    free(output);
    return result;
}

oasp_string_t* oasp_asp_hex_decode(oasp_string_t* input) {
    const unsigned char* source = (const unsigned char*)oasp_string_cstr(input);
    int64_t source_len = oasp_string_len(input);
    int64_t output_len = 0;
    unsigned char* output = NULL;
    int64_t i = 0;
    if (!source || source_len < 2) {
        return oasp_string_from_bytes("", 0);
    }
    output = (unsigned char*)malloc((size_t)(source_len / 2));
    if (!output) {
        return oasp_string_from_bytes("", 0);
    }
    while (i + 1 < source_len) {
        int high = oasp_asp_hex_nibble(source[i]);
        int low = oasp_asp_hex_nibble(source[i + 1]);
        if (high < 0 || low < 0) {
            break;
        }
        output[output_len++] = (unsigned char)((high << 4) | low);
        i += 2;
    }
    oasp_string_t* result = oasp_string_from_bytes(output, output_len);
    free(output);
    return result;
}

static void oasp_mdb_free_bound_values(char** values, unsigned int count) {
    unsigned int i = 0;
    if (!values) {
        return;
    }
    while (i < count) {
        free(values[i]);
        i++;
    }
    free(values);
}

static char* oasp_mdb_strdup_or_empty(const char* s) {
    size_t n = s ? strlen(s) : 0;
    char* out = (char*)malloc(n + 1);
    if (!out) {
        return NULL;
    }
    if (n) {
        memcpy(out, s, n);
    }
    out[n] = '\0';
    return out;
}

static char* oasp_mdb_errorf(const char* prefix, const char* detail) {
    const char* lhs = prefix ? prefix : "Error";
    const char* rhs = detail ? detail : "";
    size_t n1 = strlen(lhs);
    size_t n2 = strlen(rhs);
    char* out = (char*)malloc(n1 + 2 + n2 + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, lhs, n1);
    out[n1] = ':';
    out[n1 + 1] = ' ';
    memcpy(out + n1 + 2, rhs, n2);
    out[n1 + 2 + n2] = '\0';
    return out;
}

static int oasp_mdb_sb_reserve(EgMdbStringBuilder* sb, size_t extra) {
    size_t need = sb->len + extra + 1;
    char* next = NULL;
    size_t cap = sb->cap;
    if (need <= cap) {
        return 1;
    }
    if (cap == 0) {
        cap = 256;
    }
    while (cap < need) {
        cap *= 2;
    }
    next = (char*)realloc(sb->data, cap);
    if (!next) {
        return 0;
    }
    sb->data = next;
    sb->cap = cap;
    return 1;
}

static int oasp_mdb_sb_append_char(EgMdbStringBuilder* sb, char ch) {
    if (!oasp_mdb_sb_reserve(sb, 1)) {
        return 0;
    }
    sb->data[sb->len++] = ch;
    sb->data[sb->len] = '\0';
    return 1;
}

static int oasp_mdb_sb_append(EgMdbStringBuilder* sb, const char* text) {
    size_t len = text ? strlen(text) : 0;
    if (!oasp_mdb_sb_reserve(sb, len)) {
        return 0;
    }
    if (len) {
        memcpy(sb->data + sb->len, text, len);
        sb->len += len;
    }
    sb->data[sb->len] = '\0';
    return 1;
}

static char* oasp_mdb_sb_take(EgMdbStringBuilder* sb) {
    char* out = sb->data;
    if (!out) {
        out = oasp_mdb_strdup_or_empty("");
    }
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    return out;
}

static void oasp_mdb_sb_free(EgMdbStringBuilder* sb) {
    if (sb->data) {
        free(sb->data);
    }
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static char* oasp_mdb_expand_current_date_calls(const char* sql) {
    EgMdbStringBuilder out;
    char date_literal[32];
    time_t now;
    struct tm local_now;
    size_t i = 0;
    memset(&out, 0, sizeof(out));
    now = time(NULL);
    localtime_r(&now, &local_now);
    strftime(date_literal, sizeof(date_literal), "'%Y-%m-%d'", &local_now);
    while (sql && sql[i]) {
        size_t cursor = i;
        if (tolower((unsigned char)sql[cursor]) == 'd'
            && tolower((unsigned char)sql[cursor + 1]) == 'a'
            && tolower((unsigned char)sql[cursor + 2]) == 't'
            && tolower((unsigned char)sql[cursor + 3]) == 'e') {
            cursor += 4;
            while (sql[cursor] && isspace((unsigned char)sql[cursor])) {
                cursor++;
            }
            if (sql[cursor] == '(') {
                cursor++;
                while (sql[cursor] && isspace((unsigned char)sql[cursor])) {
                    cursor++;
                }
                if (sql[cursor] == ')') {
                    if (!oasp_mdb_sb_append(&out, date_literal)) {
                        oasp_mdb_sb_free(&out);
                        return NULL;
                    }
                    i = cursor + 1;
                    continue;
                }
            }
        }
        if (!oasp_mdb_sb_append_char(&out, sql[i])) {
            oasp_mdb_sb_free(&out);
            return NULL;
        }
        i++;
    }
    return oasp_mdb_sb_take(&out);
}

static int oasp_mdb_append_clean_cell(EgMdbStringBuilder* sb, const char* value) {
    const unsigned char* p = (const unsigned char*)(value ? value : "");
    while (*p) {
        unsigned char ch = *p++;
        if (ch == '\t' || ch == '\r' || ch == '\n') {
            if (!oasp_mdb_sb_append_char(sb, ' ')) {
                return 0;
            }
            continue;
        }
        if (ch == 0xff || ch == 0xfe) {
            continue;
        }
        if (!oasp_mdb_sb_append_char(sb, (char)ch)) {
            return 0;
        }
    }
    return 1;
}

static char oasp_mdb_variant_type_code(const MdbColumn* column) {
    if (!column) {
        return 's';
    }
    if (column->col_type == MDB_BOOL) {
        return 'b';
    }
    if (column->col_type == MDB_BYTE || column->col_type == MDB_INT || column->col_type == MDB_LONGINT) {
        return 'i';
    }
    if (column->col_type == MDB_MONEY || column->col_type == MDB_FLOAT || column->col_type == MDB_DOUBLE || column->col_type == MDB_NUMERIC) {
        return 'f';
    }
    if (column->col_type == MDB_DATETIME) {
        return 'd';
    }
    return 's';
}

static int oasp_mdb_append_typed_projection_header(EgMdbStringBuilder* out, MdbTableDef* table, const EgMdbSplitList* projection, const int* projection_indices) {
    size_t i = 0;
    while (i < projection->len) {
        if (!oasp_mdb_append_clean_cell(out, projection->items[i])
            || (i + 1 < projection->len && !oasp_mdb_sb_append_char(out, '\t'))) {
            return 0;
        }
        i++;
    }
    if (!oasp_mdb_sb_append(out, "\n#@egret-types:")) {
        return 0;
    }
    i = 0;
    while (i < projection->len) {
        MdbColumn* column = projection_indices[i] >= 0 ? g_ptr_array_index(table->columns, projection_indices[i]) : NULL;
        if (!oasp_mdb_sb_append_char(out, oasp_mdb_variant_type_code(column))
            || (i + 1 < projection->len && !oasp_mdb_sb_append_char(out, '\t'))) {
            return 0;
        }
        i++;
    }
    return oasp_mdb_sb_append_char(out, '\n');
}

static void oasp_mdb_close_table(MdbHandle* mdb, MdbTableDef* table) {
    if (table) {
        mdb_free_tabledef(table);
    }
    if (mdb) {
        mdb_close(mdb);
    }
}

static EgMdbSplitList oasp_mdb_split_sep(const char* text) {
    EgMdbSplitList out;
    const char* start = text ? text : "";
    const char* p = start;
    size_t count = 1;
    out.items = NULL;
    out.len = 0;
    while (*p) {
        if ((unsigned char)*p == OASP_MDB_SEP) {
            count++;
        }
        p++;
    }
    out.items = (char**)calloc(count, sizeof(char*));
    if (!out.items) {
        return out;
    }
    p = start;
    while (1) {
        const char* end = p;
        size_t n = 0;
        while (*end && (unsigned char)*end != OASP_MDB_SEP) {
            end++;
        }
        n = (size_t)(end - p);
        out.items[out.len] = (char*)malloc(n + 1);
        if (!out.items[out.len]) {
            break;
        }
        if (n) {
            memcpy(out.items[out.len], p, n);
        }
        out.items[out.len][n] = '\0';
        out.len++;
        if (!*end) {
            return out;
        }
        p = end + 1;
    }
    return out;
}

static void oasp_mdb_split_free(EgMdbSplitList* list) {
    size_t i = 0;
    if (!list || !list->items) {
        return;
    }
    while (i < list->len) {
        free(list->items[i]);
        i++;
    }
    free(list->items);
    list->items = NULL;
    list->len = 0;
}

static int oasp_mdb_decode_hex_item(char** item) {
    const char* source = item && *item ? *item : "";
    size_t source_len = strlen(source);
    char* decoded = NULL;
    size_t i = 0;
    if ((source_len & 1) != 0) {
        return 0;
    }
    decoded = (char*)malloc(source_len / 2 + 1);
    if (!decoded) {
        return 0;
    }
    while (i < source_len) {
        int high = oasp_asp_hex_nibble((unsigned char)source[i]);
        int low = oasp_asp_hex_nibble((unsigned char)source[i + 1]);
        if (high < 0 || low < 0 || (high == 0 && low == 0)) {
            free(decoded);
            return 0;
        }
        decoded[i / 2] = (char)((high << 4) | low);
        i += 2;
    }
    decoded[source_len / 2] = '\0';
    free(*item);
    *item = decoded;
    return 1;
}

static int oasp_mdb_decode_hex_list(EgMdbSplitList* list) {
    size_t i = 0;
    while (list && i < list->len) {
        if (!oasp_mdb_decode_hex_item(&list->items[i])) {
            return 0;
        }
        i++;
    }
    return list != NULL;
}

static char* oasp_mdb_decode_hex_string(const char* source) {
    char* value = oasp_mdb_strdup_or_empty(source);
    if (!value || !oasp_mdb_decode_hex_item(&value)) {
        free(value);
        return NULL;
    }
    return value;
}

static int oasp_mdb_column_index(MdbTableDef* table, const char* name) {
    unsigned int i = 0;
    if (!table || !table->columns || !name || !*name) {
        return -1;
    }
    while (i < table->num_cols) {
        MdbColumn* col = g_ptr_array_index(table->columns, i);
        if (col && !g_ascii_strcasecmp(col->name, name)) {
            return (int)i;
        }
        i++;
    }
    return -1;
}

static int oasp_mdb_column_is_indexed(MdbTableDef* table, int column_index) {
    unsigned int i = 0;
    if (!table || !table->indices || column_index < 0) {
        return 0;
    }
    while (i < table->num_idxs) {
        MdbIndex* index = g_ptr_array_index(table->indices, i);
        unsigned int key = 0;
        while (index && key < index->num_keys) {
            if (index->key_col_num[key] == column_index + 1) {
                return 1;
            }
            key++;
        }
        i++;
    }
    return 0;
}

static int oasp_mdb_update_table_counter(MdbTableDef* table, unsigned int offset, uint32_t value) {
    MdbHandle* mdb;
    if (!table || !table->entry || !(mdb = table->entry->mdb)
        || mdb_read_pg(mdb, table->entry->table_pg) != mdb->fmt->pg_size) {
        return 0;
    }
    mdb_put_int32(mdb->pg_buf, offset, value);
    return mdb_write_pg(mdb, table->entry->table_pg) == mdb->fmt->pg_size;
}

static int oasp_mdb_update_index_counters(MdbTableDef* table, uint32_t value) {
    MdbHandle* mdb;
    unsigned int i = 0;
    if (!table || !table->entry || !(mdb = table->entry->mdb)
        || mdb_read_pg(mdb, table->entry->table_pg) != mdb->fmt->pg_size) {
        return 0;
    }
    while (i < table->num_real_idxs) {
        mdb_put_int32(mdb->pg_buf,
            mdb->fmt->tab_cols_start_offset + i * mdb->fmt->tab_ridx_entry_size + 4,
            value);
        i++;
    }
    return mdb_write_pg(mdb, table->entry->table_pg) == mdb->fmt->pg_size;
}

/*
 * mdb_fetch_row() follows MEMO LVAL pages while resolving bound values and
 * leaves MdbHandle.pg_buf pointing at that LVAL page.  Do not use
 * mdb_find_row() after a fetch: it consults that mutable buffer.  Resolve a
 * row directly from its known data page instead.
 */
static int oasp_mdb_load_data_row(MdbTableDef* table, guint32 page_number, guint16 row_number, int* row_start, size_t* row_size) {
    MdbHandle* mdb;
    uint16_t row_count;
    uint16_t current;
    uint16_t following;
    if (!table || !table->entry || !(mdb = table->entry->mdb) || !row_start || !row_size
        || mdb_read_pg(mdb, page_number) != mdb->fmt->pg_size) {
        return 0;
    }
    row_count = (uint16_t)mdb_get_int16(mdb->pg_buf, mdb->fmt->row_count_offset);
    if (row_number >= row_count) {
        return 0;
    }
    current = (uint16_t)mdb_get_int16(mdb->pg_buf,
        mdb->fmt->row_count_offset + 2U + (unsigned int)row_number * 2U);
    if (current & 0x4000U) {
        return 0;
    }
    current &= OASP_MDB_OFFSET_MASK;
    following = row_number == 0 ? (uint16_t)mdb->fmt->pg_size
                                : (uint16_t)(mdb_get_int16(mdb->pg_buf,
                                    mdb->fmt->row_count_offset + (unsigned int)row_number * 2U)
                                    & OASP_MDB_OFFSET_MASK);
    if (current >= following || following > mdb->fmt->pg_size) {
        return 0;
    }
    *row_start = current;
    *row_size = (size_t)(following - current);
    return 1;
}

/*
 * mdbtools' public row decoder reads from its mutable page buffer and rejects
 * valid long-value rows after a bound MEMO fetch has visited an LVAL page.
 * Decode the Jet 4 row from the explicitly loaded data page instead.  This is
 * intentionally the Jet4 portion of mdb_crack_row, with all offsets bounded
 * relative to this row rather than the page.
 */
int oasp_mdb_decode_jet4_row(MdbTableDef* table, int row_start, size_t row_size, MdbField* fields) {
    MdbHandle* mdb;
    unsigned char* page;
    unsigned char* null_mask;
    unsigned int row_columns;
    unsigned int variable_columns;
    unsigned int fixed_columns;
    unsigned int fixed_found = 0;
    size_t row_end;
    size_t mask_size;
    size_t variable_table;
    size_t i;
    if (!table || !table->entry || !(mdb = table->entry->mdb) || !fields || row_start < 0
        || row_size < 3 || (size_t)row_start + row_size > (size_t)mdb->fmt->pg_size
        || mdb->fmt->pg_size != 4096) {
        return -1;
    }
    page = mdb->pg_buf;
    row_end = (size_t)row_start + row_size - 1;
    row_columns = mdb_get_int16(page, (unsigned int)row_start);
    mask_size = (row_columns + 7U) / 8U;
    if (row_columns == 0 || row_columns > 255 || mask_size + 4U > row_size) {
        return -1;
    }
    null_mask = page + row_end - mask_size + 1U;
    variable_columns = mdb_get_int16(page, (unsigned int)(row_end - mask_size - 1U));
    if (variable_columns > row_columns || variable_columns > 255
        || mask_size + 4U + (size_t)variable_columns * 2U > row_size) {
        return -1;
    }
    variable_table = row_end - mask_size - 3U;
    fixed_columns = row_columns - variable_columns;
    for (i = 0; i < table->num_cols; i++) {
        MdbColumn* column = g_ptr_array_index(table->columns, (guint)i);
        MdbField* field = &fields[i];
        unsigned int byte_number;
        unsigned int bit_number;
        size_t start;
        memset(field, 0, sizeof(*field));
        if (!column || column->col_num < 0) {
            return -1;
        }
        field->colnum = (int)i;
        field->is_fixed = column->is_fixed;
        byte_number = (unsigned int)column->col_num / 8U;
        bit_number = (unsigned int)column->col_num % 8U;
        field->is_null = byte_number >= mask_size
                       || !(null_mask[byte_number] & (unsigned char)(1U << bit_number));
        if (field->is_fixed && fixed_found < fixed_columns) {
            start = 2U + (size_t)column->fixed_offset;
            field->siz = column->col_size;
            fixed_found++;
        } else if (!field->is_fixed && column->var_col_num < variable_columns) {
            size_t variable_index = column->var_col_num;
            start = mdb_get_int16(page, (unsigned int)(variable_table - variable_index * 2U));
            size_t next = mdb_get_int16(page, (unsigned int)(variable_table - (variable_index + 1U) * 2U));
            if (next < start) {
                return -1;
            }
            field->siz = (int)(next - start);
        } else {
            field->is_null = 1;
            continue;
        }
        if (start > row_size || field->siz < 0 || (size_t)field->siz > row_size - start) {
            return -1;
        }
        field->start = row_start + (int)start;
        field->value = page + field->start;
    }
    return (int)row_columns;
}

static int oasp_mdb_index_page_is_empty(MdbHandle* mdb, guint32 page_number) {
    size_t i = OASP_MDB_INDEX_MASK_OFFSET;
    if (!page_number || mdb_read_pg(mdb, page_number) != mdb->fmt->pg_size
        || (mdb->pg_buf[0] != 3 && mdb->pg_buf[0] != 4)) {
        return -1;
    }
    while (i < OASP_MDB_INDEX_DATA_OFFSET) {
        if (mdb->pg_buf[i] != 0) {
            return 0;
        }
        i++;
    }
    return 1;
}

static size_t oasp_mdb_index_page_entries(const unsigned char* page, uint16_t* lengths, size_t capacity, size_t* total) {
    size_t boundary = 1;
    size_t previous = 0;
    size_t count = 0;
    while (boundary < OASP_MDB_INDEX_MASK_SIZE * 8) {
        if (page[OASP_MDB_INDEX_MASK_OFFSET + boundary / 8] & (unsigned char)(1U << (boundary % 8))) {
            if (count >= capacity || boundary <= previous || boundary > UINT16_MAX) {
                return SIZE_MAX;
            }
            lengths[count++] = (uint16_t)(boundary - previous);
            previous = boundary;
        }
        boundary++;
    }
    *total = previous;
    return count;
}

static void oasp_mdb_index_rebuild_mask(unsigned char* page, const uint16_t* lengths, size_t count) {
    size_t i = 0;
    size_t total = 0;
    memset(page + OASP_MDB_INDEX_MASK_OFFSET, 0, OASP_MDB_INDEX_MASK_SIZE);
    while (i < count) {
        total += lengths[i++];
        page[OASP_MDB_INDEX_MASK_OFFSET + total / 8] |= (unsigned char)(1U << (total % 8));
    }
    mdb_put_int16(page, 2, (unsigned int)(4096 - OASP_MDB_INDEX_DATA_OFFSET - total));
}

static void oasp_mdb_init_index_page(unsigned char* page, unsigned char type, guint32 table_page, guint32 previous, guint32 next) {
    memset(page, 0, 4096);
    page[0] = type;
    page[1] = 1;
    mdb_put_int16(page, 2, 4096 - OASP_MDB_INDEX_DATA_OFFSET);
    mdb_put_int32(page, 4, table_page);
    mdb_put_int32(page, 12, previous);
    mdb_put_int32(page, 16, next);
}

static int oasp_mdb_write_index_page(MdbHandle* mdb, guint32 page_number, const unsigned char* page) {
    memcpy(mdb->pg_buf, page, 4096);
    mdb->cur_pg = page_number;
    return mdb_write_pg(mdb, page_number) == mdb->fmt->pg_size;
}

static void oasp_mdb_make_long_index_entry(MdbField* field, guint32 page_number, guint16 row_number, unsigned char* entry) {
    guint32 page_row;
    entry[0] = 0x7f;
    mdb_index_swap_n((unsigned char*)field->value, 4, entry + 1);
    entry[1] |= 0x80;
    page_row = (page_number << 8) | ((row_number - 1) & 0xff);
    entry[5] = (unsigned char)((page_row >> 24) & 0xff);
    entry[6] = (unsigned char)((page_row >> 16) & 0xff);
    entry[7] = (unsigned char)((page_row >> 8) & 0xff);
    entry[8] = (unsigned char)(page_row & 0xff);
}

static int oasp_mdb_append_leaf_entry(unsigned char* page, const unsigned char* entry) {
    uint16_t lengths[512];
    size_t total = 0;
    size_t count = oasp_mdb_index_page_entries(page, lengths, sizeof(lengths) / sizeof(lengths[0]), &total);
    if (count == SIZE_MAX || page[0] != 4 || mdb_get_int16(page, 24) != 0
        || total + OASP_MDB_INDEX_LEAF_ENTRY_SIZE > 4096 - OASP_MDB_INDEX_DATA_OFFSET) {
        return 0;
    }
    if (count > 0 && (lengths[count - 1] != OASP_MDB_INDEX_LEAF_ENTRY_SIZE
        || memcmp(page + OASP_MDB_INDEX_DATA_OFFSET + total - OASP_MDB_INDEX_LEAF_ENTRY_SIZE,
                  entry, OASP_MDB_INDEX_LEAF_ENTRY_SIZE) >= 0)) {
        return 0;
    }
    memcpy(page + OASP_MDB_INDEX_DATA_OFFSET + total, entry, OASP_MDB_INDEX_LEAF_ENTRY_SIZE);
    lengths[count++] = OASP_MDB_INDEX_LEAF_ENTRY_SIZE;
    oasp_mdb_index_rebuild_mask(page, lengths, count);
    return 1;
}

static int oasp_mdb_split_root_leaf(MdbTableDef* table, MdbIndex* index, const unsigned char* entry) {
    MdbHandle* mdb = table->entry->mdb;
    unsigned char original[4096];
    unsigned char left[4096];
    unsigned char right[4096];
    unsigned char root[4096];
    unsigned char all_entries[OASP_MDB_INDEX_LEAF_ENTRY_SIZE * 403];
    uint16_t lengths[512];
    uint16_t leaf_lengths[512];
    size_t total = 0;
    size_t count;
    size_t left_count;
    size_t right_count;
    size_t i = 0;
    unsigned int left_page;
    unsigned int right_page;
    char allocation_error[256];
    memcpy(original, mdb->pg_buf, sizeof(original));
    count = oasp_mdb_index_page_entries(original, lengths, sizeof(lengths) / sizeof(lengths[0]), &total);
    if (count == SIZE_MAX || count == 0 || count + 1 > sizeof(all_entries) / OASP_MDB_INDEX_LEAF_ENTRY_SIZE) {
        return 0;
    }
    while (i < count) {
        size_t offset = i == 0 ? 0 : (size_t)i * OASP_MDB_INDEX_LEAF_ENTRY_SIZE;
        if (lengths[i] != OASP_MDB_INDEX_LEAF_ENTRY_SIZE) {
            return 0;
        }
        memcpy(all_entries + offset, original + OASP_MDB_INDEX_DATA_OFFSET + offset, OASP_MDB_INDEX_LEAF_ENTRY_SIZE);
        i++;
    }
    memcpy(all_entries + count * OASP_MDB_INDEX_LEAF_ENTRY_SIZE, entry, OASP_MDB_INDEX_LEAF_ENTRY_SIZE);
    count++;
    left_count = count / 2;
    right_count = count - left_count;
    if (!oasp_mdb_append_index_page(table, index, &left_page, allocation_error, sizeof(allocation_error))
        || !oasp_mdb_append_index_page(table, index, &right_page, allocation_error, sizeof(allocation_error))) {
        return 0;
    }
    oasp_mdb_init_index_page(left, 4, (guint32)table->entry->table_pg, 0, right_page);
    oasp_mdb_init_index_page(right, 4, (guint32)table->entry->table_pg, left_page, 0);
    memcpy(left + OASP_MDB_INDEX_DATA_OFFSET, all_entries, left_count * OASP_MDB_INDEX_LEAF_ENTRY_SIZE);
    memcpy(right + OASP_MDB_INDEX_DATA_OFFSET, all_entries + left_count * OASP_MDB_INDEX_LEAF_ENTRY_SIZE,
           right_count * OASP_MDB_INDEX_LEAF_ENTRY_SIZE);
    i = 0;
    while (i < left_count) {
        leaf_lengths[i++] = OASP_MDB_INDEX_LEAF_ENTRY_SIZE;
    }
    oasp_mdb_index_rebuild_mask(left, leaf_lengths, left_count);
    oasp_mdb_index_rebuild_mask(right, leaf_lengths, right_count);
    oasp_mdb_init_index_page(root, 3, (guint32)table->entry->table_pg, 0, 0);
    memcpy(root + OASP_MDB_INDEX_DATA_OFFSET,
           all_entries + (left_count - 1) * OASP_MDB_INDEX_LEAF_ENTRY_SIZE,
           OASP_MDB_INDEX_LEAF_ENTRY_SIZE);
    mdb_put_int32(root, OASP_MDB_INDEX_DATA_OFFSET + OASP_MDB_INDEX_LEAF_ENTRY_SIZE, left_page);
    leaf_lengths[0] = OASP_MDB_INDEX_NODE_ENTRY_SIZE;
    oasp_mdb_index_rebuild_mask(root, leaf_lengths, 1);
    mdb_put_int32(root, 20, right_page);
    return oasp_mdb_write_index_page(mdb, left_page, left)
        && oasp_mdb_write_index_page(mdb, right_page, right)
        && oasp_mdb_write_index_page(mdb, index->first_pg, root);
}

static int oasp_mdb_append_index_node(MdbTableDef* table, MdbIndex* index, const unsigned char* entry) {
    MdbHandle* mdb = table->entry->mdb;
    unsigned char root[4096];
    unsigned char leaf[4096];
    unsigned char new_leaf[4096];
    uint16_t root_lengths[512];
    uint16_t leaf_lengths[512];
    size_t root_total = 0;
    size_t leaf_total = 0;
    size_t root_count;
    size_t leaf_count;
    guint32 leaf_page;
    unsigned int new_page;
    char allocation_error[256];
    memcpy(root, mdb->pg_buf, sizeof(root));
    root_count = oasp_mdb_index_page_entries(root, root_lengths, sizeof(root_lengths) / sizeof(root_lengths[0]), &root_total);
    leaf_page = (guint32)mdb_get_int32(root, 20);
    if (root_count == SIZE_MAX || !leaf_page || mdb_read_pg(mdb, leaf_page) != mdb->fmt->pg_size
        || mdb->pg_buf[0] != 4 || mdb_get_int16(mdb->pg_buf, 24) != 0) {
        return 0;
    }
    memcpy(leaf, mdb->pg_buf, sizeof(leaf));
    if (oasp_mdb_append_leaf_entry(leaf, entry)) {
        return oasp_mdb_write_index_page(mdb, leaf_page, leaf);
    }
    leaf_count = oasp_mdb_index_page_entries(leaf, leaf_lengths, sizeof(leaf_lengths) / sizeof(leaf_lengths[0]), &leaf_total);
    if (leaf_count == SIZE_MAX || leaf_count == 0 || leaf_lengths[leaf_count - 1] != OASP_MDB_INDEX_LEAF_ENTRY_SIZE
        || root_total + OASP_MDB_INDEX_NODE_ENTRY_SIZE > 4096 - OASP_MDB_INDEX_DATA_OFFSET
        || !oasp_mdb_append_index_page(table, index, &new_page, allocation_error, sizeof(allocation_error))) {
        return 0;
    }
    mdb_put_int32(leaf, 16, new_page);
    oasp_mdb_init_index_page(new_leaf, 4, (guint32)table->entry->table_pg, leaf_page, 0);
    if (!oasp_mdb_append_leaf_entry(new_leaf, entry)) {
        return 0;
    }
    memcpy(root + OASP_MDB_INDEX_DATA_OFFSET + root_total,
           leaf + OASP_MDB_INDEX_DATA_OFFSET + leaf_total - OASP_MDB_INDEX_LEAF_ENTRY_SIZE,
           OASP_MDB_INDEX_LEAF_ENTRY_SIZE);
    mdb_put_int32(root, OASP_MDB_INDEX_DATA_OFFSET + root_total + OASP_MDB_INDEX_LEAF_ENTRY_SIZE, leaf_page);
    root_lengths[root_count++] = OASP_MDB_INDEX_NODE_ENTRY_SIZE;
    oasp_mdb_index_rebuild_mask(root, root_lengths, root_count);
    mdb_put_int32(root, 20, new_page);
    return oasp_mdb_write_index_page(mdb, leaf_page, leaf)
        && oasp_mdb_write_index_page(mdb, new_page, new_leaf)
        && oasp_mdb_write_index_page(mdb, index->first_pg, root);
}

/*
 * Check a UNIQUE LONGINT index before writing a data page.  The page writer
 * cannot rely on the journal to undo a direct mdbtools page write, so finding
 * a duplicate after the row was appended would leave an orphaned duplicate.
 */
static int oasp_mdb_unique_long_index_available(const char* db_path, MdbTableDef* table, MdbIndex* index, int num_fields, MdbField* fields) {
    MdbColumn* column;
    MdbField* field = NULL;
    gint32 candidate;
    char sql[1024];
    oasp_mdb_result result;
    char* end = NULL;
    long count;
    int i = 0;
    if (!index || !(index->flags & MDB_IDX_UNIQUE) || index->num_keys != 1
        || index->key_col_num[0] <= 0 || (unsigned int)index->key_col_num[0] > table->num_cols) {
        return 1;
    }
    column = g_ptr_array_index(table->columns, index->key_col_num[0] - 1);
    if (!column || !column->is_fixed || column->col_type != MDB_LONGINT || column->col_size != 4) {
        return 0;
    }
    /*
     * Existing Jet AutoNumber primary-key trees have a separate legacy write
     * path.  CREATE UNIQUE INDEX only accepts ordinary LONGINT columns, so
     * keep that compatibility path isolated from the new user-index check.
     */
    if (column->is_long_auto) {
        return 1;
    }
    while (i < num_fields) {
        if (fields[i].colnum == index->key_col_num[0] - 1) {
            field = &fields[i];
            break;
        }
        i++;
    }
    /* Jet UNIQUE indexes permit multiple NULL values. */
    if (!field || field->is_null) {
        return 1;
    }
    if (!field->value || field->siz != 4 || table->num_cols > 256) {
        return 0;
    }
    candidate = mdb_get_int32((unsigned char*)field->value, 0);
    for (;;) {
        if (snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM [%s] WHERE [%s] = %d",
                     table->name, column->name, candidate) >= (int)sizeof(sql)) {
            return 0;
        }
        memset(&result, 0, sizeof(result));
        if (!oasp_mdb_execute(db_path, sql, &result)
            || !result.row_count || !result.column_count || !result.cells[0].text) {
            oasp_mdb_result_dispose(&result);
            return 0;
        }
        errno = 0;
        count = strtol(result.cells[0].text, &end, 10);
        oasp_mdb_result_dispose(&result);
        if (errno || !end || *end) {
            return 0;
        }
        if (count == 0) {
            mdb_put_int32((unsigned char*)field->value, 0, candidate);
            return 1;
        }
        if (!column->is_long_auto || candidate == INT32_MAX) {
            return 0;
        }
        candidate++;
    }
}

static MdbField* oasp_mdb_find_index_field(int num_fields, MdbField* fields, int column_number) {
    int i = 0;
    while (i < num_fields) {
        if (fields[i].colnum == column_number) {
            return &fields[i];
        }
        i++;
    }
    return NULL;
}

static int oasp_mdb_index_value_equal(const MdbColumn* column, const MdbField* lhs, const MdbField* rhs) {
    size_t i = 0;
    if (!column || !lhs || !rhs || lhs->is_null || rhs->is_null || lhs->siz != rhs->siz
        || lhs->siz < 0 || (!lhs->value && lhs->siz) || (!rhs->value && rhs->siz)) {
        return 0;
    }
    /*
     * Jet text indexes use a case-insensitive collation.  This keeps the
     * engine's UNIQUE behavior consistent for ASCII text while retaining
     * byte-exact comparisons for the remaining native field encodings.
     */
    if (column->col_type == MDB_TEXT) {
        while (i < (size_t)lhs->siz) {
            unsigned char left = ((const unsigned char*)lhs->value)[i];
            unsigned char right = ((const unsigned char*)rhs->value)[i];
            if ((i & 1) == 0) {
                left = (unsigned char)tolower(left);
                right = (unsigned char)tolower(right);
            }
            if (left != right) {
                return 0;
            }
            i++;
        }
        return 1;
    }
    return lhs->siz == 0 || !memcmp(lhs->value, rhs->value, (size_t)lhs->siz);
}

static int oasp_mdb_unique_index_available(MdbTableDef* table, MdbIndex* index, int num_fields, MdbField* fields) {
    MdbField row_fields[256];
    int row_start;
    size_t row_size;
    unsigned int key;
    if (!index || !(index->flags & MDB_IDX_UNIQUE) || !index->num_keys || index->num_keys > MDB_MAX_IDX_COLS) {
        return 1;
    }
    for (key = 0; key < index->num_keys; key++) {
        MdbField* field;
        if (index->key_col_num[key] <= 0 || (unsigned int)index->key_col_num[key] > table->num_cols
            || !(field = oasp_mdb_find_index_field(num_fields, fields, index->key_col_num[key] - 1))
            || field->is_null) {
            /* Jet UNIQUE permits duplicate keys whenever any component is NULL. */
            return 1;
        }
    }
    table->strategy = MDB_TABLE_SCAN;
    mdb_rewind_table(table);
    while (mdb_fetch_row(table)) {
        int row_count;
        int equal = 1;
        if (!oasp_mdb_load_data_row(table, table->cur_phys_pg, (guint16)(table->cur_row - 1), &row_start, &row_size)) {
            return 0;
        }
        memset(row_fields, 0, sizeof(row_fields));
        row_count = oasp_mdb_decode_jet4_row(table, row_start, row_size, row_fields);
        if (row_count <= 0) {
            continue;
        }
        for (key = 0; key < index->num_keys; key++) {
            MdbColumn* column = g_ptr_array_index(table->columns, index->key_col_num[key] - 1);
            MdbField* candidate = oasp_mdb_find_index_field(num_fields, fields, index->key_col_num[key] - 1);
            MdbField* existing = oasp_mdb_find_index_field(row_count, row_fields, index->key_col_num[key] - 1);
            if (!oasp_mdb_index_value_equal(column, candidate, existing)) {
                equal = 0;
                break;
            }
        }
        if (equal) {
            return 0;
        }
    }
    return 1;
}

static int oasp_mdb_index_uses_long_entry(const MdbTableDef* table, const MdbIndex* index) {
    MdbColumn* column;
    if (!table || !index || index->num_keys != 1 || index->key_col_num[0] <= 0
        || (unsigned int)index->key_col_num[0] > table->num_cols) {
        return 0;
    }
    column = g_ptr_array_index(table->columns, index->key_col_num[0] - 1);
    return column && column->is_fixed && column->col_type == MDB_LONGINT && column->col_size == 4;
}

static int oasp_mdb_preflight_row_indexes(const char* db_path, MdbTableDef* table, int num_fields, MdbField* fields) {
    unsigned int i = 0;
    while (i < table->num_idxs) {
        MdbIndex* index = g_ptr_array_index(table->indices, i);
        if (index && index->index_type == 1 && (index->flags & MDB_IDX_UNIQUE)
            && ((oasp_mdb_index_uses_long_entry(table, index)
                 && !oasp_mdb_unique_long_index_available(db_path, table, index, num_fields, fields))
                || !oasp_mdb_unique_index_available(table, index, num_fields, fields))) {
            return 0;
        }
        i++;
    }
    return 1;
}

static int oasp_mdb_write_long_index_entry(MdbTableDef* table, MdbIndex* index, int num_fields, MdbField* fields, guint32 page_number,
                                           guint16 row_number) {
    MdbHandle* mdb = table->entry->mdb;
    MdbColumn* column;
    MdbField* field = NULL;
    unsigned char entry[OASP_MDB_INDEX_LEAF_ENTRY_SIZE];
    unsigned char root[4096];
    int i = 0;
    if (!index || index->num_keys != 1 || index->key_col_num[0] <= 0
        || (unsigned int)index->key_col_num[0] > table->num_cols) {
        return 0;
    }
    column = g_ptr_array_index(table->columns, index->key_col_num[0] - 1);
    if (!column || !column->is_fixed || column->col_type != MDB_LONGINT || column->col_size != 4) {
        return 0;
    }
    while (i < num_fields) {
        if (fields[i].colnum == index->key_col_num[0] - 1) {
            field = &fields[i];
            break;
        }
        i++;
    }
    if (!field || field->is_null || !field->value || field->siz != 4) {
        return 0;
    }
    if (oasp_mdb_index_page_is_empty(mdb, index->first_pg) < 0) {
        return 0;
    }
    oasp_mdb_make_long_index_entry(field, page_number, row_number, entry);
    /*
     * Jet marks PRIMARY KEY and UNIQUE indexes with MDB_IDX_UNIQUE.  The
     * index page writer used to accept duplicate keys, which left metadata
     * claiming a constraint that subsequent INSERT/UPDATE operations did not
     * actually enforce.
     */
    if (index->flags & MDB_IDX_UNIQUE) {
        guint32 leaf_page = index->first_pg;
        if (mdb->pg_buf[0] == 3) {
            uint16_t lengths[512];
            size_t total = 0;
            size_t count = oasp_mdb_index_page_entries(mdb->pg_buf, lengths,
                sizeof(lengths) / sizeof(lengths[0]), &total);
            if (count == SIZE_MAX || count == 0 || lengths[0] != OASP_MDB_INDEX_NODE_ENTRY_SIZE) {
                return 0;
            }
            leaf_page = (guint32)mdb_get_int32(mdb->pg_buf,
                OASP_MDB_INDEX_DATA_OFFSET + lengths[0] - 4);
        }
        while (leaf_page) {
            uint16_t lengths[512];
            size_t total = 0;
            size_t offset = 0;
            size_t count;
            size_t key = 0;
            if (mdb_read_pg(mdb, leaf_page) != mdb->fmt->pg_size || mdb->pg_buf[0] != 4) {
                return 0;
            }
            count = oasp_mdb_index_page_entries(mdb->pg_buf, lengths,
                sizeof(lengths) / sizeof(lengths[0]), &total);
            if (count == SIZE_MAX) {
                return 0;
            }
            while (key < count) {
                if (lengths[key] != OASP_MDB_INDEX_LEAF_ENTRY_SIZE) {
                    return 0;
                }
                if (!memcmp(mdb->pg_buf + OASP_MDB_INDEX_DATA_OFFSET + offset, entry, 5)) {
                    return 0;
                }
                offset += lengths[key++];
            }
            leaf_page = (guint32)mdb_get_int32(mdb->pg_buf, 20);
        }
        if (oasp_mdb_index_page_is_empty(mdb, index->first_pg) < 0) {
            return 0;
        }
    }
    memcpy(root, mdb->pg_buf, sizeof(root));
    if (root[0] == 3) {
        return oasp_mdb_append_index_node(table, index, entry);
    }
    if (root[0] != 4 || mdb_get_int16(root, 24) != 0) {
        return 0;
    }
    if (oasp_mdb_append_leaf_entry(root, entry)) {
        return oasp_mdb_write_index_page(mdb, index->first_pg, root);
    }
    memcpy(mdb->pg_buf, root, sizeof(root));
    return oasp_mdb_split_root_leaf(table, index, entry);
}

static int oasp_mdb_update_row_indexes(MdbTableDef* table, int num_fields, MdbField* fields, guint32 page_number, guint16 row_number) {
    unsigned int i = 0;
    while (i < table->num_idxs) {
        MdbIndex* index = g_ptr_array_index(table->indices, i);
        if (index && index->index_type == 1 && oasp_mdb_index_uses_long_entry(table, index)
            && !oasp_mdb_write_long_index_entry(table, index, num_fields, fields, page_number, row_number)) {
            return 0;
        }
        i++;
    }
    return 1;
}

int oasp_mdb_backfill_indexes(MdbTableDef* table) {
    MdbField fields[256];
    int row_start;
    size_t row_size;
    if (!table || !table->entry || !table->entry->mdb || table->num_cols > 256) {
        return 0;
    }
    mdb_rewind_table(table);
    while (mdb_fetch_row(table)) {
        int count;
        if (!oasp_mdb_load_data_row(table, table->cur_phys_pg, (guint16)(table->cur_row - 1),
                                    &row_start, &row_size)) {
            return 0;
        }
        memset(fields, 0, sizeof(fields));
        count = oasp_mdb_decode_jet4_row(table, row_start, row_size, fields);
        if (count <= 0 || (unsigned int)count != table->num_cols
            || !oasp_mdb_update_row_indexes(table, count, fields, table->cur_phys_pg,
                                            (guint16)(table->cur_row - 1))) {
            return 0;
        }
    }
    return 1;
}

static int oasp_mdb_remove_row_indexes(MdbTableDef* table, guint32 page_number, guint16 row_number) {
    unsigned int index_number = 0;
    guint32 target = (page_number << 8) | (row_number & 0xff);
    while (index_number < table->num_idxs) {
        MdbIndex* index = g_ptr_array_index(table->indices, index_number);
        if (index && index->index_type == 1) {
            MdbHandle* mdb = table->entry->mdb;
            unsigned char root[4096];
            unsigned char leaf[4096];
            uint16_t lengths[512];
            uint16_t root_lengths[512];
            size_t count;
            size_t root_count = 0;
            size_t total = 0;
            size_t root_total = 0;
            size_t found = SIZE_MAX;
            size_t start = 0;
            size_t i = 0;
            guint32 leaf_page;
            int root_is_node;
            if (!index->first_pg || mdb_read_pg(mdb, index->first_pg) != mdb->fmt->pg_size
                || (mdb->pg_buf[0] != 3 && mdb->pg_buf[0] != 4)) {
                return 0;
            }
            memcpy(root, mdb->pg_buf, sizeof(root));
            root_is_node = root[0] == 3;
            if (root_is_node) {
                root_count = oasp_mdb_index_page_entries(root, root_lengths,
                    sizeof(root_lengths) / sizeof(root_lengths[0]), &root_total);
                if (root_count == SIZE_MAX || root_count == 0 || root_lengths[0] != OASP_MDB_INDEX_NODE_ENTRY_SIZE) {
                    return 0;
                }
                leaf_page = (guint32)mdb_get_int32(root,
                    OASP_MDB_INDEX_DATA_OFFSET + root_lengths[0] - 4);
            } else {
                leaf_page = index->first_pg;
            }
            while (leaf_page) {
                size_t offset = 0;
                if (mdb_read_pg(mdb, leaf_page) != mdb->fmt->pg_size || mdb->pg_buf[0] != 4
                    || mdb_get_int16(mdb->pg_buf, 24) != 0) {
                    return 0;
                }
                memcpy(leaf, mdb->pg_buf, sizeof(leaf));
                count = oasp_mdb_index_page_entries(leaf, lengths, sizeof(lengths) / sizeof(lengths[0]), &total);
                if (count == SIZE_MAX) {
                    return 0;
                }
                i = 0;
                while (i < count) {
                    const unsigned char* entry = leaf + OASP_MDB_INDEX_DATA_OFFSET + offset;
                    guint32 row_id;
                    if (lengths[i] != OASP_MDB_INDEX_LEAF_ENTRY_SIZE) {
                        return 0;
                    }
                    row_id = ((guint32)entry[5] << 24) | ((guint32)entry[6] << 16)
                        | ((guint32)entry[7] << 8) | entry[8];
                    if (row_id == target) {
                        found = i;
                        start = offset;
                        break;
                    }
                    offset += lengths[i++];
                }
                if (found != SIZE_MAX) {
                    break;
                }
                leaf_page = (guint32)mdb_get_int32(leaf, 16);
            }
            if (found == SIZE_MAX) {
                return 0;
            }
            {
                size_t removed = lengths[found];
                int removed_last = found + 1 == count;
                memmove(leaf + OASP_MDB_INDEX_DATA_OFFSET + start,
                        leaf + OASP_MDB_INDEX_DATA_OFFSET + start + removed,
                        total - start - removed);
                memset(leaf + OASP_MDB_INDEX_DATA_OFFSET + total - removed, 0, removed);
                i = 0;
                while (i + 1 < count) {
                    lengths[i] = OASP_MDB_INDEX_LEAF_ENTRY_SIZE;
                    i++;
                }
                oasp_mdb_index_rebuild_mask(leaf, lengths, count - 1);
                if (!oasp_mdb_write_index_page(mdb, leaf_page, leaf)) {
                    return 0;
                }
                if (root_is_node && removed_last && count > 1) {
                    size_t root_offset = 0;
                    i = 0;
                    while (i < root_count) {
                        guint32 child;
                        if (root_lengths[i] != OASP_MDB_INDEX_NODE_ENTRY_SIZE) {
                            return 0;
                        }
                        child = (guint32)mdb_get_int32(root,
                            OASP_MDB_INDEX_DATA_OFFSET + root_offset + OASP_MDB_INDEX_LEAF_ENTRY_SIZE);
                        if (child == leaf_page) {
                            memcpy(root + OASP_MDB_INDEX_DATA_OFFSET + root_offset,
                                   leaf + OASP_MDB_INDEX_DATA_OFFSET
                                       + (count - 2) * OASP_MDB_INDEX_LEAF_ENTRY_SIZE,
                                   OASP_MDB_INDEX_LEAF_ENTRY_SIZE);
                            if (!oasp_mdb_write_index_page(mdb, index->first_pg, root)) {
                                return 0;
                            }
                            break;
                        }
                        root_offset += root_lengths[i++];
                    }
                }
            }
        }
        index_number++;
    }
    return 1;
}

static guint16 oasp_mdb_add_row_preserving_flags(MdbTableDef* table, const unsigned char* row_buffer, int row_size) {
    MdbHandle* mdb = table->entry->mdb;
    unsigned char* new_page = (unsigned char*)mdb_new_data_pg(table->entry);
    int row_count = mdb_get_int16(mdb->pg_buf, mdb->fmt->row_count_offset);
    int position = mdb->fmt->pg_size;
    int i = 0;
    if (!new_page) {
        return 0;
    }
    while (i < row_count) {
        int source;
        size_t existing_size;
        uint16_t raw_offset;
        if (mdb_find_row(mdb, i, &source, &existing_size) != 0
            || existing_size > (size_t)position) {
            g_free(new_page);
            return 0;
        }
        raw_offset = (uint16_t)mdb_get_int16(mdb->pg_buf,
            mdb->fmt->row_count_offset + 2 + i * 2);
        position -= (int)existing_size;
        memcpy(new_page + position, mdb->pg_buf + (source & OASP_MDB_OFFSET_MASK), existing_size);
        mdb_put_int16(new_page, mdb->fmt->row_count_offset + 2 + i * 2,
            (unsigned int)position | (raw_offset & (uint16_t)~OASP_MDB_OFFSET_MASK));
        i++;
    }
    position -= row_size;
    memcpy(new_page + position, row_buffer, (size_t)row_size);
    mdb_put_int16(new_page, mdb->fmt->row_count_offset + 2 + row_count * 2, (unsigned int)position);
    row_count++;
    mdb_put_int16(new_page, mdb->fmt->row_count_offset, (unsigned int)row_count);
    mdb_put_int16(new_page, 2,
        (unsigned int)(position - mdb->fmt->row_count_offset - 2 - row_count * 2));
    memcpy(mdb->pg_buf, new_page, (size_t)mdb->fmt->pg_size);
    g_free(new_page);
    return (guint16)row_count;
}

/*
 * MDBTools' public mdb_pack_row() writes variable values in MdbField array
 * order, but Jet's offset table is indexed by MdbColumn.var_col_num. Those
 * orders differ when a schema interleaves MEMO columns with ordinary TEXT
 * columns: Jet assigns long-value columns after the inline variable columns.
 * Pack Jet 4 rows from the physical metadata so INSERT and UPDATE cannot move
 * values into neighboring columns. Jet 3 keeps the upstream implementation
 * because its jump-table encoding is format-specific.
 */
static int oasp_mdb_pack_row_preserving_layout(MdbTableDef* table, unsigned char* row_buffer, size_t row_capacity, unsigned int num_fields, MdbField* fields) {
    MdbHandle* mdb = table ? table->entry->mdb : NULL;
    MdbField* fields_by_column[256];
    unsigned int variable_offsets[256];
    unsigned int fixed_end = 2;
    unsigned int position;
    unsigned int variable_count = 0;
    unsigned int null_mask_size;
    unsigned int i;
    if (!table || !mdb || !row_buffer || !fields || num_fields == 0
        || num_fields > table->num_cols || num_fields > 256) {
        return 0;
    }
    if (IS_JET3(mdb)) {
        return mdb_pack_row(table, row_buffer, num_fields, fields);
    }
    memset(row_buffer, 0, row_capacity);
    memset(fields_by_column, 0, sizeof(fields_by_column));
    memset(variable_offsets, 0, sizeof(variable_offsets));
    i = 0;
    while (i < num_fields) {
        if (fields[i].colnum < 0 || (unsigned int)fields[i].colnum >= num_fields
            || fields_by_column[fields[i].colnum]) {
            return 0;
        }
        fields_by_column[fields[i].colnum] = &fields[i];
        i++;
    }
    i = 0;
    while (i < num_fields) {
        MdbColumn* column = g_ptr_array_index(table->columns, i);
        MdbField* field = fields_by_column[i];
        if (!column || !field || column->col_num < 0 || (unsigned int)column->col_num >= num_fields) {
            return 0;
        }
        if (column->is_fixed && column->col_type != MDB_BOOL) {
            unsigned int end = 2 + column->fixed_offset + column->col_size;
            if (end > fixed_end) {
                fixed_end = end;
            }
        } else if (!column->is_fixed) {
            if (column->var_col_num >= 256 || variable_offsets[column->var_col_num] != 0) {
                return 0;
            }
            variable_count++;
        }
        i++;
    }
    null_mask_size = (num_fields + 7) / 8;
    if (fixed_end + 4 + variable_count * 2 + null_mask_size > row_capacity) {
        return 0;
    }
    row_buffer[0] = (unsigned char)(num_fields & 0xff);
    row_buffer[1] = (unsigned char)((num_fields >> 8) & 0xff);
    i = 0;
    while (i < num_fields) {
        MdbColumn* column = g_ptr_array_index(table->columns, i);
        MdbField* field = fields_by_column[i];
        if (column->is_fixed && column->col_type != MDB_BOOL) {
            if (!field->is_null && (field->siz != column->col_size || !field->value)) {
                return 0;
            }
            if (!field->is_null) {
                memcpy(row_buffer + 2 + column->fixed_offset, field->value, (size_t)field->siz);
            }
        }
        i++;
    }
    position = fixed_end;
    i = 0;
    while (i < variable_count) {
        unsigned int column_index = 0;
        MdbColumn* column = NULL;
        MdbField* field = NULL;
        while (column_index < num_fields) {
            column = g_ptr_array_index(table->columns, column_index);
            if (!column->is_fixed && column->var_col_num == i) {
                field = fields_by_column[column_index];
                break;
            }
            column_index++;
        }
        if (!field || field->siz < 0 || (!field->is_null && field->siz > 0 && !field->value)
            || position + (unsigned int)(field->is_null ? 0 : field->siz)
                + 4 + variable_count * 2 + null_mask_size > row_capacity) {
            return 0;
        }
        variable_offsets[i] = position;
        if (!field->is_null && field->siz > 0) {
            memcpy(row_buffer + position, field->value, (size_t)field->siz);
            position += (unsigned int)field->siz;
        }
        i++;
    }
    row_buffer[position] = (unsigned char)(position & 0xff);
    row_buffer[position + 1] = (unsigned char)((position >> 8) & 0xff);
    position += 2;
    i = variable_count;
    while (i > 0) {
        unsigned int offset = variable_offsets[--i];
        row_buffer[position++] = (unsigned char)(offset & 0xff);
        row_buffer[position++] = (unsigned char)((offset >> 8) & 0xff);
    }
    row_buffer[position++] = (unsigned char)(variable_count & 0xff);
    row_buffer[position++] = (unsigned char)((variable_count >> 8) & 0xff);
    i = 0;
    while (i < num_fields) {
        MdbColumn* column = g_ptr_array_index(table->columns, i);
        MdbField* field = fields_by_column[i];
        if (!field->is_null) {
            row_buffer[position + column->col_num / 8] |= (unsigned char)(1u << (column->col_num % 8));
        }
        i++;
    }
    position += null_mask_size;
    return (int)position;
}

static int oasp_mdb_insert_packed_row(const char* db_path, MdbTableDef* table, int num_fields, MdbField* fields) {
    MdbHandle* mdb = table->entry->mdb;
    unsigned char row_buffer[4096];
    int row_size = oasp_mdb_pack_row_preserving_layout(table, row_buffer, sizeof(row_buffer), (unsigned int)num_fields, fields);
    guint32 cursor = 0;
    gint32 page_number = 0;
    guint16 row_number;
    int result;
    if (row_size <= 0 || row_size >= (int)sizeof(row_buffer)) {
        return 0;
    }
    while ((page_number = mdb_map_find_next(mdb, table->free_usage_map, (unsigned int)table->freemap_sz, cursor)) > 0) {
        int rows;
        if (mdb_read_pg(mdb, (unsigned long)page_number) != mdb->fmt->pg_size) {
            return 0;
        }
        rows = mdb_get_int16(mdb->pg_buf, mdb->fmt->row_count_offset);
        if (rows == 0 || mdb_pg_get_freespace(mdb) >= row_size + 2) {
            break;
        }
        cursor = (guint32)page_number;
    }
    if (page_number <= 0) {
        char allocation_error[256];
        if (!oasp_mdb_append_table_data_page(table, allocation_error, sizeof(allocation_error))) {
            return 0;
        }
        page_number = mdb_map_find_next(mdb, table->free_usage_map, (unsigned int)table->freemap_sz, cursor);
        if (page_number <= 0 || mdb_read_pg(mdb, (unsigned long)page_number) != mdb->fmt->pg_size) {
            return 0;
        }
    }
    if (!oasp_mdb_preflight_row_indexes(db_path, table, num_fields, fields)
        || mdb_read_pg(mdb, (unsigned long)page_number) != mdb->fmt->pg_size) {
        return 0;
    }
    row_number = oasp_mdb_add_row_preserving_flags(table, row_buffer, row_size);
    if (row_number == 0) {
        return 0;
    }
    if (mdb_write_pg(mdb, (unsigned long)page_number) != mdb->fmt->pg_size) {
        return 0;
    }
    result = oasp_mdb_update_row_indexes(table, num_fields, fields, (guint32)page_number, row_number);
    if (result != 1) {
        return 0;
    }
    if (table->num_real_idxs > 0
        && !oasp_mdb_update_index_counters(table, table->num_rows + 1)) {
        return 0;
    }
    table->num_rows++;
    return oasp_mdb_update_table_counter(table, table->entry->mdb->fmt->tab_num_rows_offset, table->num_rows);
}

int oasp_mdb_replace_fields(MdbTableDef* table, int row_number, int num_fields, MdbField* fields) {
    unsigned char row_buffer[4096];
    int row_size = oasp_mdb_pack_row_preserving_layout(table, row_buffer, sizeof(row_buffer),
                                                     (unsigned int)num_fields, fields);
    return row_size > 0 && row_size < (int)sizeof(row_buffer)
        && mdb_replace_row(table, row_number, row_buffer, row_size) == 0;
}

static int oasp_mdb_parse_number(const char* text, double* value) {
    char* end = NULL;
    double parsed = 0.0;
    if (!text || !*text) {
        return 0;
    }
    if (!g_ascii_strcasecmp(text, "true")) {
        *value = 1.0;
        return 1;
    }
    if (!g_ascii_strcasecmp(text, "false")) {
        *value = 0.0;
        return 1;
    }
    parsed = strtod(text, &end);
    while (end && *end && isspace((unsigned char)*end)) {
        end++;
    }
    if (!end || *end) {
        return 0;
    }
    *value = parsed;
    return 1;
}

static int oasp_mdb_compare_values(const char* lhs, const char* rhs) {
    double lhs_number = 0.0;
    double rhs_number = 0.0;
    if (oasp_mdb_parse_number(lhs, &lhs_number) && oasp_mdb_parse_number(rhs, &rhs_number)) {
        return lhs_number < rhs_number ? -1 : (lhs_number > rhs_number ? 1 : 0);
    }
    return g_ascii_strcasecmp(lhs ? lhs : "", rhs ? rhs : "");
}

static int oasp_mdb_contains_fold(const char* text, const char* needle) {
    char* lower_text = g_ascii_strdown(text ? text : "", -1);
    char* lower_needle = g_ascii_strdown(needle ? needle : "", -1);
    int found = lower_text && lower_needle && strstr(lower_text, lower_needle) != NULL;
    g_free(lower_text);
    g_free(lower_needle);
    return found;
}

static int oasp_mdb_parse_int64(const char* text, int64_t minimum, int64_t maximum, int64_t* output) {
    char* end = NULL;
    long long value;
    errno = 0;
    value = strtoll(text ? text : "", &end, 10);
    while (end && *end && isspace((unsigned char)*end)) {
        end++;
    }
    if (errno != 0 || !end || end == text || *end || value < minimum || value > maximum) {
        return 0;
    }
    *output = (int64_t)value;
    return 1;
}

static int oasp_mdb_parse_double_value(const char* text, double* output) {
    char* end = NULL;
    double value;
    errno = 0;
    value = strtod(text ? text : "", &end);
    while (end && *end && isspace((unsigned char)*end)) {
        end++;
    }
    if (errno != 0 || !end || end == text || *end) {
        return 0;
    }
    *output = value;
    return 1;
}

static int oasp_mdb_parse_datetime(const char* text, double* output) {
    struct tm parsed;
    const char* value = text ? text : "";
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int matched = 0;
    time_t now;
    if (!g_ascii_strcasecmp(value, "date()") || !g_ascii_strcasecmp(value, "now()") || !g_ascii_strcasecmp(value, "getdate()")) {
        now = time(NULL);
        localtime_r(&now, &parsed);
        if (!g_ascii_strcasecmp(value, "date()")) {
            parsed.tm_hour = 0;
            parsed.tm_min = 0;
            parsed.tm_sec = 0;
        }
        mdb_tm_to_date(&parsed, output);
        return 1;
    }
    matched = sscanf(value, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);
    if (matched < 3) {
        matched = sscanf(value, "%d/%d/%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);
    }
    if (matched < 3 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return 0;
    }
    memset(&parsed, 0, sizeof(parsed));
    parsed.tm_year = year - 1900;
    parsed.tm_mon = month - 1;
    parsed.tm_mday = day;
    parsed.tm_hour = hour;
    parsed.tm_min = minute;
    parsed.tm_sec = second;
    mdb_tm_to_date(&parsed, output);
    return 1;
}

static void oasp_mdb_put_int64_le(void* buffer, int64_t value) {
    unsigned char* bytes = (unsigned char*)buffer;
    uint64_t bits = (uint64_t)value;
    unsigned int i = 0;
    while (i < 8) {
        bytes[i] = (unsigned char)((bits >> (i * 8)) & 0xff);
        i++;
    }
}

/*
 * Convert a normalized SQL literal into the exact byte representation expected
 * by MDBTools. owned_value records heap storage that the caller must release;
 * NULL and AutoNumber use field flags rather than textual sentinel values.
 */
static int oasp_mdb_literal_to_field(MdbHandle* mdb, MdbColumn* column, const char* literal, MdbField* field, char** owned_value) {
    const char kind = literal && *literal ? literal[0] : '\0';
    const char* value = kind ? literal + 1 : "";
    int64_t integer = 0;
    double real = 0.0;
    size_t capacity = 0;
    int encoded_len = 0;
    *owned_value = NULL;
    field->colnum = column->col_num;
    field->is_fixed = column->is_fixed;
    field->is_null = kind == 'n';
    field->value = NULL;
    field->siz = 0;
    if (kind == 'n') {
        if (column->col_type == MDB_BOOL) {
            return 0;
        }
        return 1;
    }
    if (kind != 's' && kind != 'v' && kind != 'd') {
        return 0;
    }
    switch (column->col_type) {
        case MDB_BOOL:
            if (!g_ascii_strcasecmp(value, "true") || !strcmp(value, "1") || !strcmp(value, "-1")) {
                field->is_null = 0;
                return 1;
            }
            if (!g_ascii_strcasecmp(value, "false") || !strcmp(value, "0")) {
                field->is_null = 1;
                return 1;
            }
            return 0;
        case MDB_BYTE:
            if (!oasp_mdb_parse_int64(value, 0, UINT8_MAX, &integer)) {
                return 0;
            }
            *owned_value = (char*)malloc(1);
            if (*owned_value) {
                (*owned_value)[0] = (char)integer;
            }
            field->siz = 1;
            break;
        case MDB_INT:
            if (!oasp_mdb_parse_int64(value, INT16_MIN, INT16_MAX, &integer)) {
                return 0;
            }
            *owned_value = (char*)malloc(2);
            if (*owned_value) {
                mdb_put_int16(*owned_value, 0, (uint16_t)integer);
            }
            field->siz = 2;
            break;
        case MDB_LONGINT:
            if (!oasp_mdb_parse_int64(value, INT32_MIN, INT32_MAX, &integer)) {
                return 0;
            }
            *owned_value = (char*)malloc(4);
            if (*owned_value) {
                mdb_put_int32(*owned_value, 0, (uint32_t)integer);
            }
            field->siz = 4;
            break;
        case MDB_FLOAT: {
            float single;
            if (!oasp_mdb_parse_double_value(value, &real)) {
                return 0;
            }
            single = (float)real;
            *owned_value = (char*)malloc(sizeof(single));
            if (*owned_value) {
                memcpy(*owned_value, &single, sizeof(single));
            }
            field->siz = (int)sizeof(single);
            break;
        }
        case MDB_DOUBLE:
            if (!oasp_mdb_parse_double_value(value, &real)) {
                return 0;
            }
            *owned_value = (char*)malloc(sizeof(real));
            if (*owned_value) {
                memcpy(*owned_value, &real, sizeof(real));
            }
            field->siz = (int)sizeof(real);
            break;
        case MDB_DATETIME:
            if (!oasp_mdb_parse_datetime(value, &real)) {
                return 0;
            }
            *owned_value = (char*)malloc(sizeof(real));
            if (*owned_value) {
                memcpy(*owned_value, &real, sizeof(real));
            }
            field->siz = (int)sizeof(real);
            break;
        case MDB_MONEY:
            if (!oasp_mdb_parse_double_value(value, &real) || real > 922337203685477.5807 || real < -922337203685477.5807) {
                return 0;
            }
            integer = (int64_t)(real * 10000.0 + (real >= 0.0 ? 0.5 : -0.5));
            *owned_value = (char*)malloc(8);
            if (*owned_value) {
                oasp_mdb_put_int64_le(*owned_value, integer);
            }
            field->siz = 8;
            break;
        case MDB_TEXT:
            capacity = strlen(value) * 4 + 4;
            *owned_value = (char*)calloc(capacity, 1);
            if (*owned_value) {
                encoded_len = mdb_ascii2unicode(mdb, value, strlen(value), *owned_value, capacity);
            }
            if (!*owned_value || encoded_len < 0 || encoded_len > column->col_size) {
                free(*owned_value);
                *owned_value = NULL;
                return 0;
            }
            field->siz = encoded_len;
            break;
        case MDB_MEMO:
            capacity = strlen(value) * 4 + MDB_MEMO_OVERHEAD + 4;
            *owned_value = (char*)calloc(capacity, 1);
            if (*owned_value) {
                encoded_len = mdb_ascii2unicode(mdb, value, strlen(value), *owned_value + MDB_MEMO_OVERHEAD, capacity - MDB_MEMO_OVERHEAD);
            }
            if (!*owned_value || encoded_len < 0) {
                free(*owned_value);
                *owned_value = NULL;
                return 0;
            }
            mdb_put_int32(*owned_value, 0, UINT32_C(0x80000000) | (uint32_t)encoded_len);
            field->siz = MDB_MEMO_OVERHEAD + encoded_len;
            break;
        default:
            return 0;
    }
    field->value = *owned_value;
    return *owned_value != NULL;
}

int oasp_mdb_convert_literal_field(MdbHandle* mdb, MdbColumn* column, const char* literal, MdbField* field, char** owned_value) {
    return oasp_mdb_literal_to_field(mdb, column, literal, field, owned_value);
}

static int oasp_mdb_externalize_long_values(MdbTableDef* table, MdbField* fields, char** owned_values, size_t count, char* error,
                                            size_t error_capacity) {
    for (size_t i = 0; i < count; i++) {
        MdbColumn* column = g_ptr_array_index(table->columns, i);
        unsigned char descriptor[MDB_MEMO_OVERHEAD];
        char* replacement;
        if (!column || column->col_type != MDB_MEMO || fields[i].is_null
            || fields[i].siz <= MDB_MEMO_OVERHEAD + 1024 || !fields[i].value) {
            continue;
        }
        replacement = malloc(MDB_MEMO_OVERHEAD);
        if (!replacement
            || !oasp_mdb_store_long_value(table, i,
                                          (const unsigned char*)fields[i].value + MDB_MEMO_OVERHEAD,
                                          (size_t)fields[i].siz - MDB_MEMO_OVERHEAD, descriptor,
                                          error, error_capacity)) {
            free(replacement);
            return 0;
        }
        memcpy(replacement, descriptor, sizeof(descriptor));
        free(owned_values[i]);
        owned_values[i] = replacement;
        fields[i].value = replacement;
        fields[i].siz = MDB_MEMO_OVERHEAD;
    }
    return 1;
}

static char* oasp_mdb_property_literal(const char* expression) {
    const char* start = expression;
    const char* end;
    char* literal;
    size_t length;
    size_t output = 1;
    char quote;
    if (!start) {
        return NULL;
    }
    while (isspace((unsigned char)*start)) {
        start++;
    }
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    length = (size_t)(end - start);
    if (length == 0) {
        return NULL;
    }
    quote = *start;
    if ((quote == '\'' || quote == '"') && length >= 2 && end[-1] == quote) {
        const char* cursor = start + 1;
        literal = (char*)malloc(length);
        if (!literal) {
            return NULL;
        }
        literal[0] = 's';
        while (cursor < end - 1) {
            literal[output++] = *cursor;
            if (*cursor == quote && cursor + 1 < end - 1 && cursor[1] == quote) {
                cursor++;
            }
            cursor++;
        }
        literal[output] = '\0';
        return literal;
    }
    literal = (char*)malloc(length + 2);
    if (!literal) {
        return NULL;
    }
    if ((*start == '#' && end[-1] == '#')
        || (length == 5 && !g_ascii_strncasecmp(start, "now()", length))
        || (length == 6 && !g_ascii_strncasecmp(start, "date()", length))
        || (length == 9 && !g_ascii_strncasecmp(start, "getdate()", length))) {
        literal[0] = 'd';
        if (*start == '#') {
            start++;
            length -= 2;
        }
    } else if (length == 4 && !g_ascii_strncasecmp(start, "null", length)) {
        literal[0] = 'n';
    } else {
        literal[0] = 'v';
    }
    memcpy(literal + 1, start, length);
    literal[length + 1] = '\0';
    return literal;
}

static int oasp_mdb_property_is_true(const char* value) {
    return value && (!g_ascii_strcasecmp(value, "yes") || !g_ascii_strcasecmp(value, "true")
                     || !strcmp(value, "1") || !strcmp(value, "-1"));
}

static uint64_t oasp_mdb_row_hash(char** values, unsigned int count, const char* salt) {
    uint64_t hash = UINT64_C(1469598103934665603);
    unsigned int i = 0;
    const unsigned char* p = (const unsigned char*)(salt ? salt : "");
    while (*p) {
        hash = (hash ^ *p++) * UINT64_C(1099511628211);
    }
    while (i < count) {
        p = (const unsigned char*)(values[i] ? values[i] : "");
        while (*p) {
            hash = (hash ^ *p++) * UINT64_C(1099511628211);
        }
        hash = (hash ^ OASP_MDB_SEP) * UINT64_C(1099511628211);
        i++;
    }
    return hash;
}

static void oasp_mdb_free_query_rows(EgMdbQueryRow* rows, size_t row_count, unsigned int column_count) {
    size_t i = 0;
    while (i < row_count) {
        oasp_mdb_free_bound_values(rows[i].values, column_count);
        i++;
    }
    free(rows);
}

static int oasp_mdb_append_aggregate(EgMdbStringBuilder* out, EgMdbQueryRow* rows, size_t row_count, EgMdbSplitList* projection, int* projection_indices,
                                     int aggregate_kind) {
    const char* aggregate_name = aggregate_kind == 1 ? "COUNT" : (aggregate_kind == 2 ? "MAX" : (aggregate_kind == 3 ? "MIN" : (aggregate_kind == 4 ? "SUM" : "AVG")));
    const char* column_name = aggregate_kind == 1 ? "*" : projection->items[0];
    char value_buffer[128];
    size_t i = 0;
    size_t numeric_count = 0;
    double numeric_total = 0.0;
    const char* extremum = NULL;
    if (!oasp_mdb_sb_append(out, aggregate_name) || !oasp_mdb_sb_append_char(out, '(') || !oasp_mdb_sb_append(out, column_name) || !oasp_mdb_sb_append(out, ")\n")) {
        return 0;
    }
    if (aggregate_kind == 1) {
        snprintf(value_buffer, sizeof(value_buffer), "%zu", row_count);
        return oasp_mdb_sb_append(out, value_buffer) && oasp_mdb_sb_append_char(out, '\n');
    }
    while (i < row_count) {
        const char* value = rows[i].values[projection_indices[0]];
        if (value && *value) {
            if (aggregate_kind == 2 || aggregate_kind == 3) {
                if (!extremum || (aggregate_kind == 2 ? oasp_mdb_compare_values(value, extremum) > 0 : oasp_mdb_compare_values(value, extremum) < 0)) {
                    extremum = value;
                }
            } else {
                double number = 0.0;
                if (oasp_mdb_parse_number(value, &number)) {
                    numeric_total += number;
                    numeric_count++;
                }
            }
        }
        i++;
    }
    if (aggregate_kind == 2 || aggregate_kind == 3) {
        if (extremum && !oasp_mdb_append_clean_cell(out, extremum)) {
            return 0;
        }
    } else if (numeric_count > 0) {
        double result = aggregate_kind == 5 ? numeric_total / (double)numeric_count : numeric_total;
        snprintf(value_buffer, sizeof(value_buffer), "%.15g", result);
        if (!oasp_mdb_sb_append(out, value_buffer)) {
            return 0;
        }
    }
    return oasp_mdb_sb_append_char(out, '\n');
}

/*
 * Scan one table under a shared lock, bind typed columns, filter/project rows,
 * then apply DISTINCT/order/TOP/aggregate semantics before serialization.
 * Every row retained for sorting owns its bound strings.
 */
static char* oasp_mdb_query_raw(const char* db_path, const char* table_name, const char* select_columns, int where_kind, const char* where_column,
                                const char* where_value, const char* order_column, int order_desc, int top_count, int distinct, int aggregate_kind) {
    MdbHandle* mdb = NULL;
    MdbTableDef* table = NULL;
    EgMdbStringBuilder out;
    EgMdbSplitList projection;
    EgMdbQueryRow* rows = NULL;
    char** bound_values = NULL;
    int* bound_lens = NULL;
    int* projection_indices = NULL;
    size_t row_count = 0;
    size_t row_capacity = 0;
    size_t output_count = 0;
    size_t i = 0;
    unsigned int column_count = 0;
    int where_index = -1;
    int order_index = -1;
    int random_order = 0;
    memset(&out, 0, sizeof(out));
    memset(&projection, 0, sizeof(projection));
    if (!db_path || !table_name || !*db_path || !*table_name) {
        return oasp_mdb_errorf("Error", "invalid MDB query arguments");
    }
    mdb = mdb_open(db_path, MDB_NOFLAGS);
    if (!mdb) {
        return oasp_mdb_errorf("Error", "unable to open MDB file");
    }
    mdb_set_bind_size(mdb, OASP_MDB_BIND_SIZE);
    table = mdb_read_table_by_name(mdb, (char*)table_name, MDB_TABLE);
    if (!table || !mdb_read_columns(table)) {
        oasp_mdb_close_table(mdb, table);
        return oasp_mdb_errorf("Error", "unable to read MDB table");
    }
    mdb_rewind_table(table);
    column_count = table->num_cols;
    bound_values = (char**)calloc(column_count, sizeof(char*));
    bound_lens = (int*)calloc(column_count, sizeof(int));
    if (!bound_values || !bound_lens) {
        oasp_mdb_close_table(mdb, table);
        free(bound_lens);
        oasp_mdb_free_bound_values(bound_values, column_count);
        return oasp_mdb_errorf("Error", "out of memory");
    }
    i = 0;
    while (i < column_count) {
        bound_values[i] = (char*)calloc(OASP_MDB_BIND_SIZE + 1, 1);
        if (!bound_values[i] || mdb_bind_column(table, (int)i + 1, bound_values[i], &bound_lens[i]) == -1) {
            oasp_mdb_close_table(mdb, table);
            free(bound_lens);
            oasp_mdb_free_bound_values(bound_values, column_count);
            return oasp_mdb_errorf("Error", "unable to bind MDB column");
        }
        i++;
    }
    if (select_columns && *select_columns) {
        projection = oasp_mdb_split_sep(select_columns);
    }
    if (projection.len == 0) {
        projection.items = (char**)calloc(column_count, sizeof(char*));
        while (projection.len < column_count) {
            MdbColumn* col = g_ptr_array_index(table->columns, projection.len);
            projection.items[projection.len] = oasp_mdb_strdup_or_empty(col ? col->name : "");
            projection.len++;
        }
    }
    projection_indices = (int*)calloc(projection.len, sizeof(int));
    i = 0;
    while (i < projection.len) {
        projection_indices[i] = oasp_mdb_column_index(table, projection.items[i]);
        if (projection_indices[i] < 0) {
            oasp_mdb_close_table(mdb, table);
            free(bound_lens);
            oasp_mdb_free_bound_values(bound_values, column_count);
            oasp_mdb_split_free(&projection);
            free(projection_indices);
            return oasp_mdb_errorf("Error", "SELECT column not found");
        }
        i++;
    }
    if (where_kind != 0) {
        where_index = oasp_mdb_column_index(table, where_column);
        if (where_index < 0) {
            oasp_mdb_close_table(mdb, table);
            free(bound_lens);
            oasp_mdb_free_bound_values(bound_values, column_count);
            oasp_mdb_split_free(&projection);
            free(projection_indices);
            return oasp_mdb_errorf("Error", "WHERE column not found");
        }
    }
    random_order = order_column && g_ascii_strncasecmp(order_column, "rnd", 3) == 0;
    if (order_column && *order_column && !random_order) {
        order_index = oasp_mdb_column_index(table, order_column);
        if (order_index < 0) {
            oasp_mdb_close_table(mdb, table);
            free(bound_lens);
            oasp_mdb_free_bound_values(bound_values, column_count);
            oasp_mdb_split_free(&projection);
            free(projection_indices);
            return oasp_mdb_errorf("Error", "ORDER BY column not found");
        }
    }
    if (aggregate_kind == 0 && !oasp_mdb_append_typed_projection_header(&out, table, &projection, projection_indices)) {
        oasp_mdb_close_table(mdb, table);
        free(bound_lens);
        oasp_mdb_free_bound_values(bound_values, column_count);
        oasp_mdb_split_free(&projection);
        free(projection_indices);
        oasp_mdb_sb_free(&out);
        return oasp_mdb_errorf("Error", "out of memory");
    }
    while (mdb_fetch_row(table)) {
        int comparison = 0;
        int matches = 1;
        if (where_kind != 0) {
            const char* actual = bound_values[where_index] ? bound_values[where_index] : "";
            const char* expected = where_value ? where_value : "";
            comparison = oasp_mdb_compare_values(actual, expected);
            matches = where_kind == 1 ? comparison == 0
                : (where_kind == 2 ? comparison > 0
                : (where_kind == 3 ? comparison < 0
                : oasp_mdb_contains_fold(actual, expected)));
        }
        if (!matches) {
            continue;
        }
        if (row_count == row_capacity) {
            size_t next_capacity = row_capacity == 0 ? 16 : row_capacity * 2;
            EgMdbQueryRow* next_rows = (EgMdbQueryRow*)realloc(rows, next_capacity * sizeof(EgMdbQueryRow));
            if (!next_rows) {
                oasp_mdb_close_table(mdb, table);
                free(bound_lens);
                oasp_mdb_free_bound_values(bound_values, column_count);
                oasp_mdb_split_free(&projection);
                free(projection_indices);
                oasp_mdb_free_query_rows(rows, row_count, column_count);
                return oasp_mdb_errorf("Error", "out of memory");
            }
            rows = next_rows;
            row_capacity = next_capacity;
        }
        rows[row_count].values = (char**)calloc(column_count, sizeof(char*));
        if (!rows[row_count].values) {
            break;
        }
        i = 0;
        while (i < column_count) {
            rows[row_count].values[i] = oasp_mdb_strdup_or_empty(bound_values[i]);
            if (!rows[row_count].values[i]) {
                break;
            }
            i++;
        }
        if (i != column_count) {
            oasp_mdb_free_bound_values(rows[row_count].values, column_count);
            break;
        }
        rows[row_count].random_key = oasp_mdb_row_hash(rows[row_count].values, column_count, order_column);
        row_count++;
    }
    oasp_mdb_close_table(mdb, table);
    free(bound_lens);
    oasp_mdb_free_bound_values(bound_values, column_count);
    if (order_index >= 0 || random_order) {
        i = 1;
        while (i < row_count) {
            EgMdbQueryRow moving = rows[i];
            size_t j = i;
            while (j > 0) {
                int cmp = random_order
                    ? (rows[j - 1].random_key < moving.random_key ? -1 : (rows[j - 1].random_key > moving.random_key ? 1 : 0))
                    : oasp_mdb_compare_values(rows[j - 1].values[order_index], moving.values[order_index]);
                if (order_desc) {
                    cmp = -cmp;
                }
                if (cmp <= 0) {
                    break;
                }
                rows[j] = rows[j - 1];
                j--;
            }
            rows[j] = moving;
            i++;
        }
    }
    output_count = top_count > 0 && (size_t)top_count < row_count ? (size_t)top_count : row_count;
    if (aggregate_kind != 0) {
        if ((aggregate_kind > 1 && projection.len != 1) || !oasp_mdb_append_aggregate(&out, rows, row_count, &projection, projection_indices, aggregate_kind)) {
            oasp_mdb_sb_free(&out);
        }
    } else {
        i = 0;
        while (i < output_count) {
            size_t p = 0;
            int duplicate = 0;
            if (distinct) {
                size_t previous = 0;
                while (previous < i && !duplicate) {
                    p = 0;
                    duplicate = 1;
                    while (p < projection.len) {
                        if (strcmp(rows[previous].values[projection_indices[p]], rows[i].values[projection_indices[p]])) {
                            duplicate = 0;
                            break;
                        }
                        p++;
                    }
                    previous++;
                }
            }
            if (!duplicate) {
                p = 0;
                while (p < projection.len) {
                    oasp_mdb_append_clean_cell(&out, rows[i].values[projection_indices[p]]);
                    if (p + 1 < projection.len) {
                        oasp_mdb_sb_append_char(&out, '\t');
                    }
                    p++;
                }
                oasp_mdb_sb_append_char(&out, '\n');
            }
            i++;
        }
    }
    oasp_mdb_split_free(&projection);
    free(projection_indices);
    oasp_mdb_free_query_rows(rows, row_count, column_count);
    return oasp_mdb_sb_take(&out);
}

static char* oasp_mdb_sql_query_raw(const char* db_path, const char* table_name, const char* select_columns, const char* where_sql, const char* order_column,
                                    int order_desc, int top_count, int distinct, int aggregate_kind) {
    MdbSQL* sql = NULL;
    MdbTableDef* table = NULL;
    EgMdbStringBuilder query;
    EgMdbStringBuilder out;
    EgMdbSplitList projection;
    EgMdbQueryRow* rows = NULL;
    int* projection_indices = NULL;
    size_t row_count = 0;
    size_t row_capacity = 0;
    size_t output_count = 0;
    size_t i = 0;
    unsigned int bound_offset = 0;
    unsigned int column_count = 0;
    int order_index = -1;
    int random_order = 0;
    char* expanded_query = NULL;
    char* error = NULL;
    memset(&query, 0, sizeof(query));
    memset(&out, 0, sizeof(out));
    memset(&projection, 0, sizeof(projection));
    if (!db_path || !table_name || !*db_path || !*table_name || !where_sql || !*where_sql) {
        return oasp_mdb_errorf("Error", "invalid MDB SQL query arguments");
    }
    sql = mdb_sql_init();
    if (!sql || !mdb_sql_open(sql, (char*)db_path)) {
        error = oasp_mdb_errorf("Error", sql ? mdb_sql_last_error(sql) : "unable to initialize MDB SQL");
        if (sql) {
            mdb_sql_exit(sql);
        }
        return error;
    }
    mdb_set_bind_size(sql->mdb, OASP_MDB_BIND_SIZE);
    if (!oasp_mdb_sb_append(&query, "SELECT * FROM ") || !oasp_mdb_sb_append(&query, table_name) || !oasp_mdb_sb_append(&query, " WHERE ") || !oasp_mdb_sb_append(&query, where_sql)) {
        oasp_mdb_sb_free(&query);
        mdb_sql_exit(sql);
        return oasp_mdb_errorf("Error", "out of memory");
    }
    expanded_query = oasp_mdb_expand_current_date_calls(query.data);
    mdb_sql_run_query(sql, expanded_query ? expanded_query : query.data);
    free(expanded_query);
    oasp_mdb_sb_free(&query);
    if (mdb_sql_has_error(sql) || !sql->cur_table) {
        error = oasp_mdb_errorf("Error", mdb_sql_has_error(sql) ? mdb_sql_last_error(sql) : "MDB SQL query returned no table");
        mdb_sql_exit(sql);
        return error;
    }
    table = sql->cur_table;
    column_count = table->num_cols;
    /*
     * mdb_sql_run_query() already binds every selected column. Binding a
     * second time allocates another bind_size buffer per column and leaves
     * the first set unused for the lifetime of the query.
     */
    bound_offset = 0;
    if (select_columns && *select_columns) {
        projection = oasp_mdb_split_sep(select_columns);
    }
    if (projection.len == 0) {
        projection.items = (char**)calloc(column_count, sizeof(char*));
        while (projection.items && projection.len < column_count) {
            MdbColumn* col = g_ptr_array_index(table->columns, projection.len);
            projection.items[projection.len] = oasp_mdb_strdup_or_empty(col ? col->name : "");
            if (!projection.items[projection.len]) {
                break;
            }
            projection.len++;
        }
    }
    if (projection.len == 0) {
        oasp_mdb_split_free(&projection);
        mdb_sql_exit(sql);
        return oasp_mdb_errorf("Error", "unable to resolve MDB SQL projection");
    }
    projection_indices = (int*)calloc(projection.len, sizeof(int));
    if (!projection_indices) {
        oasp_mdb_split_free(&projection);
        mdb_sql_exit(sql);
        return oasp_mdb_errorf("Error", "out of memory");
    }
    i = 0;
    while (i < projection.len) {
        projection_indices[i] = oasp_mdb_column_index(table, projection.items[i]);
        if (projection_indices[i] < 0) {
            free(projection_indices);
            oasp_mdb_split_free(&projection);
            mdb_sql_exit(sql);
            return oasp_mdb_errorf("Error", "SELECT column not found");
        }
        i++;
    }
    random_order = order_column && g_ascii_strncasecmp(order_column, "rnd", 3) == 0;
    if (order_column && *order_column && !random_order) {
        order_index = oasp_mdb_column_index(table, order_column);
        if (order_index < 0) {
            free(projection_indices);
            oasp_mdb_split_free(&projection);
            mdb_sql_exit(sql);
            return oasp_mdb_errorf("Error", "ORDER BY column not found");
        }
    }
    if (aggregate_kind == 0 && !oasp_mdb_append_typed_projection_header(&out, table, &projection, projection_indices)) {
        free(projection_indices);
        oasp_mdb_split_free(&projection);
        mdb_sql_exit(sql);
        oasp_mdb_sb_free(&out);
        return oasp_mdb_errorf("Error", "out of memory");
    }
    while (mdb_sql_fetch_row(sql, table)) {
        if (row_count == row_capacity) {
            size_t next_capacity = row_capacity == 0 ? 16 : row_capacity * 2;
            EgMdbQueryRow* next_rows = (EgMdbQueryRow*)realloc(rows, next_capacity * sizeof(EgMdbQueryRow));
            if (!next_rows) {
                break;
            }
            rows = next_rows;
            row_capacity = next_capacity;
        }
        rows[row_count].values = (char**)calloc(column_count, sizeof(char*));
        if (!rows[row_count].values) {
            break;
        }
        i = 0;
        while (i < column_count) {
            const char* value = bound_offset + i < sql->bound_values->len ? (const char*)g_ptr_array_index(sql->bound_values, bound_offset + i) : "";
            rows[row_count].values[i] = oasp_mdb_strdup_or_empty(value);
            if (!rows[row_count].values[i]) {
                break;
            }
            i++;
        }
        if (i != column_count) {
            oasp_mdb_free_bound_values(rows[row_count].values, column_count);
            break;
        }
        rows[row_count].random_key = oasp_mdb_row_hash(rows[row_count].values, column_count, order_column);
        row_count++;
    }
    mdb_sql_exit(sql);
    if (order_index >= 0 || random_order) {
        i = 1;
        while (i < row_count) {
            EgMdbQueryRow moving = rows[i];
            size_t j = i;
            while (j > 0) {
                int cmp = random_order
                    ? (rows[j - 1].random_key < moving.random_key ? -1 : (rows[j - 1].random_key > moving.random_key ? 1 : 0))
                    : oasp_mdb_compare_values(rows[j - 1].values[order_index], moving.values[order_index]);
                if (order_desc) {
                    cmp = -cmp;
                }
                if (cmp <= 0) {
                    break;
                }
                rows[j] = rows[j - 1];
                j--;
            }
            rows[j] = moving;
            i++;
        }
    }
    output_count = top_count > 0 && (size_t)top_count < row_count ? (size_t)top_count : row_count;
    if (aggregate_kind != 0) {
        if ((aggregate_kind > 1 && projection.len != 1) || !oasp_mdb_append_aggregate(&out, rows, row_count, &projection, projection_indices, aggregate_kind)) {
            oasp_mdb_sb_free(&out);
        }
    } else {
        i = 0;
        while (i < output_count) {
            size_t p = 0;
            int duplicate = 0;
            if (distinct) {
                size_t previous = 0;
                while (previous < i && !duplicate) {
                    p = 0;
                    duplicate = 1;
                    while (p < projection.len) {
                        if (strcmp(rows[previous].values[projection_indices[p]], rows[i].values[projection_indices[p]])) {
                            duplicate = 0;
                            break;
                        }
                        p++;
                    }
                    previous++;
                }
            }
            if (!duplicate) {
                p = 0;
                while (p < projection.len) {
                    oasp_mdb_append_clean_cell(&out, rows[i].values[projection_indices[p]]);
                    if (p + 1 < projection.len) {
                        oasp_mdb_sb_append_char(&out, '\t');
                    }
                    p++;
                }
                oasp_mdb_sb_append_char(&out, '\n');
            }
            i++;
        }
    }
    oasp_mdb_split_free(&projection);
    free(projection_indices);
    oasp_mdb_free_query_rows(rows, row_count, column_count);
    return oasp_mdb_sb_take(&out);
}

static char* oasp_mdb_table_dump_raw(const char* db_path, const char* table_name) {
    MdbHandle* mdb = NULL;
    MdbTableDef* table = NULL;
    EgMdbStringBuilder out;
    char** bound_values = NULL;
    int* bound_lens = NULL;
    unsigned int* column_numbers = NULL;
    unsigned int i = 0;
    unsigned int num_cols = 0;
    unsigned int source_column_count = 0;
    memset(&out, 0, sizeof(out));

    if (!db_path || !table_name || !*db_path || !*table_name) {
        return oasp_mdb_errorf("Error", "invalid MDB query arguments");
    }
    mdb = mdb_open(db_path, MDB_NOFLAGS);
    if (!mdb) {
        return oasp_mdb_errorf("Error", "unable to open MDB file");
    }
    mdb_set_bind_size(mdb, OASP_MDB_BIND_SIZE);
    table = mdb_read_table_by_name(mdb, (char*)table_name, MDB_TABLE);
    if (!table) {
        oasp_mdb_close_table(mdb, table);
        return oasp_mdb_errorf("Error", "access table not found");
    }
    if (!mdb_read_columns(table)) {
        oasp_mdb_close_table(mdb, table);
        return oasp_mdb_errorf("Error", "unable to read table columns");
    }
    mdb_rewind_table(table);
    source_column_count = table->num_cols;
    column_numbers = (unsigned int*)calloc(source_column_count, sizeof(unsigned int));
    if (!column_numbers) {
        oasp_mdb_close_table(mdb, table);
        return oasp_mdb_errorf("Error", "out of memory");
    }
    while (i < source_column_count) {
        MdbColumn* col = g_ptr_array_index(table->columns, i);
        if (col && *col->name) {
            column_numbers[num_cols++] = i;
        }
        i++;
    }
    if (num_cols == 0) {
        free(column_numbers);
        oasp_mdb_close_table(mdb, table);
        return oasp_mdb_errorf("Error", "table has no readable columns");
    }
    bound_values = (char**)calloc(num_cols, sizeof(char*));
    bound_lens = (int*)calloc(num_cols, sizeof(int));
    if (!bound_values || !bound_lens) {
        oasp_mdb_close_table(mdb, table);
        oasp_mdb_free_bound_values(bound_values, num_cols);
        free(bound_lens);
        free(column_numbers);
        return oasp_mdb_errorf("Error", "out of memory");
    }
    while (i < num_cols) {
        MdbColumn* col = g_ptr_array_index(table->columns, column_numbers[i]);
        size_t buf_size = OASP_MDB_BIND_SIZE + 1;
        bound_values[i] = (char*)calloc(buf_size, 1);
        if (!bound_values[i]) {
            oasp_mdb_close_table(mdb, table);
            free(bound_lens);
            oasp_mdb_free_bound_values(bound_values, num_cols);
            free(column_numbers);
            return oasp_mdb_errorf("Error", "out of memory");
        }
        if (mdb_bind_column(table, (int)column_numbers[i] + 1, bound_values[i], &bound_lens[i]) == -1) {
            oasp_mdb_close_table(mdb, table);
            free(bound_lens);
            oasp_mdb_free_bound_values(bound_values, num_cols);
            free(column_numbers);
            return oasp_mdb_errorf("Error", "unable to bind table column");
        }
        if (!oasp_mdb_append_clean_cell(&out, col->name)) {
            oasp_mdb_close_table(mdb, table);
            free(bound_lens);
            oasp_mdb_free_bound_values(bound_values, num_cols);
            free(column_numbers);
            oasp_mdb_sb_free(&out);
            return oasp_mdb_errorf("Error", "out of memory");
        }
        if (i + 1 < num_cols && !oasp_mdb_sb_append_char(&out, '\t')) {
            oasp_mdb_close_table(mdb, table);
            free(bound_lens);
            oasp_mdb_free_bound_values(bound_values, num_cols);
            free(column_numbers);
            oasp_mdb_sb_free(&out);
            return oasp_mdb_errorf("Error", "out of memory");
        }
        i++;
    }
    if (!oasp_mdb_sb_append_char(&out, '\n')) {
        oasp_mdb_close_table(mdb, table);
        free(bound_lens);
        oasp_mdb_free_bound_values(bound_values, num_cols);
        free(column_numbers);
        oasp_mdb_sb_free(&out);
        return oasp_mdb_errorf("Error", "out of memory");
    }
    while (mdb_fetch_row(table)) {
        i = 0;
        while (i < num_cols) {
            if (!oasp_mdb_append_clean_cell(&out, bound_values[i] ? bound_values[i] : "")) {
                oasp_mdb_close_table(mdb, table);
                free(bound_lens);
                oasp_mdb_free_bound_values(bound_values, num_cols);
                free(column_numbers);
                oasp_mdb_sb_free(&out);
                return oasp_mdb_errorf("Error", "out of memory");
            }
            if (i + 1 < num_cols && !oasp_mdb_sb_append_char(&out, '\t')) {
                oasp_mdb_close_table(mdb, table);
                free(bound_lens);
                oasp_mdb_free_bound_values(bound_values, num_cols);
                free(column_numbers);
                oasp_mdb_sb_free(&out);
                return oasp_mdb_errorf("Error", "out of memory");
            }
            i++;
        }
        if (!oasp_mdb_sb_append_char(&out, '\n')) {
            oasp_mdb_close_table(mdb, table);
            free(bound_lens);
            oasp_mdb_free_bound_values(bound_values, num_cols);
            free(column_numbers);
            oasp_mdb_sb_free(&out);
            return oasp_mdb_errorf("Error", "out of memory");
        }
    }
    oasp_mdb_close_table(mdb, table);
    free(bound_lens);
    oasp_mdb_free_bound_values(bound_values, num_cols);
    free(column_numbers);
    return oasp_mdb_sb_take(&out);
}

typedef struct EgMdbDump {
    EgMdbSplitList columns;
    char*** rows;
    size_t row_count;
} EgMdbDump;

static void oasp_mdb_dump_free(EgMdbDump* dump) {
    size_t i = 0;
    if (!dump) {
        return;
    }
    while (i < dump->row_count) {
        oasp_mdb_split_free(&(EgMdbSplitList){dump->rows[i], dump->columns.len});
        i++;
    }
    free(dump->rows);
    dump->rows = NULL;
    dump->row_count = 0;
    oasp_mdb_split_free(&dump->columns);
}

static int oasp_mdb_dump_read(EgMdbDump* dump, const char* db_path, const char* table_name) {
    MdbHandle* mdb = NULL;
    MdbTableDef* table = NULL;
    char** bound_values = NULL;
    int* bound_lens = NULL;
    unsigned int* column_numbers = NULL;
    unsigned int source_column_count = 0;
    unsigned int column_count = 0;
    unsigned int i = 0;
    memset(dump, 0, sizeof(*dump));
    if (!db_path || !table_name || !*db_path || !*table_name) {
        return 0;
    }
    mdb = mdb_open(db_path, MDB_NOFLAGS);
    if (!mdb) {
        return 0;
    }
    mdb_set_bind_size(mdb, OASP_MDB_BIND_SIZE);
    table = mdb_read_table_by_name(mdb, (char*)table_name, MDB_TABLE);
    if (!table || !mdb_read_columns(table)) {
        oasp_mdb_close_table(mdb, table);
        return 0;
    }
    source_column_count = table->num_cols;
    column_numbers = (unsigned int*)calloc(source_column_count, sizeof(unsigned int));
    if (!column_numbers) {
        oasp_mdb_close_table(mdb, table);
        return 0;
    }
    while (i < source_column_count) {
        MdbColumn* col = g_ptr_array_index(table->columns, i);
        if (col && *col->name) {
            column_numbers[column_count++] = i;
        }
        i++;
    }
    if (column_count == 0) {
        free(column_numbers);
        oasp_mdb_close_table(mdb, table);
        return 0;
    }
    dump->columns.items = (char**)calloc(column_count, sizeof(char*));
    bound_values = (char**)calloc(column_count, sizeof(char*));
    bound_lens = (int*)calloc(column_count, sizeof(int));
    if (!dump->columns.items || !bound_values || !bound_lens) {
        free(column_numbers);
        free(bound_lens);
        oasp_mdb_free_bound_values(bound_values, column_count);
        oasp_mdb_close_table(mdb, table);
        oasp_mdb_dump_free(dump);
        return 0;
    }
    i = 0;
    while (i < column_count) {
        MdbColumn* col = g_ptr_array_index(table->columns, column_numbers[i]);
        dump->columns.items[i] = oasp_mdb_strdup_or_empty(col->name);
        bound_values[i] = (char*)calloc(OASP_MDB_BIND_SIZE + 1, 1);
        if (!dump->columns.items[i] || !bound_values[i] || mdb_bind_column(table, (int)column_numbers[i] + 1, bound_values[i], &bound_lens[i]) == -1) {
            free(column_numbers);
            free(bound_lens);
            oasp_mdb_free_bound_values(bound_values, column_count);
            oasp_mdb_close_table(mdb, table);
            oasp_mdb_dump_free(dump);
            return 0;
        }
        dump->columns.len++;
        i++;
    }
    mdb_rewind_table(table);
    while (mdb_fetch_row(table)) {
        char*** rows = NULL;
        char** values = (char**)calloc(column_count, sizeof(char*));
        if (!values) {
            break;
        }
        i = 0;
        while (i < column_count) {
            values[i] = oasp_mdb_strdup_or_empty(bound_values[i]);
            if (!values[i]) {
                oasp_mdb_free_bound_values(values, column_count);
                values = NULL;
                break;
            }
            i++;
        }
        if (!values) {
            break;
        }
        rows = (char***)realloc(dump->rows, (dump->row_count + 1) * sizeof(char**));
        if (!rows) {
            oasp_mdb_free_bound_values(values, column_count);
            break;
        }
        dump->rows = rows;
        dump->rows[dump->row_count++] = values;
    }
    free(column_numbers);
    free(bound_lens);
    oasp_mdb_free_bound_values(bound_values, column_count);
    oasp_mdb_close_table(mdb, table);
    if (i != column_count) {
        oasp_mdb_dump_free(dump);
        return 0;
    }
    return 1;
}

static const char* oasp_mdb_column_suffix(const char* name) {
    const char* dot = strrchr(name ? name : "", '.');
    return dot ? dot + 1 : (name ? name : "");
}

static int oasp_mdb_name_targets_table(const char* name, const char* table_name) {
    const char* dot = strchr(name ? name : "", '.');
    size_t table_len = table_name ? strlen(table_name) : 0;
    return !dot || ((size_t)(dot - name) == table_len && !g_ascii_strncasecmp(name, table_name, table_len));
}

static int oasp_mdb_dump_column_index(const EgMdbDump* dump, const char* name) {
    const char* suffix = oasp_mdb_column_suffix(name);
    size_t i = 0;
    while (dump && i < dump->columns.len) {
        if (!g_ascii_strcasecmp(dump->columns.items[i], suffix)) {
            return (int)i;
        }
        i++;
    }
    return -1;
}

static char* oasp_mdb_join_columns_error(const EgMdbDump* left, const EgMdbDump* right) {
    EgMdbStringBuilder out;
    size_t i = 0;
    memset(&out, 0, sizeof(out));
    if (!oasp_mdb_sb_append(&out, "JOIN column not found; left columns: ")) {
        oasp_mdb_sb_free(&out);
        return oasp_mdb_errorf("Error", "out of memory");
    }
    while (left && i < left->columns.len) {
        if (!oasp_mdb_sb_append(&out, left->columns.items[i]) || (i + 1 < left->columns.len && !oasp_mdb_sb_append_char(&out, ','))) {
            oasp_mdb_sb_free(&out);
            return oasp_mdb_errorf("Error", "out of memory");
        }
        i++;
    }
    if (!oasp_mdb_sb_append(&out, "; right columns: ")) {
        oasp_mdb_sb_free(&out);
        return oasp_mdb_errorf("Error", "out of memory");
    }
    i = 0;
    while (right && i < right->columns.len) {
        if (!oasp_mdb_sb_append(&out, right->columns.items[i]) || (i + 1 < right->columns.len && !oasp_mdb_sb_append_char(&out, ','))) {
            oasp_mdb_sb_free(&out);
            return oasp_mdb_errorf("Error", "out of memory");
        }
        i++;
    }
    {
        char* detail = oasp_mdb_sb_take(&out);
        char* error = oasp_mdb_errorf("Error", detail);
        free(detail);
        return error;
    }
}

static char* oasp_mdb_join_query_raw(const char* db_path, const char* left_table, const char* right_table, const char* select_columns, int where_kind,
                                     const char* where_column, const char* where_value, const char* join_left_column, const char* join_right_column, int top_count, int distinct) {
    EgMdbDump left;
    EgMdbDump right;
    EgMdbSplitList projection;
    EgMdbStringBuilder out;
    int left_join_index = -1;
    int right_join_index = -1;
    int where_left_index = -1;
    int where_right_index = -1;
    size_t output_count = 0;
    size_t i = 0;
    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    memset(&projection, 0, sizeof(projection));
    memset(&out, 0, sizeof(out));
    if (!oasp_mdb_dump_read(&left, db_path, left_table)) {
        oasp_mdb_dump_free(&left);
        oasp_mdb_dump_free(&right);
        return oasp_mdb_errorf("Error", "unable to read left MDB JOIN table");
    }
    if (!oasp_mdb_dump_read(&right, db_path, right_table)) {
        oasp_mdb_dump_free(&left);
        oasp_mdb_dump_free(&right);
        return oasp_mdb_errorf("Error", "unable to read right MDB JOIN table");
    }
    projection = oasp_mdb_split_sep(select_columns ? select_columns : "");
    if (projection.len == 0) {
        oasp_mdb_dump_free(&left);
        oasp_mdb_dump_free(&right);
        return oasp_mdb_errorf("Error", "JOIN requires explicit SELECT columns");
    }
    left_join_index = oasp_mdb_dump_column_index(&left, oasp_mdb_name_targets_table(join_left_column, left_table) ? join_left_column : join_right_column);
    right_join_index = oasp_mdb_dump_column_index(&right, oasp_mdb_name_targets_table(join_left_column, right_table) ? join_left_column : join_right_column);
    if (left_join_index < 0 || right_join_index < 0) {
        char* error = oasp_mdb_join_columns_error(&left, &right);
        oasp_mdb_split_free(&projection);
        oasp_mdb_dump_free(&left);
        oasp_mdb_dump_free(&right);
        return error;
    }
    if (where_kind != 0) {
        if (oasp_mdb_name_targets_table(where_column, left_table)) {
            where_left_index = oasp_mdb_dump_column_index(&left, where_column);
        } else {
            where_right_index = oasp_mdb_dump_column_index(&right, where_column);
        }
        if (where_left_index < 0 && where_right_index < 0) {
            oasp_mdb_split_free(&projection);
            oasp_mdb_dump_free(&left);
            oasp_mdb_dump_free(&right);
            return oasp_mdb_errorf("Error", "JOIN WHERE column not found");
        }
    }
    i = 0;
    while (i < projection.len) {
        oasp_mdb_append_clean_cell(&out, oasp_mdb_column_suffix(projection.items[i]));
        if (i + 1 < projection.len) {
            oasp_mdb_sb_append_char(&out, '\t');
        }
        i++;
    }
    oasp_mdb_sb_append_char(&out, '\n');
    i = 0;
    while (i < left.row_count) {
        size_t j = 0;
        while (j < right.row_count) {
            int matches = !g_ascii_strcasecmp(left.rows[i][left_join_index], right.rows[j][right_join_index]);
            if (matches && where_left_index >= 0) {
                int comparison = oasp_mdb_compare_values(left.rows[i][where_left_index], where_value);
                matches = where_kind == 1 ? comparison == 0 : (where_kind == 2 ? comparison > 0 : (where_kind == 3 ? comparison < 0 : oasp_mdb_contains_fold(left.rows[i][where_left_index], where_value)));
            }
            if (matches && where_right_index >= 0) {
                int comparison = oasp_mdb_compare_values(right.rows[j][where_right_index], where_value);
                matches = where_kind == 1 ? comparison == 0 : (where_kind == 2 ? comparison > 0 : (where_kind == 3 ? comparison < 0 : oasp_mdb_contains_fold(right.rows[j][where_right_index], where_value)));
            }
            if (matches && (top_count <= 0 || output_count < (size_t)top_count)) {
                size_t p = 0;
                while (p < projection.len) {
                    const EgMdbDump* source = oasp_mdb_name_targets_table(projection.items[p], left_table) ? &left : &right;
                    int column_index = oasp_mdb_dump_column_index(source, projection.items[p]);
                    if (column_index < 0) {
                        oasp_mdb_sb_free(&out);
                        oasp_mdb_split_free(&projection);
                        oasp_mdb_dump_free(&left);
                        oasp_mdb_dump_free(&right);
                        return oasp_mdb_errorf("Error", "JOIN SELECT column not found");
                    }
                    const char* value = source == &left ? left.rows[i][column_index] : right.rows[j][column_index];
                    oasp_mdb_append_clean_cell(&out, value);
                    if (p + 1 < projection.len) {
                        oasp_mdb_sb_append_char(&out, '\t');
                    }
                    p++;
                }
                oasp_mdb_sb_append_char(&out, '\n');
                output_count++;
            }
            j++;
        }
        i++;
    }
    (void)distinct;
    oasp_mdb_split_free(&projection);
    oasp_mdb_dump_free(&left);
    oasp_mdb_dump_free(&right);
    return oasp_mdb_sb_take(&out);
}

typedef struct EgMdbBoundRow {
    char* values[256];
    int lengths[256];
} EgMdbBoundRow;

static const char* oasp_mdb_skip_space(const char* text) {
    while (text && isspace((unsigned char)*text)) {
        text++;
    }
    return text;
}

static char* oasp_mdb_trimmed_copy(const char* begin, const char* end) {
    while (begin < end && isspace((unsigned char)*begin)) {
        begin++;
    }
    while (end > begin && isspace((unsigned char)end[-1])) {
        end--;
    }
    return g_strndup(begin, (gsize)(end - begin));
}

static int oasp_mdb_outer_parentheses(const char* text, size_t length) {
    size_t i;
    int depth = 0;
    int quoted = 0;
    if (length < 2 || text[0] != '(' || text[length - 1] != ')') {
        return 0;
    }
    for (i = 0; i < length; i++) {
        if (text[i] == '\'' && (i == 0 || text[i - 1] != '\\')) {
            if (quoted && i + 1 < length && text[i + 1] == '\'') {
                i++;
            } else {
                quoted = !quoted;
            }
        } else if (!quoted && text[i] == '(') {
            depth++;
        } else if (!quoted && text[i] == ')') {
            if (--depth == 0 && i + 1 < length) {
                return 0;
            }
        }
    }
    return depth == 0 && !quoted;
}

static const char* oasp_mdb_find_top_level_keyword(const char* text, const char* keyword) {
    size_t keyword_len = strlen(keyword);
    int depth = 0;
    int quoted = 0;
    const char* cursor;
    for (cursor = text; *cursor; cursor++) {
        if (*cursor == '\'') {
            if (quoted && cursor[1] == '\'') {
                cursor++;
            } else {
                quoted = !quoted;
            }
            continue;
        }
        if (quoted) {
            continue;
        }
        if (*cursor == '(') {
            depth++;
            continue;
        }
        if (*cursor == ')' && depth > 0) {
            depth--;
            continue;
        }
        if (depth == 0 && !g_ascii_strncasecmp(cursor, keyword, keyword_len)
            && (cursor == text || !isalnum((unsigned char)cursor[-1]))
            && !isalnum((unsigned char)cursor[keyword_len])) {
            return cursor;
        }
    }
    return NULL;
}

static char* oasp_mdb_unquote_literal(const char* text) {
    const char* start = oasp_mdb_skip_space(text);
    size_t length = strlen(start);
    char* value;
    size_t in = 0;
    size_t out = 0;
    while (length && isspace((unsigned char)start[length - 1])) {
        length--;
    }
    if (!g_ascii_strncasecmp(start, "null", length) && length == 4) {
        return NULL;
    }
    if (length >= 2 && ((start[0] == '\'' && start[length - 1] == '\'')
                        || (start[0] == '#' && start[length - 1] == '#'))) {
        value = g_malloc(length - 1);
        if (!value) {
            return NULL;
        }
        for (in = 1; in + 1 < length; in++) {
            value[out++] = start[in];
            if (start[0] == '\'' && start[in] == '\'' && in + 2 < length && start[in + 1] == '\'') {
                in++;
            }
        }
        value[out] = '\0';
        return value;
    }
    return g_strndup(start, length);
}

static int oasp_mdb_bound_column(MdbTableDef* table, const char* name) {
    char* copy;
    char* begin;
    char* end;
    int index;
    if (!name) {
        return -1;
    }
    copy = g_strdup(name);
    if (!copy) {
        return -1;
    }
    begin = copy;
    while (*begin && isspace((unsigned char)*begin)) {
        begin++;
    }
    end = begin + strlen(begin);
    while (end > begin && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    if (*begin == '[' && end > begin + 1 && end[-1] == ']') {
        begin++;
        end[-1] = '\0';
    }
    if (strrchr(begin, '.')) {
        begin = strrchr(begin, '.') + 1;
    }
    index = oasp_mdb_column_index(table, begin);
    g_free(copy);
    return index;
}

static int oasp_mdb_bind_all_columns(MdbTableDef* table, EgMdbBoundRow* row) {
    unsigned int i;
    memset(row, 0, sizeof(*row));
    for (i = 0; i < table->num_cols; i++) {
        MdbColumn* column = g_ptr_array_index(table->columns, i);
        if (column && column->col_type == MDB_OLE) {
            continue;
        }
        row->values[i] = g_malloc0(OASP_MDB_BIND_SIZE + 1);
        if (!row->values[i] || mdb_bind_column(table, (int)i + 1, row->values[i], &row->lengths[i]) == -1) {
            return 0;
        }
    }
    return 1;
}

static void oasp_mdb_unbind_all_columns(MdbTableDef* table, EgMdbBoundRow* row) {
    unsigned int i;
    for (i = 0; i < table->num_cols && i < 256; i++) {
        g_free(row->values[i]);
    }
}

static int oasp_mdb_predicate_matches(MdbTableDef* table, const EgMdbBoundRow* row, const char* expression);

static int oasp_mdb_compare_predicate(MdbTableDef* table, const EgMdbBoundRow* row, const char* expression) {
    static const char* operators[] = {"<=", ">=", "<>", "=", "<", ">"};
    const char* operator = NULL;
    size_t operator_len = 0;
    size_t i;
    int quoted = 0;
    int depth = 0;
    const char* is_null;
    const char* in;
    char* left;
    char* right;
    int column;
    int comparison;
    expression = oasp_mdb_skip_space(expression);
    is_null = oasp_mdb_find_top_level_keyword(expression, "is");
    if (is_null && !g_ascii_strncasecmp(oasp_mdb_skip_space(is_null + 2), "null", 4)) {
        left = oasp_mdb_trimmed_copy(expression, is_null);
        column = oasp_mdb_bound_column(table, left);
        g_free(left);
        return column >= 0 && row->lengths[column] <= 0;
    }
    if (is_null && !g_ascii_strncasecmp(oasp_mdb_skip_space(is_null + 2), "not", 3)
        && !g_ascii_strncasecmp(oasp_mdb_skip_space(oasp_mdb_skip_space(is_null + 2) + 3), "null", 4)) {
        left = oasp_mdb_trimmed_copy(expression, is_null);
        column = oasp_mdb_bound_column(table, left);
        g_free(left);
        return column >= 0 && row->lengths[column] > 0;
    }
    in = oasp_mdb_find_top_level_keyword(expression, "in");
    if (in) {
        const char* list = oasp_mdb_skip_space(in + 2);
        const char* cursor;
        const char* item;
        left = oasp_mdb_trimmed_copy(expression, in);
        column = oasp_mdb_bound_column(table, left);
        g_free(left);
        if (column < 0 || row->lengths[column] <= 0 || *list != '(') {
            return 0;
        }
        item = list + 1;
        quoted = 0;
        for (cursor = item; ; cursor++) {
            if (*cursor == '\'' && (cursor == item || cursor[-1] != '\\')) {
                if (quoted && cursor[1] == '\'') {
                    cursor++;
                } else {
                    quoted = !quoted;
                }
            }
            if (!*cursor || (!quoted && (*cursor == ',' || *cursor == ')'))) {
                char* value = oasp_mdb_trimmed_copy(item, cursor);
                char* literal = oasp_mdb_unquote_literal(value);
                g_free(value);
                if (literal && oasp_mdb_compare_values(row->values[column], literal) == 0) {
                    g_free(literal);
                    return 1;
                }
                g_free(literal);
                if (!*cursor || *cursor == ')') {
                    return 0;
                }
                item = cursor + 1;
            }
        }
    }
    for (i = 0; i < strlen(expression); i++) {
        if (expression[i] == '\'') {
            quoted = !quoted;
        } else if (!quoted && expression[i] == '(') {
            depth++;
        } else if (!quoted && expression[i] == ')' && depth > 0) {
            depth--;
        } else if (!quoted && depth == 0) {
            size_t op;
            for (op = 0; op < G_N_ELEMENTS(operators); op++) {
                size_t length = strlen(operators[op]);
                if (!strncmp(expression + i, operators[op], length)) {
                    operator = expression + i;
                    operator_len = length;
                    break;
                }
            }
            if (operator) {
                break;
            }
        }
    }
    if (!operator) {
        return 0;
    }
    left = oasp_mdb_trimmed_copy(expression, operator);
    right = oasp_mdb_unquote_literal(operator + operator_len);
    column = oasp_mdb_bound_column(table, left);
    g_free(left);
    if (column < 0 || !right || row->lengths[column] <= 0) {
        g_free(right);
        return 0;
    }
    comparison = oasp_mdb_compare_values(row->values[column], right);
    g_free(right);
    return (operator_len == 1 && operator[0] == '=' && comparison == 0)
        || (operator_len == 2 && operator[0] == '<' && operator[1] == '>' && comparison != 0)
        || (operator_len == 1 && operator[0] == '<' && comparison < 0)
        || (operator_len == 2 && operator[0] == '<' && operator[1] == '=' && comparison <= 0)
        || (operator_len == 1 && operator[0] == '>' && comparison > 0)
        || (operator_len == 2 && operator[0] == '>' && operator[1] == '=' && comparison >= 0);
}

static int oasp_mdb_predicate_matches(MdbTableDef* table, const EgMdbBoundRow* row, const char* expression) {
    const char* split;
    char* copy;
    size_t length;
    expression = oasp_mdb_skip_space(expression ? expression : "");
    length = strlen(expression);
    while (oasp_mdb_outer_parentheses(expression, length)) {
        expression++;
        length -= 2;
        while (length && isspace((unsigned char)expression[0])) {
            expression++;
            length--;
        }
        while (length && isspace((unsigned char)expression[length - 1])) {
            length--;
        }
    }
    if (!length) {
        return 1;
    }
    copy = oasp_mdb_trimmed_copy(expression, expression + length);
    if (!copy) {
        return 0;
    }
    split = oasp_mdb_find_top_level_keyword(copy, "or");
    if (split) {
        * (char*)split = '\0';
        int result = oasp_mdb_predicate_matches(table, row, copy)
            || oasp_mdb_predicate_matches(table, row, split + 2);
        g_free(copy);
        return result;
    }
    split = oasp_mdb_find_top_level_keyword(copy, "and");
    if (split) {
        * (char*)split = '\0';
        int result = oasp_mdb_predicate_matches(table, row, copy)
            && oasp_mdb_predicate_matches(table, row, split + 3);
        g_free(copy);
        return result;
    }
    if (!g_ascii_strncasecmp(copy, "not ", 4)) {
        int result = !oasp_mdb_predicate_matches(table, row, copy + 4);
        g_free(copy);
        return result;
    }
    int result = oasp_mdb_compare_predicate(table, row, copy);
    g_free(copy);
    return result;
}

static char* oasp_mdb_expression_value(MdbTableDef* table, const EgMdbBoundRow* row, const char* expression) {
    const char* operators = "&+-*/";
    const char* operator = NULL;
    size_t i;
    int quoted = 0;
    int depth = 0;
    char* text = oasp_mdb_trimmed_copy(expression, expression + strlen(expression));
    if (!text) {
        return NULL;
    }
    for (i = 0; text[i]; i++) {
        if (text[i] == '\'') {
            quoted = !quoted;
        } else if (!quoted && text[i] == '(') {
            depth++;
        } else if (!quoted && text[i] == ')' && depth > 0) {
            depth--;
        } else if (!quoted && depth == 0 && strchr(operators, text[i]) && i != 0) {
            operator = text + i;
        }
    }
    if (operator) {
        char op = *operator;
        char* left_text = oasp_mdb_trimmed_copy(text, operator);
        char* right_text = oasp_mdb_trimmed_copy(operator + 1, text + strlen(text));
        char* lhs = left_text ? oasp_mdb_expression_value(table, row, left_text) : NULL;
        char* rhs = right_text ? oasp_mdb_expression_value(table, row, right_text) : NULL;
        char* result = NULL;
        g_free(left_text);
        g_free(right_text);
        if (!lhs || !rhs) {
            g_free(lhs);
            g_free(rhs);
            g_free(text);
            return NULL;
        }
        if (op == '&') {
            result = g_strdup_printf("s%s%s", lhs + 1, rhs + 1);
        } else {
            double a;
            double b;
            if (!oasp_mdb_parse_number(lhs + 1, &a) || !oasp_mdb_parse_number(rhs + 1, &b)
                || (op == '/' && b == 0.0)) {
                g_free(lhs);
                g_free(rhs);
                g_free(text);
                return NULL;
            }
            result = g_strdup_printf("v%.15g", op == '+' ? a + b : op == '-' ? a - b : op == '*' ? a * b : a / b);
        }
        g_free(lhs);
        g_free(rhs);
        g_free(text);
        return result;
    }
    if (!g_ascii_strcasecmp(text, "null")) {
        g_free(text);
        return g_strdup("n");
    }
    if ((text[0] == '\'' && text[strlen(text) - 1] == '\'') || (text[0] == '#' && text[strlen(text) - 1] == '#')) {
        char* literal = oasp_mdb_unquote_literal(text);
        char* result = g_strdup_printf("%c%s", text[0] == '#' ? 'd' : 's', literal ? literal : "");
        g_free(literal);
        g_free(text);
        return result;
    }
    {
        int column = oasp_mdb_bound_column(table, text);
        if (column >= 0) {
            MdbColumn* metadata = g_ptr_array_index(table->columns, column);
            char prefix = metadata && (metadata->col_type == MDB_TEXT || metadata->col_type == MDB_MEMO) ? 's' : 'v';
            char* result = row->lengths[column] <= 0 ? g_strdup("n") : g_strdup_printf("%c%s", prefix, row->values[column]);
            g_free(text);
            return result;
        }
    }
    {
        char* result = g_strdup_printf("v%s", text);
        g_free(text);
        return result;
    }
}

/*
 * Update matching rows in place under one journal transaction. Index entries
 * are removed before row replacement and rebuilt afterward so readers never
 * observe an index pointing at stale row bytes.
 */
char* oasp_mdb_native_update_where_raw(const char* db_path, const char* table_name, const char* where_sql, const char* set_columns, const char* set_expressions) {
    MdbHandle* mdb = NULL;
    MdbTableDef* table = NULL;
    EgMdbSplitList columns;
    EgMdbSplitList values;
    char* decoded_where_sql = NULL;
    EgMdbBoundRow bound_row;
    size_t i = 0;
    int found = 0;
    int updating_indexed_column = 0;
    OaspMdbLock lock;
    char lock_error[512];
    char* error = NULL;
    int row_start = 0;
    size_t row_size = 0;
    int num_fields = 0;
    unsigned char row_buffer[4096];
    MdbField fields[256];
    MdbField normalized_fields[256];
    memset(&columns, 0, sizeof(columns));
    memset(&values, 0, sizeof(values));
    memset(&lock, 0, sizeof(lock));

    memset(&bound_row, 0, sizeof(bound_row));
    if (!db_path || !table_name || !where_sql) {
        return oasp_mdb_errorf("Error", "invalid MDB update arguments");
    }
    columns = oasp_mdb_split_sep(set_columns ? set_columns : "");
    values = oasp_mdb_split_sep(set_expressions ? set_expressions : "");
    decoded_where_sql = oasp_mdb_decode_hex_string(where_sql);
    if (columns.len == 0 || columns.len != values.len || columns.len > 256
        || !oasp_mdb_decode_hex_list(&columns) || !oasp_mdb_decode_hex_list(&values)
        || !decoded_where_sql) {
        error = oasp_mdb_errorf("Error", "invalid encoded UPDATE payload");
        goto cleanup;
    }
    if (!oasp_mdb_lock_acquire(db_path, 1, &lock, lock_error, sizeof(lock_error))) {
        error = oasp_mdb_errorf("Error", lock_error);
        goto cleanup;
    }
    mdb = mdb_open(db_path, MDB_WRITABLE);
    if (!mdb) {
        error = oasp_mdb_errorf("Error", "unable to open MDB file for writing");
        goto cleanup;
    }
    mdb_set_bind_size(mdb, OASP_MDB_BIND_SIZE);
    table = mdb_read_table_by_name(mdb, (char*)table_name, MDB_TABLE);
    if (!table) {
        error = oasp_mdb_errorf("Error", "access table not found");
        goto cleanup;
    }
    if (!mdb_read_columns(table)) {
        error = oasp_mdb_errorf("Error", "unable to read table metadata");
        goto cleanup;
    }
    if (table->num_idxs > 0) {
        (void)mdb_read_indices(table);
        if (!table->indices || table->indices->len < table->num_idxs) {
            error = oasp_mdb_errorf("Error", "unable to read table indexes");
            goto cleanup;
        }
    }
    i = 0;
    while (i < columns.len) {
        int column_index = oasp_mdb_column_index(table, columns.items[i]);
        if (column_index < 0) {
            error = oasp_mdb_errorf("Error", "UPDATE column not found");
            goto cleanup;
        }
        if (oasp_mdb_column_is_indexed(table, column_index)) {
            updating_indexed_column = 1;
        }
        i++;
    }
    mdb_rewind_table(table);
    if (!oasp_mdb_bind_all_columns(table, &bound_row)) {
        error = oasp_mdb_errorf("Error", "unable to bind UPDATE row");
        goto cleanup;
    }
    while (mdb_fetch_row(table)) {
        char* assigned_values[256];
        char* owned_fields[256];
        size_t estimated_size = 64;
        int replace_rc = 1;
        int assignments_changed = 0;
        memset(assigned_values, 0, sizeof(assigned_values));
        memset(owned_fields, 0, sizeof(owned_fields));
        if (!oasp_mdb_predicate_matches(table, &bound_row, decoded_where_sql)) {
            continue;
        }
        if (!oasp_mdb_load_data_row(table, table->cur_phys_pg, (guint16)(table->cur_row - 1),
                                    &row_start, &row_size)) {
            error = oasp_mdb_errorf("Error", "unable to reload UPDATE data page");
            goto update_row_cleanup;
        }
        num_fields = oasp_mdb_decode_jet4_row(table, row_start, row_size, fields);
        if (num_fields <= 0 || num_fields > 256) {
            /*
             * A table scan can expose a deleted LVAL overflow slot. It has
             * no valid row header on the table data page and must never be
             * treated as an UPDATE target.
             */
            continue;
        }
        /*
         * ALTER ADD COLUMN leaves older rows without a physical field for the
         * new nullable column. Rebuild the row against the current schema so
         * assigning that column is valid and untouched new columns stay NULL.
         */
        if ((unsigned int)num_fields != table->num_cols) {
            size_t source = 0;
            memset(normalized_fields, 0, sizeof(normalized_fields));
            for (i = 0; i < table->num_cols; i++) {
                MdbColumn* column = g_ptr_array_index(table->columns, i);
                normalized_fields[i].colnum = (int)i;
                normalized_fields[i].is_fixed = column ? column->is_fixed : 0;
                normalized_fields[i].is_null = 1;
            }
            while (source < (size_t)num_fields) {
                if (fields[source].colnum < 0 || (unsigned int)fields[source].colnum >= table->num_cols) {
                    error = oasp_mdb_errorf("Error", "invalid MDB row column layout");
                    goto update_row_cleanup;
                }
                normalized_fields[fields[source].colnum] = fields[source];
                source++;
            }
            memcpy(fields, normalized_fields, table->num_cols * sizeof(*fields));
            num_fields = (int)table->num_cols;
        }
        i = 0;
        while (i < (size_t)num_fields) {
            estimated_size += (size_t)(fields[i].siz > 0 ? fields[i].siz : 0);
            i++;
        }
        i = 0;
        while (i < columns.len) {
            int column_index = oasp_mdb_column_index(table, columns.items[i]);
            MdbColumn* column = column_index >= 0 ? g_ptr_array_index(table->columns, column_index) : NULL;
            int field_index = 0;
            while (field_index < num_fields && fields[field_index].colnum != column_index) {
                field_index++;
            }
            char* expression_value = oasp_mdb_expression_value(table, &bound_row, values.items[i]);
            int memo_is_unchanged;
            if (!column || field_index >= num_fields || !expression_value
                || !oasp_mdb_literal_to_field(mdb, column, expression_value, &fields[field_index], &assigned_values[i])) {
                g_free(expression_value);
                error = oasp_mdb_errorf("Error", "invalid value for MDB UPDATE column");
                goto update_row_cleanup;
            }
            memo_is_unchanged = column->col_type == MDB_MEMO && expression_value[0] == 's'
                && bound_row.lengths[column_index] >= 0
                && !strcmp(expression_value + 1, bound_row.values[column_index]);
            g_free(expression_value);
            free(owned_fields[column_index]);
            owned_fields[column_index] = assigned_values[i];
            assigned_values[i] = NULL;
            if (!memo_is_unchanged) {
                assignments_changed = 1;
            }
            estimated_size += (size_t)fields[field_index].siz;
            i++;
        }
        if (!assignments_changed) {
            found++;
            goto update_row_cleanup;
        }
        for (i = 0; i < (size_t)num_fields; i++) {
            if (!fields[i].is_null && !owned_fields[i]) {
                owned_fields[i] = malloc((size_t)fields[i].siz);
                if (!owned_fields[i]) {
                    error = oasp_mdb_errorf("Error", "out of memory while snapshotting MDB UPDATE row");
                    goto update_row_cleanup;
                }
                memcpy(owned_fields[i], fields[i].value, (size_t)fields[i].siz);
                fields[i].value = owned_fields[i];
            }
        }
        if (!lock.transaction_active && !oasp_mdb_transaction_begin(&lock, lock_error, sizeof(lock_error))) {
            error = oasp_mdb_errorf("Error", lock_error);
            goto update_row_cleanup;
        }
        if (!oasp_mdb_externalize_long_values(table, fields, owned_fields, (size_t)num_fields,
                                              lock_error, sizeof(lock_error))) {
            error = oasp_mdb_errorf("Error", lock_error);
            goto update_row_cleanup;
        }
        estimated_size = 64;
        for (i = 0; i < (size_t)num_fields; i++) {
            estimated_size += (size_t)(fields[i].is_null ? 0 : fields[i].siz);
        }
        if (estimated_size >= sizeof(row_buffer)) {
            error = oasp_mdb_errorf("Error", "MDB UPDATE row exceeds Jet page capacity");
            goto update_row_cleanup;
        }
        {
            int new_row_size = oasp_mdb_pack_row_preserving_layout(table, row_buffer, sizeof(row_buffer), (unsigned int)num_fields, fields);
            if (new_row_size <= 0 || (size_t)new_row_size >= sizeof(row_buffer)) {
                error = oasp_mdb_errorf("Error", "unable to encode MDB UPDATE row");
            } else {
                if (updating_indexed_column && table->num_idxs > 0
                    && !oasp_mdb_remove_row_indexes(table, table->cur_phys_pg, (guint16)(table->cur_row - 1))) {
                    error = oasp_mdb_errorf("Error", "unable to remove MDB indexes during UPDATE");
                } else if (mdb_read_pg(mdb, table->cur_phys_pg) != mdb->fmt->pg_size) {
                    /*
                     * Long-value allocation reads its table definition and
                     * writes an LVAL page through the shared mdbtools buffer.
                     * Reload the target data page before mdb_replace_row(),
                     * which reconstructs its output from mdb->pg_buf.
                     */
                    error = oasp_mdb_errorf("Error", "unable to reload MDB UPDATE page");
                } else {
                    replace_rc = mdb_replace_row(table, table->cur_row - 1, row_buffer, new_row_size);
                    if (updating_indexed_column && replace_rc == 0
                        && !oasp_mdb_update_row_indexes(table, num_fields, fields, table->cur_phys_pg,
                                                      (guint16)(table->cur_row - 1))) {
                        error = oasp_mdb_errorf("Error", "unable to rebuild MDB indexes during UPDATE");
                    }
                }
            }
        }
        if (!error && replace_rc != 0) {
            error = oasp_mdb_errorf("Error", "mdbtools row update failed");
            goto update_row_cleanup;
        }
        found++;

update_row_cleanup:
        i = 0;
        while (i < 256) {
            free(assigned_values[i]);
            free(owned_fields[i]);
            i++;
        }
        if (error) {
            goto cleanup;
        }
    }
    if (found == 0) {
        error = oasp_mdb_errorf("Error", "UPDATE target row not found");
    }

cleanup:
    if (table) {
        oasp_mdb_unbind_all_columns(table, &bound_row);
    }
    free(decoded_where_sql);
    oasp_mdb_close_table(mdb, table);
    mdb = NULL;
    table = NULL;
    oasp_mdb_split_free(&columns);
    oasp_mdb_split_free(&values);
    if (error) {
        if (!oasp_mdb_transaction_rollback(&lock, lock_error, sizeof(lock_error))) {
            free(error);
            error = oasp_mdb_errorf("Error", lock_error);
        }
    } else if (lock.transaction_active) {
        if (!oasp_mdb_transaction_commit(&lock, lock_error, sizeof(lock_error))) {
            error = oasp_mdb_errorf("Error", lock_error);
            (void)oasp_mdb_transaction_rollback(&lock, lock_error, sizeof(lock_error));
        }
    }
    oasp_mdb_lock_release(&lock);
    return error ? error : oasp_mdb_strdup_or_empty("");
}

/*
 * Build a typed row, allocate AutoNumber while exclusively locked, append data
 * and index entries, then update table counters in the same journal transaction.
 */
char* oasp_mdb_native_insert_raw(const char* db_path, const char* table_name, const char* set_columns, const char* set_values) {
    MdbHandle* mdb = NULL;
    MdbTableDef* table = NULL;
    EgMdbSplitList columns;
    EgMdbSplitList values;
    MdbField fields[256];
    char* field_values[256];
    unsigned char provided[256];
    size_t i = 0;
    size_t j = 0;
    size_t estimated_size = 64;
    OaspMdbLock lock;
    char lock_error[512];
    int rc = 0;
    uint32_t next_autonumber = 0;
    int has_generated_autonumber = 0;
    char* error = NULL;
    memset(&columns, 0, sizeof(columns));
    memset(&values, 0, sizeof(values));
    memset(fields, 0, sizeof(fields));
    memset(field_values, 0, sizeof(field_values));
    memset(provided, 0, sizeof(provided));
    memset(&lock, 0, sizeof(lock));
    if (!db_path || !table_name || !*db_path || !*table_name) {
        return oasp_mdb_errorf("Error", "invalid MDB insert arguments");
    }
    columns = oasp_mdb_split_sep(set_columns ? set_columns : "");
    values = oasp_mdb_split_sep(set_values ? set_values : "");
    if (columns.len == 0 || columns.len != values.len || columns.len > 256
        || !oasp_mdb_decode_hex_list(&columns) || !oasp_mdb_decode_hex_list(&values)) {
        error = oasp_mdb_errorf("Error", "invalid encoded INSERT payload");
        goto cleanup;
    }
    if (!oasp_mdb_lock_acquire(db_path, 1, &lock, lock_error, sizeof(lock_error))) {
        error = oasp_mdb_errorf("Error", lock_error);
        goto cleanup;
    }
    mdb = mdb_open(db_path, MDB_WRITABLE);
    if (!mdb) {
        error = oasp_mdb_errorf("Error", "unable to open MDB file for writing");
        goto cleanup;
    }
    table = mdb_read_table_by_name(mdb, (char*)table_name, MDB_TABLE);
    if (!table || !mdb_read_columns(table) || table->num_cols > 256) {
        error = oasp_mdb_errorf("Error", "unable to read target table metadata");
        goto cleanup;
    }
    if (table->num_idxs > 0) {
        (void)mdb_read_indices(table);
        if (!table->indices || table->indices->len < table->num_idxs) {
            error = oasp_mdb_errorf("Error", "unable to read target table indexes");
            goto cleanup;
        }
    }
    mdb_rewind_table(table);
    i = 0;
    while (i < table->num_cols) {
        MdbColumn* col = g_ptr_array_index(table->columns, i);
        fields[i].colnum = (int)i;
        fields[i].is_null = 1;
        fields[i].is_fixed = col ? (col->col_type != MDB_TEXT && col->col_type != MDB_MEMO) : 0;
        i++;
    }
    i = 0;
    while (i < columns.len) {
        int col_index = oasp_mdb_column_index(table, columns.items[i]);
        MdbColumn* col = col_index >= 0 ? g_ptr_array_index(table->columns, col_index) : NULL;
        if (!col) {
            error = oasp_mdb_errorf("Error", "INSERT column not found");
            goto cleanup;
        }
        if (col->is_long_auto) {
            error = oasp_mdb_errorf("Error", "explicit values for MDB AutoNumber columns are not supported");
            goto cleanup;
        }
        j = 0;
        while (j < i) {
            if (!g_ascii_strcasecmp(columns.items[j], columns.items[i])) {
                error = oasp_mdb_errorf("Error", "duplicate MDB INSERT column");
                goto cleanup;
            }
            j++;
        }
        if (!oasp_mdb_literal_to_field(mdb, col, values.items[i], &fields[col_index], &field_values[col_index])) {
            error = oasp_mdb_errorf("Error", "invalid value for MDB INSERT column");
            goto cleanup;
        }
        provided[col_index] = 1;
        estimated_size += (size_t)fields[col_index].siz;
        i++;
    }
    if (mdb_read_pg(mdb, table->entry->table_pg) != mdb->fmt->pg_size) {
        error = oasp_mdb_errorf("Error", "unable to read MDB table definition");
        goto cleanup;
    }
    next_autonumber = (uint32_t)mdb_get_int32(mdb->pg_buf, 20) + 1;
    i = 0;
    while (i < table->num_cols) {
        MdbColumn* col = g_ptr_array_index(table->columns, i);
        const char* default_expression = col ? mdb_col_get_prop(col, "DefaultValue") : NULL;
        const char* required = col ? mdb_col_get_prop(col, "Required") : NULL;
        if (col && col->is_long_auto && !provided[i]) {
            field_values[i] = (char*)malloc(4);
            if (!field_values[i]) {
                error = oasp_mdb_errorf("Error", "out of memory");
                goto cleanup;
            }
            mdb_put_int32(field_values[i], 0, next_autonumber);
            fields[i].value = field_values[i];
            fields[i].siz = 4;
            fields[i].is_null = 0;
            estimated_size += 4;
            has_generated_autonumber = 1;
        } else if (col && !provided[i] && default_expression) {
            char* default_literal = oasp_mdb_property_literal(default_expression);
            if (!default_literal
                || !oasp_mdb_literal_to_field(mdb, col, default_literal, &fields[i], &field_values[i])) {
                free(default_literal);
                error = oasp_mdb_errorf("Error", "invalid MDB column default value");
                goto cleanup;
            }
            free(default_literal);
            estimated_size += (size_t)fields[i].siz;
        }
        if (col && col->col_type != MDB_BOOL && oasp_mdb_property_is_true(required) && fields[i].is_null) {
            error = oasp_mdb_errorf("Error", "required MDB column has no value");
            goto cleanup;
        }
        i++;
    }
    if (estimated_size >= 4096 && !oasp_mdb_transaction_begin(&lock, lock_error, sizeof(lock_error))) {
        error = oasp_mdb_errorf("Error", lock_error);
        goto cleanup;
    }
    if (!lock.transaction_active && !oasp_mdb_transaction_begin(&lock, lock_error, sizeof(lock_error))) {
        error = oasp_mdb_errorf("Error", lock_error);
        goto cleanup;
    }
    if (!oasp_mdb_externalize_long_values(table, fields, field_values, table->num_cols,
                                          lock_error, sizeof(lock_error))) {
        error = oasp_mdb_errorf("Error", lock_error);
        goto cleanup;
    }
    estimated_size = 64;
    for (i = 0; i < table->num_cols; i++) {
        estimated_size += (size_t)(fields[i].is_null ? 0 : fields[i].siz);
    }
    if (estimated_size >= 4096) {
        error = oasp_mdb_errorf("Error", "MDB INSERT row exceeds Jet page capacity");
        goto cleanup;
    }
    rc = oasp_mdb_insert_packed_row(db_path, table, (int)table->num_cols, fields);
    if (rc == 1 && has_generated_autonumber) {
        for (i = 0; i < table->num_cols; i++) {
            MdbColumn* col = g_ptr_array_index(table->columns, i);
            if (col && col->is_long_auto && fields[i].value && fields[i].siz == 4) {
                next_autonumber = (uint32_t)mdb_get_int32((unsigned char*)fields[i].value, 0);
                break;
            }
        }
        if (!oasp_mdb_update_table_counter(table, 20, next_autonumber)) {
            error = oasp_mdb_errorf("Error", "unable to update MDB autonumber counter");
        }
    }

cleanup:
    j = 0;
    while (j < 256) {
        free(field_values[j]);
        j++;
    }
    oasp_mdb_close_table(mdb, table);
    mdb = NULL;
    table = NULL;
    oasp_mdb_split_free(&columns);
    oasp_mdb_split_free(&values);
    if (error || rc != 1) {
        if (!oasp_mdb_transaction_rollback(&lock, lock_error, sizeof(lock_error))) {
            free(error);
            error = oasp_mdb_errorf("Error", lock_error);
        }
    } else {
        if (!oasp_mdb_transaction_commit(&lock, lock_error, sizeof(lock_error))) {
            error = oasp_mdb_errorf("Error", lock_error);
            (void)oasp_mdb_transaction_rollback(&lock, lock_error, sizeof(lock_error));
        }
    }
    oasp_mdb_lock_release(&lock);
    if (error) {
        return error;
    }
    return rc == 1 ? oasp_mdb_strdup_or_empty("") : oasp_mdb_errorf("Error", "mdbtools row insert failed");
}

/* Mark matching rows deleted and remove their index entries atomically. */
char* oasp_mdb_native_delete_where_raw(const char* db_path, const char* table_name, const char* where_sql) {
    MdbHandle* mdb = NULL;
    MdbTableDef* table = NULL;
    char* decoded_where_sql = NULL;
    EgMdbBoundRow bound_row;
    int deleted = 0;
    OaspMdbLock lock;
    char lock_error[512];
    int row = 0;
    int row_offset = 0;
    size_t row_size = 0;
    unsigned int row_count_offset = 0;
    char* error = NULL;
    int require_match = 0;
    memset(&lock, 0, sizeof(lock));
    memset(&bound_row, 0, sizeof(bound_row));
    if (!db_path || !table_name || !*db_path || !*table_name) {
        return oasp_mdb_errorf("Error", "invalid MDB delete arguments");
    }
    decoded_where_sql = oasp_mdb_decode_hex_string(where_sql ? where_sql : "");
    if (!decoded_where_sql) {
        error = oasp_mdb_errorf("Error", "invalid encoded DELETE payload");
        goto cleanup;
    }
    require_match = decoded_where_sql[0] != '\0';
    if (!oasp_mdb_lock_acquire(db_path, 1, &lock, lock_error, sizeof(lock_error))) {
        error = oasp_mdb_errorf("Error", lock_error);
        goto cleanup;
    }
    mdb = mdb_open(db_path, MDB_WRITABLE);
    if (!mdb) {
        error = oasp_mdb_errorf("Error", "unable to open MDB file for writing");
        goto cleanup;
    }
    table = mdb_read_table_by_name(mdb, (char*)table_name, MDB_TABLE);
    if (!table || !mdb_read_columns(table)) {
        error = oasp_mdb_errorf("Error", "unable to read target table");
        goto cleanup;
    }
    if (table->num_idxs > 0) {
        (void)mdb_read_indices(table);
        if (!table->indices || table->indices->len < table->num_idxs) {
            error = oasp_mdb_errorf("Error", "unable to read target table indexes");
            goto cleanup;
        }
    }
    mdb_rewind_table(table);
    if (!oasp_mdb_bind_all_columns(table, &bound_row)) {
        error = oasp_mdb_errorf("Error", "unable to bind DELETE row");
        goto cleanup;
    }
    row_count_offset = table->entry->mdb->fmt->row_count_offset;
    while (mdb_fetch_row(table)) {
        if (!oasp_mdb_predicate_matches(table, &bound_row, decoded_where_sql)) {
            continue;
        }
        row = (int)table->cur_row - 1;
        if (mdb_find_row(mdb, row, &row_offset, &row_size) != 0 || row_size == 0) {
            error = oasp_mdb_errorf("Error", "unable to locate DELETE row");
            goto cleanup;
        }
        if (!lock.transaction_active && !oasp_mdb_transaction_begin(&lock, lock_error, sizeof(lock_error))) {
            error = oasp_mdb_errorf("Error", lock_error);
            goto cleanup;
        }
        if (table->num_idxs > 0
            && !oasp_mdb_remove_row_indexes(table, table->cur_phys_pg, (guint16)row)) {
            error = oasp_mdb_errorf("Error", "unable to update MDB indexes during DELETE");
            goto cleanup;
        }
        if (mdb_read_pg(mdb, table->cur_phys_pg) != mdb->fmt->pg_size
            || mdb_find_row(mdb, row, &row_offset, &row_size) != 0 || row_size == 0) {
            error = oasp_mdb_errorf("Error", "unable to reload DELETE row");
            goto cleanup;
        }
        row_offset = (row_offset & OASP_MDB_OFFSET_MASK) | OASP_MDB_DELETED_ROW;
        mdb_put_int16(mdb->pg_buf, row_count_offset + 2 + row * 2, (unsigned int)row_offset);
        if (mdb_write_pg(mdb, table->cur_phys_pg) <= 0) {
            error = oasp_mdb_errorf("Error", "unable to write DELETE page");
            goto cleanup;
        }
        deleted++;
    }
    if (deleted > 0) {
        uint32_t remaining = table->num_rows > (unsigned int)deleted ? table->num_rows - (unsigned int)deleted : 0;
        if ((table->num_real_idxs > 0 && !oasp_mdb_update_index_counters(table, remaining))
            || !oasp_mdb_update_table_counter(table, table->entry->mdb->fmt->tab_num_rows_offset, remaining)) {
            error = oasp_mdb_errorf("Error", "unable to update MDB row count after DELETE");
        } else {
            table->num_rows = remaining;
        }
    }

cleanup:
    if (table) {
        oasp_mdb_unbind_all_columns(table, &bound_row);
    }
    free(decoded_where_sql);
    oasp_mdb_close_table(mdb, table);
    mdb = NULL;
    table = NULL;
    if (error) {
        if (!oasp_mdb_transaction_rollback(&lock, lock_error, sizeof(lock_error))) {
            free(error);
            error = oasp_mdb_errorf("Error", lock_error);
        }
    } else if (lock.transaction_active) {
        if (!oasp_mdb_transaction_commit(&lock, lock_error, sizeof(lock_error))) {
            error = oasp_mdb_errorf("Error", lock_error);
            (void)oasp_mdb_transaction_rollback(&lock, lock_error, sizeof(lock_error));
        }
    }
    oasp_mdb_lock_release(&lock);
    if (error) {
        return error;
    }
    if (require_match && deleted == 0) {
        return oasp_mdb_errorf("Error", "DELETE target row not found");
    }
    return oasp_mdb_strdup_or_empty("");
}

static char* oasp_mdb_encode_hex_text(const char* text) {
    static const char digits[] = "0123456789abcdef";
    size_t length = strlen(text ? text : "");
    char* encoded = g_malloc(length * 2 + 1);
    size_t i;
    if (!encoded) {
        return NULL;
    }
    for (i = 0; i < length; i++) {
        encoded[i * 2] = digits[((const unsigned char*)text)[i] >> 4];
        encoded[i * 2 + 1] = digits[((const unsigned char*)text)[i] & 15];
    }
    encoded[length * 2] = '\0';
    return encoded;
}

static char* oasp_mdb_typed_expression(const char* value) {
    const char* text = value && *value ? value + 1 : "";
    char* escaped;
    if (!value || !*value || value[0] == 'n') {
        return g_strdup("null");
    }
    if (value[0] == 's') {
        GString* builder = g_string_new(NULL);
        size_t i;
        if (!builder) {
            return NULL;
        }
        for (i = 0; text[i]; i++) {
            g_string_append_c(builder, text[i]);
            if (text[i] == '\'') {
                g_string_append_c(builder, '\'');
            }
        }
        escaped = g_string_free(builder, FALSE);
        char* result = g_strdup_printf("'%s'", escaped);
        g_free(escaped);
        return result;
    }
    if (value[0] == 'd') {
        return g_strdup_printf("#%s#", text);
    }
    return g_strdup(text);
}

static char* oasp_mdb_legacy_where(const char* where_column, const char* where_value) {
    char* column = oasp_mdb_decode_hex_string(where_column);
    char* value = oasp_mdb_decode_hex_string(where_value);
    char* expression;
    char* encoded;
    if (!column || !value) {
        g_free(column);
        g_free(value);
        return NULL;
    }
    if (value[0] == 'n') {
        expression = g_strdup_printf("%s IS NULL", column);
    } else {
        char* right = oasp_mdb_typed_expression(value);
        expression = right ? g_strdup_printf("%s = %s", column, right) : NULL;
        g_free(right);
    }
    encoded = expression ? oasp_mdb_encode_hex_text(expression) : NULL;
    g_free(expression);
    g_free(column);
    g_free(value);
    return encoded;
}

char* oasp_mdb_native_update_typed_where_raw(const char* db_path, const char* table_name, const char* where_sql, const char* set_columns, const char* set_values) {
    EgMdbSplitList values = oasp_mdb_split_sep(set_values ? set_values : "");
    GString* expressions = g_string_new(NULL);
    char* result;
    size_t i;
    if (!expressions || !oasp_mdb_decode_hex_list(&values)) {
        oasp_mdb_split_free(&values);
        if (expressions) {
            g_string_free(expressions, TRUE);
        }
        return oasp_mdb_errorf("Error", "invalid legacy UPDATE payload");
    }
    for (i = 0; i < values.len; i++) {
        char* expression = oasp_mdb_typed_expression(values.items[i]);
        char* encoded = expression ? oasp_mdb_encode_hex_text(expression) : NULL;
        g_free(expression);
        if (!encoded) {
            oasp_mdb_split_free(&values);
            g_string_free(expressions, TRUE);
            return oasp_mdb_errorf("Error", "out of memory translating UPDATE payload");
        }
        if (i) {
            g_string_append_c(expressions, OASP_MDB_SEP);
        }
        g_string_append(expressions, encoded);
        g_free(encoded);
    }
    result = oasp_mdb_native_update_where_raw(db_path, table_name, where_sql, set_columns, expressions->str);
    oasp_mdb_split_free(&values);
    g_string_free(expressions, TRUE);
    return result;
}

char* oasp_mdb_native_update_raw(const char* db_path, const char* table_name, const char* where_column, const char* where_value, const char* set_columns,
                                 const char* set_values) {
    char* where = oasp_mdb_legacy_where(where_column, where_value);
    char* result;
    if (!where) {
        return oasp_mdb_errorf("Error", "invalid legacy UPDATE predicate");
    }
    result = oasp_mdb_native_update_typed_where_raw(db_path, table_name, where, set_columns, set_values);
    g_free(where);
    return result;
}

char* oasp_mdb_native_delete_raw(const char* db_path, const char* table_name, const char* where_column, const char* where_value) {
    char* where;
    char* result;
    if (!where_column || !*where_column) {
        where = oasp_mdb_encode_hex_text("");
    } else {
        where = oasp_mdb_legacy_where(where_column, where_value);
    }
    if (!where) {
        return oasp_mdb_errorf("Error", "invalid legacy DELETE predicate");
    }
    result = oasp_mdb_native_delete_where_raw(db_path, table_name, where);
    g_free(where);
    return result;
}

/* Convert and free the malloc-owned result returned by every raw operation. */
static oasp_string_t* oasp_mdb_runtime_result(char* raw) {
    oasp_string_t* result = oasp_string_from_cstr(raw ? raw : "");
    free(raw);
    return result;
}

oasp_string_t* oasp_mdb_query(oasp_string_t* db_path, oasp_string_t* table_name, oasp_string_t* select_columns, int64_t where_kind, oasp_string_t* where_column,
                              oasp_string_t* where_value, oasp_string_t* order_column, int64_t order_desc, int64_t top_count, int64_t distinct, int64_t aggregate_kind) {
    OaspMdbLock lock;
    char error[512];
    char* result;
    if (!oasp_mdb_lock_acquire(oasp_string_cstr(db_path), 0, &lock, error, sizeof(error))) {
        return oasp_mdb_runtime_result(oasp_mdb_errorf("Error", error));
    }
    result = oasp_mdb_query_raw(oasp_string_cstr(db_path), oasp_string_cstr(table_name), oasp_string_cstr(select_columns), (int)where_kind, oasp_string_cstr(where_column), oasp_string_cstr(where_value), oasp_string_cstr(order_column), (int)order_desc, (int)top_count, (int)distinct, (int)aggregate_kind);
    oasp_mdb_lock_release(&lock);
    return oasp_mdb_runtime_result(result);
}

oasp_string_t* oasp_mdb_sql_query(oasp_string_t* db_path, oasp_string_t* table_name, oasp_string_t* select_columns, oasp_string_t* where_sql, oasp_string_t* order_column,
                                  int64_t order_desc, int64_t top_count, int64_t distinct, int64_t aggregate_kind) {
    OaspMdbLock lock;
    char error[512];
    char* result;
    if (!oasp_mdb_lock_acquire(oasp_string_cstr(db_path), 0, &lock, error, sizeof(error))) {
        return oasp_mdb_runtime_result(oasp_mdb_errorf("Error", error));
    }
    result = oasp_mdb_sql_query_raw(oasp_string_cstr(db_path), oasp_string_cstr(table_name), oasp_string_cstr(select_columns), oasp_string_cstr(where_sql), oasp_string_cstr(order_column), (int)order_desc, (int)top_count, (int)distinct, (int)aggregate_kind);
    oasp_mdb_lock_release(&lock);
    return oasp_mdb_runtime_result(result);
}

oasp_string_t* oasp_mdb_join_query(oasp_string_t* db_path, oasp_string_t* left_table, oasp_string_t* right_table, oasp_string_t* select_columns, int64_t where_kind,
                                   oasp_string_t* where_column, oasp_string_t* where_value, oasp_string_t* join_left_column, oasp_string_t* join_right_column, int64_t top_count, int64_t distinct) {
    OaspMdbLock lock;
    char error[512];
    char* result;
    if (!oasp_mdb_lock_acquire(oasp_string_cstr(db_path), 0, &lock, error, sizeof(error))) {
        return oasp_mdb_runtime_result(oasp_mdb_errorf("Error", error));
    }
    result = oasp_mdb_join_query_raw(oasp_string_cstr(db_path), oasp_string_cstr(left_table), oasp_string_cstr(right_table), oasp_string_cstr(select_columns), (int)where_kind, oasp_string_cstr(where_column), oasp_string_cstr(where_value), oasp_string_cstr(join_left_column), oasp_string_cstr(join_right_column), (int)top_count, (int)distinct);
    oasp_mdb_lock_release(&lock);
    return oasp_mdb_runtime_result(result);
}

oasp_string_t* oasp_mdb_table_dump(oasp_string_t* db_path, oasp_string_t* table_name) {
    OaspMdbLock lock;
    char error[512];
    char* result;
    if (!oasp_mdb_lock_acquire(oasp_string_cstr(db_path), 0, &lock, error, sizeof(error))) {
        return oasp_mdb_runtime_result(oasp_mdb_errorf("Error", error));
    }
    result = oasp_mdb_table_dump_raw(oasp_string_cstr(db_path), oasp_string_cstr(table_name));
    oasp_mdb_lock_release(&lock);
    return oasp_mdb_runtime_result(result);
}

oasp_string_t* oasp_mdb_update(oasp_string_t* db_path, oasp_string_t* table_name, oasp_string_t* where_column, oasp_string_t* where_value, oasp_string_t* set_columns,
                               oasp_string_t* set_values) {
    return oasp_mdb_runtime_result(oasp_mdb_native_update_raw(oasp_string_cstr(db_path), oasp_string_cstr(table_name), oasp_string_cstr(where_column), oasp_string_cstr(where_value), oasp_string_cstr(set_columns), oasp_string_cstr(set_values)));
}

oasp_string_t* oasp_mdb_insert_row(oasp_string_t* db_path, oasp_string_t* table_name, oasp_string_t* columns, oasp_string_t* values) {
    return oasp_mdb_runtime_result(oasp_mdb_native_insert_raw(oasp_string_cstr(db_path), oasp_string_cstr(table_name), oasp_string_cstr(columns), oasp_string_cstr(values)));
}

oasp_string_t* oasp_mdb_delete(oasp_string_t* db_path, oasp_string_t* table_name, oasp_string_t* where_column, oasp_string_t* where_value) {
    return oasp_mdb_runtime_result(oasp_mdb_native_delete_raw(oasp_string_cstr(db_path), oasp_string_cstr(table_name), oasp_string_cstr(where_column), oasp_string_cstr(where_value)));
}

oasp_string_t* oasp_mdb_create_table(oasp_string_t* db_path, oasp_string_t* sql) {
    char error[512];
    if (!oasp_mdb_create_table_file(oasp_string_cstr(db_path), oasp_string_cstr(sql), error, sizeof(error))) {
        return oasp_mdb_runtime_result(oasp_mdb_errorf("Error", error));
    }
    return oasp_mdb_runtime_result(oasp_mdb_strdup_or_empty(""));
}
