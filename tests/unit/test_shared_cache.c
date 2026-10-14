// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

/*
 * Module overview (`tests/test_shared_cache.c`): Native regression test. Local fixtures
 * construct the smallest state needed to exercise ownership, limits, persistence, and failure
 * paths; assertions document the externally observable contract and guard against silent
 * regressions.
 * Keep future changes inside these invariants and extend focused tests whenever ownership,
 * limits, or external behavior changes.
 */

#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 700

#include "bytecode.h"
#include "module_image.h"
#include "parser.h"
#include "runtime.h"
#include "shared_cache.h"

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int64_t modified_nanoseconds(const struct stat *metadata) {
#if defined(__APPLE__)
    return (int64_t)metadata->st_mtimespec.tv_nsec;
#else
    return (int64_t)metadata->st_mtim.tv_nsec;
#endif
}

static int64_t status_nanoseconds(const struct stat *metadata) {
#if defined(__APPLE__)
    return (int64_t)metadata->st_ctimespec.tv_nsec;
#else
    return (int64_t)metadata->st_ctim.tv_nsec;
#endif
}

static uint64_t content_hash(const void *data, size_t length) {
    const unsigned char *bytes = data;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool contains_bytes(const void *haystack, size_t haystack_length, const void *needle, size_t needle_length) {
    if (!needle_length)
        return true;
    if (needle_length > haystack_length)
        return false;
    const unsigned char *bytes = haystack;
    for (size_t i = 0; i <= haystack_length - needle_length; i++)
        if (!memcmp(bytes + i, needle, needle_length))
            return true;
    return false;
}

static void remove_directory(const char *path) {
    DIR *directory = opendir(path);
    assert(directory);
    struct dirent *entry;
    char child[4096];
    while ((entry = readdir(directory)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        int length = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        assert(length > 0 && (size_t)length < sizeof(child));
        assert(unlink(child) == 0);
    }
    closedir(directory);
    assert(rmdir(path) == 0);
}

int main(void) {
    char directory[] = "/tmp/openasp-cache-test-XXXXXX";
    assert(mkdtemp(directory));
    char path[4096];
    int path_length = snprintf(path, sizeof(path), "%s/module.inc", directory);
    assert(path_length > 0 && (size_t)path_length < sizeof(path));

    static const char body[] = "\nDim matrix(2, 3)\n"
                               "Const greeting = \"hi\"\n"
                               "Function Add(ByVal a, ByRef b)\n"
                               "On Error Resume Next\n"
                               "If a > 0 Then\n"
                               "Add = a + b\n"
                               "Else\n"
                               "Add = 0\n"
                               "End If\n"
                               "On Error GoTo 0\n"
                               "End Function\n"
                               "Class Counter\n"
                               "Private total\n"
                               "Public Function Value()\n"
                               "Value = 7\n"
                               "End Function\n"
                               "End Class\n"
                               "On Error Resume Next\n"
                               "On Error GoTo 0\n"
                               "Select Case greeting\n"
                               "Case \"hi\", \"hey\"\n"
                               "value = Add(1, 2)\n"
                               "Case Else\n"
                               "value = 0\n"
                               "End Select\n";
    const size_t padding = 120000;
    const size_t body_length = sizeof(body) - 1;
    char *script = malloc(padding + body_length + 1);
    assert(script);
    script[0] = '\'';
    memset(script + 1, 'x', padding);
    memcpy(script + padding + 1, body, body_length);
    size_t script_length = padding + 1 + body_length;
    FILE *source = fopen(path, "wb");
    assert(source);
    assert(fwrite(script, 1, script_length, source) == script_length);
    assert(fclose(source) == 0);

    oasp_arena *arena = oasp_arena_create(16384);
    assert(arena);
    oasp_parser parser;
    oasp_block program;
    oasp_bytecode bytecode;
    assert(oasp_parser_init(&parser, arena, script, script_length));
    assert(oasp_parse_program(&parser, &program));
    oasp_parser_dispose(&parser);
    assert(oasp_bytecode_compile(arena, &program, &bytecode));
    struct stat metadata;
    assert(stat(path, &metadata) == 0);
    oasp_preprocess_dependency dependency = {.path = path,
                                             .modified_seconds = (int64_t)metadata.st_mtime,
                                             .modified_nanoseconds = modified_nanoseconds(&metadata),
                                             .status_seconds = (int64_t)metadata.st_ctime,
                                             .status_nanoseconds = status_nanoseconds(&metadata),
                                             .file_size = (int64_t)metadata.st_size,
                                             .content_hash = content_hash(script, script_length)};
    char *modules[] = {path};
    oasp_module_image_metadata image_metadata = {
        .path = path,
        .dependencies = &dependency,
        .dependency_count = 1,
        .modules = modules,
        .module_count = 1,
    };
    oasp_buffer image;
    assert(oasp_module_image_encode(&bytecode, &image_metadata, &image));
    assert(sizeof(oasp_image_instruction) == 24);
    assert(image.length < script_length);
    assert(!contains_bytes(image.data, image.length, script, 64));
    oasp_module_image decoded_without_debug;
    assert(oasp_module_image_decode(NULL, image.data, image.length, &decoded_without_debug));
    assert(!decoded_without_debug.has_debug_info);
    oasp_slice missing_debug_source;
    assert(!oasp_module_image_debug_source(&decoded_without_debug, &missing_debug_source));
    oasp_module_image_dispose(&decoded_without_debug);

    oasp_slice debug_source = {script, script_length};
    image_metadata.debug_source = &debug_source;
    oasp_buffer debug_image;
    assert(oasp_module_image_encode(&bytecode, &image_metadata, &debug_image));
    assert(debug_image.length > image.length);
    assert(contains_bytes(debug_image.data, debug_image.length, script, 64));
    oasp_module_image decoded_with_debug;
    assert(oasp_module_image_decode(NULL, debug_image.data, debug_image.length, &decoded_with_debug));
    assert(decoded_with_debug.has_debug_info);
    oasp_slice decoded_debug_source;
    assert(oasp_module_image_debug_source(&decoded_with_debug, &decoded_debug_source));
    assert(decoded_debug_source.length == script_length);
    assert(!memcmp(decoded_debug_source.data, script, script_length));
    oasp_module_image_dispose(&decoded_with_debug);
    oasp_buffer_dispose(&debug_image);
    image_metadata.debug_source = NULL;

    unsigned char *damaged = malloc(image.length);
    assert(damaged);
    memcpy(damaged, image.data, image.length);
    damaged[image.length - 1] ^= 1u;
    assert(oasp_module_image_fresh(damaged, image.length, path));
    assert(!oasp_module_image_valid(damaged, image.length, path));
    free(damaged);

    oasp_shared_cache *cache = oasp_shared_cache_create(image.length + image.length / 2, directory);
    assert(cache);
    uint64_t version = 0;
    assert(oasp_shared_cache_publish(cache, path, image.data, image.length, 0, &version));
    assert(version != 0);
    oasp_shared_cache_view held;
    assert(oasp_shared_cache_acquire(cache, path, &held, NULL));

    if (getenv("OPENASP_TEST_NO_FORK")) {
        oasp_shared_cache_release(cache, &held);
        assert(oasp_shared_cache_publish(cache, path, image.data, image.length, version, NULL));
    } else {
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            bool ok = oasp_shared_cache_publish(cache, path, image.data, image.length, version, NULL);
            _exit(ok ? 0 : 1);
        }
        struct timespec delay = {0, 50000000};
        assert(nanosleep(&delay, NULL) == 0);
        int status = 0;
        pid_t completed = waitpid(child, &status, WNOHANG);
        oasp_shared_cache_release(cache, &held);
        if (!completed)
            assert(waitpid(child, &status, 0) == child);
        else
            assert(completed == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    assert(oasp_shared_cache_generation(cache) >= 1);

    if (!getenv("OPENASP_TEST_NO_FORK")) {
        oasp_shared_cache_view version_view;
        uint64_t current_version = 0;
        assert(oasp_shared_cache_acquire(cache, path, &version_view, &current_version));
        oasp_shared_cache_release(cache, &version_view);
        int ready_pipe[2];
        assert(pipe(ready_pipe) == 0);
        pid_t reader = fork();
        assert(reader >= 0);
        if (reader == 0) {
            close(ready_pipe[0]);
            oasp_shared_cache_view leaked;
            bool acquired = oasp_shared_cache_acquire(cache, path, &leaked, NULL);
            char ready = acquired ? '1' : '0';
            (void)write(ready_pipe[1], &ready, 1);
            pause();
            _exit(1);
        }
        close(ready_pipe[1]);
        char ready = 0;
        assert(read(ready_pipe[0], &ready, 1) == 1 && ready == '1');
        close(ready_pipe[0]);
        assert(kill(reader, SIGKILL) == 0);
        int status = 0;
        assert(waitpid(reader, &status, 0) == reader);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        oasp_shared_cache_recover_process(cache, (int64_t)reader);
        assert(oasp_shared_cache_publish(cache, path, image.data, image.length, current_version, NULL));
    }

    oasp_shared_cache_view current;
    assert(oasp_shared_cache_acquire(cache, path, &current, NULL));
    oasp_arena *decoded_arena = oasp_arena_create(16384);
    oasp_module_image decoded;
    assert(decoded_arena);
    size_t decoded_arena_used = oasp_arena_used(decoded_arena);
    assert(oasp_module_image_decode(decoded_arena, current.data, current.size, &decoded));
    assert(oasp_arena_used(decoded_arena) == decoded_arena_used);
    assert(decoded.bytecode.length == bytecode.length);
    assert(decoded.bytecode.instructions != NULL);
    assert(decoded.bytecode.name_count == bytecode.name_count);
    assert(decoded.bytecode.constant_count == bytecode.constant_count);
    assert(decoded.bytecode.image_decode_storage == NULL);
    assert(decoded.bytecode.names == NULL);
    assert(decoded.bytecode.constants == NULL);
    assert(decoded.bytecode.global_slot_names == NULL);
    assert(decoded.bytecode.routines == NULL);
    assert(decoded.bytecode.classes == NULL);
    assert(bytecode.root_error_aware);
    assert(decoded.bytecode.root_error_aware);
    assert(decoded.module_count == 1);
    oasp_slice decoded_module;
    assert(oasp_module_image_module_at(&decoded, 0, &decoded_module));
    assert(decoded_module.length == strlen(path));
    assert(!memcmp(decoded_module.data, path, decoded_module.length));
    assert((const unsigned char *)decoded_module.data >= (const unsigned char *)current.data);
    assert((const unsigned char *)decoded_module.data + decoded_module.length <=
           (const unsigned char *)current.data + current.size);
    oasp_request *request = oasp_request_create(directory, "", "", "GET");
    assert(request);
    oasp_runtime runtime;
    oasp_runtime_init(&runtime, request);
    assert(oasp_runtime_execute_bytecode(&runtime, &decoded.bytecode));
    oasp_slice value_name = {"value", 5};
    const oasp_value *direct_value = oasp_runtime_get(&runtime, value_name);
    assert(direct_value && direct_value->kind == OASP_VALUE_INT && direct_value->as.integer == 3);
    oasp_request_destroy(request);
    assert(decoded.bytecode.routine_count == 2);
    assert(decoded.bytecode.class_count == 1);
    oasp_routine_bytecode decoded_routine;
    oasp_class_bytecode decoded_class;
    assert(oasp_bytecode_routine_at(&decoded.bytecode, 0, &decoded_routine));
    assert(oasp_bytecode_class_at(&decoded.bytecode, 0, &decoded_class));
    assert(bytecode.routines[0].error_aware);
    assert(decoded_routine.error_aware);
    assert(decoded_routine.name.length != 0);
    assert(decoded_class.name.length != 0);
    assert(decoded_class.routine_count == 1);
    assert((const unsigned char *)decoded_routine.name.data >= (const unsigned char *)current.data);
    assert((const unsigned char *)decoded_routine.name.data + decoded_routine.name.length <=
           (const unsigned char *)current.data + current.size);
    for (size_t i = 0; i < bytecode.length; i++) {
        const oasp_instruction *shared = oasp_bytecode_instruction_at(&decoded.bytecode, i);
        assert(shared);
        assert(shared->opcode == bytecode.instructions[i].opcode);
        assert(shared->flags == bytecode.instructions[i].flags);
        assert(shared->reserved == bytecode.instructions[i].reserved);
        assert(shared->line == bytecode.instructions[i].line);
        assert(shared->target == bytecode.instructions[i].target);
        assert(shared->error_target == bytecode.instructions[i].error_target);
        assert(shared->operand == bytecode.instructions[i].operand);
        assert(shared->slot == bytecode.instructions[i].slot);
    }
    oasp_module_image_dispose(&decoded);
    oasp_arena_destroy(decoded_arena);
    oasp_shared_cache_release(cache, &current);
    oasp_shared_cache_destroy(cache);

    cache = oasp_shared_cache_create(image.length + image.length / 2, directory);
    assert(cache);
    void *disk_image = NULL;
    size_t disk_size = 0;
    assert(oasp_shared_cache_disk_load(cache, path, &disk_image, &disk_size));
    assert(oasp_module_image_valid(disk_image, disk_size, path));
    unsigned char *unaligned = malloc(disk_size + 1);
    assert(unaligned);
    memcpy(unaligned + 1, disk_image, disk_size);
    assert(!oasp_module_image_valid(unaligned + 1, disk_size, path));
    oasp_module_image rejected;
    assert(!oasp_module_image_decode(NULL, unaligned + 1, disk_size, &rejected));
    free(unaligned);
    free(disk_image);
    oasp_shared_cache_destroy(cache);

    struct timespec preserved_mtime[2];
#if defined(__APPLE__)
    preserved_mtime[0] = metadata.st_atimespec;
    preserved_mtime[1] = metadata.st_mtimespec;
#else
    preserved_mtime[0] = metadata.st_atim;
    preserved_mtime[1] = metadata.st_mtim;
#endif
    script[padding + 1] = script[padding + 1] == 'x' ? 'y' : 'x';
    source = fopen(path, "wb");
    assert(source);
    assert(fwrite(script, 1, script_length, source) == script_length);
    assert(fclose(source) == 0);
    assert(utimensat(AT_FDCWD, path, preserved_mtime, 0) == 0);
    assert(stat(path, &metadata) == 0);
    assert((int64_t)metadata.st_mtime == dependency.modified_seconds);
    assert(modified_nanoseconds(&metadata) == dependency.modified_nanoseconds);
    assert((int64_t)metadata.st_size == dependency.file_size);
    assert((int64_t)metadata.st_ctime != dependency.status_seconds ||
           status_nanoseconds(&metadata) != dependency.status_nanoseconds);
    cache = oasp_shared_cache_create(image.length + image.length / 2, directory);
    assert(cache);
    assert(!oasp_shared_cache_disk_load(cache, path, &disk_image, &disk_size));
    oasp_shared_cache_destroy(cache);

    oasp_buffer_dispose(&image);
    oasp_arena_destroy(arena);
    free(script);
    assert(unlink(path) == 0);
    remove_directory(directory);
    return 0;
}
