// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Interpreter state and internal execution API. A runtime may use request
 * arenas for short-lived rendering or the generational allocator for a
 * persistent service. Any managed pointer held across an allocation must be
 * reachable from these structures or an explicit oasp_gc_root, and every old
 * object mutation must pass through the write barrier helpers below.
 */

#ifndef OPENASP_RUNTIME_H
#define OPENASP_RUNTIME_H

#include "allocator.h"
#include "parser.h"

#include <sys/types.h>

typedef struct oasp_binding {
    /* Case-insensitive linked symbol entry or slot-backed local variable. */
    struct oasp_binding *next;
    oasp_slice name;
    oasp_value value;
    /* By-reference/copy-back metadata used at routine return. */
    struct oasp_binding *reference;
    const oasp_expr *copyback_target;
    bool dirty;
    bool constant;
} oasp_binding;

typedef struct oasp_bytecode oasp_bytecode;
typedef struct oasp_routine_bytecode oasp_routine_bytecode;
typedef struct oasp_class_bytecode oasp_class_bytecode;
typedef struct oasp_component_context oasp_component_context;
typedef struct oasp_runtime oasp_runtime;
typedef struct oasp_string_builder oasp_string_builder;
typedef struct oasp_gc_root oasp_gc_root;
typedef struct oasp_fork_child oasp_fork_child;
typedef struct oasp_memory_manager oasp_memory_manager;
typedef struct oasp_debugger oasp_debugger;
typedef struct oasp_component_vtable oasp_component_vtable;
typedef struct {
    const char *output_path;
    bool gen_debug_info;
} oasp_oac_options;

struct oasp_gc_root {
    /* Stack-disciplined root chain installed around allocation-capable helpers. */
    oasp_gc_root *previous;
    oasp_value *values;
    size_t value_count;
    oasp_object **objects;
    size_t object_count;
    oasp_array **arrays;
    size_t array_count;
};
typedef bool (*oasp_runtime_include_handler)(void *context, oasp_runtime *runtime, oasp_slice path);

typedef struct {
    /* Last recordset/column lookup, avoiding repeated name-to-ordinal scans. */
    const oasp_object *recordset;
    const char *name;
    size_t name_length;
    const void *schema;
    int ordinal;
    bool valid;
} oasp_recordset_ordinal_cache;

typedef struct oasp_routine_def {
    const oasp_bytecode *bytecode;
    size_t routine_index;
    uint32_t symbol_id;
    uint32_t reserved;
} oasp_routine_def;

typedef struct {
    oasp_routine_def *items;
    size_t count;
    size_t capacity;
} oasp_routine_table;

typedef struct oasp_class_def {
    struct oasp_class_def *next;
    oasp_slice name;
    const oasp_bytecode *bytecode;
    size_t class_index;
    bool resolved;
} oasp_class_def;

typedef struct oasp_frame {
    /* Dynamic call chain plus named and resolved-slot local storage. */
    struct oasp_frame *parent;
    oasp_binding *locals;
    oasp_binding *slots;
    size_t slot_count;
    /* Routine/class execution context and frame-owned string builders. */
    oasp_object *object;
    const oasp_bytecode *routine_bytecode;
    const oasp_routine_bytecode *routine;
    oasp_string_builder *string_builders;
    /* Last debugger-visible instruction in this suspended call frame. */
    const oasp_bytecode *debug_bytecode;
    size_t debug_pc;
    bool exit_routine;
    bool transient;
} oasp_frame;

typedef enum {
    OASP_COMPONENT_NONE,
    OASP_COMPONENT_DICTIONARY,
    OASP_COMPONENT_FSO,
    OASP_COMPONENT_REGEXP,
    OASP_COMPONENT_STREAM,
    OASP_COMPONENT_XML,
    OASP_COMPONENT_HTTP,
    OASP_COMPONENT_HTTP_MULTI,
    OASP_COMPONENT_ADO_CONNECTION,
    OASP_COMPONENT_ADO_RECORDSET,
    OASP_COMPONENT_ADO_FIELDS,
    OASP_COMPONENT_ADO_FIELD,
    OASP_COMPONENT_ADO_COMMAND,
    OASP_COMPONENT_ADO_PARAMETERS,
    OASP_COMPONENT_ADO_PARAMETER,
    OASP_COMPONENT_SOCKET,
    OASP_COMPONENT_REDIS,
    OASP_COMPONENT_REDIS_MULTI,
    OASP_COMPONENT_REDIS_MULTI_CLIENT,
    OASP_COMPONENT_REDIS_MULTI_COMMAND,
    OASP_COMPONENT_JSON,
    OASP_COMPONENT_CRYPTO,
    OASP_COMPONENT_OPENAI,
    OASP_COMPONENT_OPENAI_STREAM,
    OASP_COMPONENT_PROCESS,
    OASP_COMPONENT_WEBSOCKET,
    OASP_COMPONENT_MEMCACHE,
    OASP_COMPONENT_MEMCACHE_MULTI,
    OASP_COMPONENT_MEMCACHE_MULTI_CLIENT,
    OASP_COMPONENT_MEMCACHE_MULTI_COMMAND,
    OASP_COMPONENT_SHELL,
    OASP_COMPONENT_MATCH_COLLECTION,
    OASP_COMPONENT_MATCH,
    OASP_COMPONENT_SUBMATCH_COLLECTION,
    OASP_COMPONENT_TEXT_FILE,
    OASP_COMPONENT_FILE,
    OASP_COMPONENT_FOLDER,
    OASP_COMPONENT_FILE_COLLECTION,
    OASP_COMPONENT_FOLDER_COLLECTION,
    OASP_COMPONENT_XML_NODE,
    OASP_COMPONENT_XML_ATTRIBUTE_MAP,
    OASP_COMPONENT_XML_NODE_LIST,
    OASP_COMPONENT_XML_PARSE_ERROR,
    OASP_COMPONENT_EXTERNAL
} oasp_component_kind;

struct oasp_object {
    /* Script classes use fields; native components use kind/name/native. */
    oasp_class_def *class_def;
    oasp_binding *fields;
    oasp_component_kind component_kind;
    const char *component_name;
    void *native;
    const oasp_component_vtable *component_vtable;
};

struct oasp_runtime {
    /* Borrowed request resources and the selected allocation backend. */
    oasp_request *request;
    oasp_arena *arena;
    oasp_arena *scratch;
    oasp_arena *format_arena;
    oasp_allocator *allocator;
    oasp_memory_manager *memory;
    oasp_gc_root *gc_roots;
    oasp_component_context *component_context;
    void *external_request_contexts;
    oasp_fork_child *fork_children;
    /* Registered globals, routines, classes, active frame, and With receiver. */
    oasp_binding *variables;
    oasp_routine_table *routines;
    oasp_class_def *classes;
    oasp_frame *frame;
    const oasp_bytecode *bytecode;
    const oasp_bytecode *global_slot_bytecode;
    oasp_binding **global_slots;
    size_t global_slot_count;
    oasp_object *with_object;
    /* Request/session/application compatibility state and read cursors. */
    oasp_binding *session;
    oasp_binding *application;
    oasp_string_builder *string_builders;
    oasp_recordset_ordinal_cache recordset_ordinals[16];
    size_t recordset_ordinal_next;
    int session_timeout;
    bool session_abandoned;
    size_t binary_read_offset;
    /* Structured interpreter control flow and first active ASP error. */
    bool option_explicit;
    bool resume_next;
    bool stopped;
    int loop_exit;
    int error_number;
    const char *error_description;
    const char *error_source;
    size_t error_line;
    uint64_t error_generation;
    bool error_pending;
    /* Include callbacks are borrowed; depth and safepoints bound recursion/work. */
    oasp_runtime_include_handler include_handler;
    oasp_runtime_include_handler transfer_handler;
    void *include_context;
    size_t include_depth;
    uint32_t safepoint_counter;
    /* Optional VM debugger and current normalized source view. */
    const oasp_debugger *debugger;
    const char *debug_source_name;
    oasp_slice debug_source;
    const oasp_bytecode *debug_root_bytecode;
    size_t debug_root_pc;
    bool debugger_aborted;
};

/*
 * Initializes an arena-backed request runtime or an owning GC-backed service
 * runtime. `request` and optional `config` are borrowed. dispose releases
 * native component state and an owned allocator but never destroys `request`.
 */
void oasp_runtime_init(oasp_runtime *runtime, oasp_request *request);
bool oasp_runtime_init_service(oasp_runtime *runtime, oasp_request *request, const oasp_gc_config *config);
void oasp_runtime_dispose(oasp_runtime *runtime);

/*
 * Allocation helpers centralize alignment, object tracing, zero-initialization,
 * finalizers, external-byte accounting, and barriers. Returned memory belongs
 * to the runtime allocator and must not be freed directly.
 */
void *oasp_runtime_alloc(oasp_runtime *runtime, size_t size, size_t alignment);
void *oasp_runtime_alloc_values(oasp_runtime *runtime, size_t count);
oasp_binding *oasp_runtime_alloc_binding(oasp_runtime *runtime);
oasp_array *oasp_runtime_alloc_array(oasp_runtime *runtime);
oasp_object *oasp_runtime_alloc_object(oasp_runtime *runtime);
oasp_class_def *oasp_runtime_alloc_class_def(oasp_runtime *runtime);
char *oasp_runtime_copy(oasp_runtime *runtime, const char *data, size_t length);
char *oasp_runtime_lower(oasp_runtime *runtime, const char *data, size_t length);
bool oasp_runtime_defer(oasp_runtime *runtime, void *owner, oasp_finalizer_fn function, void *context);
bool oasp_runtime_account_external(oasp_runtime *runtime, void *owner, ptrdiff_t delta);
void oasp_runtime_note_external_pressure(oasp_runtime *runtime, size_t bytes);
void oasp_runtime_write_barrier(oasp_runtime *runtime, void *owner, const oasp_value *value);
oasp_value oasp_runtime_value_copy(oasp_runtime *runtime, const oasp_value *value, bool copy_text);
bool oasp_runtime_register_fork_child(oasp_runtime *runtime, pid_t pid);
void oasp_runtime_reset_fork_children(oasp_runtime *runtime);

/* Safepoints advance GC work and fail when the request deadline has expired. */
void oasp_runtime_gc_safepoint(oasp_runtime *runtime);
bool oasp_runtime_safepoint(oasp_runtime *runtime, size_t line);
void oasp_runtime_gc_stats(const oasp_runtime *runtime, oasp_gc_stats *stats);

/*
 * Pushes/pops temporary value, object, and array ranges onto the precise root
 * chain. Enter/leave must remain strictly nested on every control-flow path.
 */
void oasp_runtime_gc_root_enter(oasp_runtime *runtime, oasp_gc_root *root, oasp_value *values, size_t value_count, oasp_object **objects,
                                size_t object_count, oasp_array **arrays, size_t array_count);
void oasp_runtime_gc_root_leave(oasp_runtime *runtime, oasp_gc_root *root);

/* Convenience rendering and persistent Session/Application state transfer. */
int oasp_render_file_service(oasp_request *request, const char *path, const oasp_gc_config *config, oasp_gc_stats *stats);
int oasp_render_file_oac(oasp_request *request, const char *path, const oasp_oac_options *options);
int oasp_render_file_service_oac(oasp_request *request, const char *path, const oasp_gc_config *config, oasp_gc_stats *stats, const oasp_oac_options *options);
int oasp_engine_render_file_service(oasp_engine *engine, oasp_request *request, const char *path, const oasp_gc_config *config, oasp_gc_stats *stats);
int oasp_engine_render_file_oac(oasp_engine *engine, oasp_request *request, const char *path, const oasp_oac_options *options);
int oasp_engine_render_file_service_oac(oasp_engine *engine, oasp_request *request, const char *path, const oasp_gc_config *config, oasp_gc_stats *stats,
                                        const oasp_oac_options *options);
int oasp_render_file_debug(oasp_request *request, const char *path, const oasp_debugger *debugger);
int oasp_render_file_service_debug(oasp_request *request, const char *path, const oasp_gc_config *config, oasp_gc_stats *stats, const oasp_debugger *debugger);
void oasp_runtime_init_state(oasp_runtime *runtime, oasp_request *request, const oasp_binding *session, const oasp_binding *application, int session_timeout);
oasp_value oasp_runtime_persist_copy(oasp_runtime *runtime, const oasp_value *value);
oasp_binding *oasp_runtime_clone_store(oasp_runtime *runtime, const oasp_binding *store);

/*
 * Expression and call helpers evaluate operands under ASP coercion, ByRef,
 * default-member, and Resume Next rules. Argument/reference arrays are borrowed
 * for `count` entries and result values remain runtime-owned.
 */
oasp_value oasp_runtime_eval(oasp_runtime *runtime, const oasp_expr *expr);
oasp_value oasp_runtime_call_routine(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t routine_index, const oasp_value *arguments, oasp_expr *const *references,
                                     size_t count);
oasp_value oasp_runtime_call_routine_view(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t routine_index, const oasp_value *arguments, const uintptr_t *references,
                                          size_t count);
oasp_value oasp_runtime_call_name(oasp_runtime *runtime, oasp_slice name, const oasp_value *arguments, oasp_expr *const *references, size_t count,
                                  size_t line);
oasp_value oasp_runtime_call_builtin(oasp_runtime *runtime, uint16_t builtin_id, oasp_slice name, const oasp_value *arguments, size_t count,
                                     size_t line);
oasp_value oasp_runtime_call_name_id(oasp_runtime *runtime, oasp_slice name, uint32_t symbol_id, uint16_t builtin_id, uint16_t member_id,
                                     const oasp_value *arguments, oasp_expr *const *references, size_t count, size_t line);
oasp_value oasp_runtime_call_member(oasp_runtime *runtime, const oasp_value *receiver, oasp_slice member, const oasp_value *arguments, oasp_expr *const *references,
                                    size_t count, size_t line);
oasp_value oasp_runtime_call_member_id(oasp_runtime *runtime, const oasp_value *receiver, uint16_t member_id, oasp_slice member, const oasp_value *arguments,
                                       oasp_expr *const *references, size_t count, size_t line);
oasp_value oasp_runtime_call_member_view(oasp_runtime *runtime, const oasp_bytecode *bytecode, const oasp_value *receiver, uint16_t member_id, oasp_slice member,
                                         const oasp_value *arguments, const uintptr_t *references, size_t count, size_t line);
oasp_value oasp_runtime_call_default(oasp_runtime *runtime, const oasp_value *receiver, const oasp_value *arguments, oasp_expr *const *references, size_t count,
                                     size_t line);
oasp_value oasp_runtime_call_default_view(oasp_runtime *runtime, const oasp_bytecode *bytecode, const oasp_value *receiver, const oasp_value *arguments, const uintptr_t *references,
                                          size_t count, size_t line);
oasp_value oasp_runtime_call_name_view(oasp_runtime *runtime, const oasp_bytecode *bytecode, oasp_slice name, uint32_t symbol_id, uint16_t builtin_id,
                                       uint16_t member_id, const oasp_value *arguments, const uintptr_t *references, size_t count, size_t line);
oasp_value oasp_runtime_get_member(oasp_runtime *runtime, const oasp_value *receiver, oasp_slice member, size_t line);
oasp_value oasp_runtime_get_member_id(oasp_runtime *runtime, const oasp_value *receiver, uint16_t member_id, oasp_slice member, size_t line);
oasp_value oasp_runtime_get_name_id(oasp_runtime *runtime, oasp_slice name, uint16_t member_id, size_t line);
bool oasp_runtime_set_member(oasp_runtime *runtime, const oasp_value *receiver, oasp_slice member, const oasp_value *value, size_t line);
oasp_value oasp_runtime_apply_unary(oasp_runtime *runtime, oasp_expr_kind kind, const oasp_value *operand, size_t line);
oasp_value oasp_runtime_apply_binary(oasp_runtime *runtime, oasp_expr_kind kind, const oasp_value *left, const oasp_value *right, size_t line);

/* Optimized append/write paths preserve response limits and GC ownership. */
oasp_value oasp_runtime_concat_values(oasp_runtime *runtime, oasp_value *values, size_t count, size_t line);
bool oasp_runtime_append_global_slot(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t slot, const oasp_value *suffix, size_t line);
bool oasp_runtime_append_local_slot(oasp_runtime *runtime, size_t slot, const oasp_value *suffix, size_t line);
bool oasp_runtime_append_global_slot_values(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t slot, const oasp_value *suffixes, size_t count,
                                            size_t line);
bool oasp_runtime_append_local_slot_values(oasp_runtime *runtime, size_t slot, const oasp_value *suffixes, size_t count, size_t line);
bool oasp_runtime_write_value(oasp_runtime *runtime, const oasp_value *value, size_t line);
bool oasp_runtime_write_values(oasp_runtime *runtime, const oasp_value *values, size_t count, size_t line);
bool oasp_runtime_write_htmlencoded_value(oasp_runtime *runtime, const oasp_value *value, size_t line);

/* Mutation helpers enforce assignment, array shape, and Preserve semantics. */
bool oasp_runtime_assign_value(oasp_runtime *runtime, const oasp_expr *target, const oasp_value *value, size_t line);
bool oasp_runtime_declare_array(oasp_runtime *runtime, oasp_slice name, const oasp_value *bounds, size_t dimension_count, bool preserve,
                                bool declaration, size_t line);

/*
 * Execution entry points register definitions, prepare resolved global slots,
 * and interpret AST or validated bytecode until completion or runtime failure.
 */
bool oasp_runtime_execute(oasp_runtime *runtime, const oasp_block *program);
bool oasp_runtime_execute_dynamic(oasp_runtime *runtime, const oasp_block *program);
bool oasp_runtime_register_bytecode_definitions(oasp_runtime *runtime, const oasp_bytecode *bytecode);
bool oasp_runtime_prepare_bytecode_globals(oasp_runtime *runtime, const oasp_bytecode *bytecode);
bool oasp_runtime_execute_bytecode(oasp_runtime *runtime, const oasp_bytecode *bytecode);
bool oasp_runtime_execute_include(oasp_runtime *runtime, oasp_slice path);
bool oasp_runtime_execute_statement(oasp_runtime *runtime, const oasp_stmt *statement);
void oasp_runtime_raise(oasp_runtime *runtime, int number, const char *description, const char *source, size_t line);
const oasp_value *oasp_runtime_get(oasp_runtime *runtime, oasp_slice name);
bool oasp_runtime_set(oasp_runtime *runtime, oasp_slice name, const oasp_value *value, bool constant);
const oasp_value *oasp_runtime_get_global_slot(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t slot);
bool oasp_runtime_set_global_slot(oasp_runtime *runtime, const oasp_bytecode *bytecode, size_t slot, const oasp_value *value, bool constant);
const oasp_value *oasp_runtime_get_local_slot(const oasp_runtime *runtime, size_t slot);
bool oasp_runtime_set_local_slot(oasp_runtime *runtime, size_t slot, const oasp_value *value, bool constant);

#endif
