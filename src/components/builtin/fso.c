// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "builtin_internal.h"
#include "component_internal.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static oasp_value fso_slice_value(oasp_runtime *runtime, const char *data, size_t length) {
    char *copy = oasp_runtime_copy(runtime, data, length);
    return copy ? oasp_value_string(copy, length) : oasp_value_empty();
}

static void fso_text_file_finalize(void *context) {
    oasp_text_file *text_file = context;
    if (text_file->file)
        fclose(text_file->file);
    text_file->file = NULL;
}

char *oasp_builtin_host_path(oasp_runtime *runtime, const char *path) {
    size_t length = strlen(path);
    char *host = oasp_runtime_copy(runtime, path, length);
    if (!host)
        return NULL;
    for (size_t i = 0; i < length; i++)
        if (host[i] == '\\')
            host[i] = '/';
    return host;
}

char *oasp_builtin_case_path(oasp_runtime *runtime, const char *path) {
    char *host = oasp_builtin_host_path(runtime, path);
    if (!host || access(host, F_OK) == 0)
        return host;

    char resolved[PATH_MAX];
    size_t used = 0;
    const char *cursor = host;
    if (*cursor == '/') {
        resolved[used++] = '/';
        cursor++;
    }
    while (*cursor) {
        const char *slash = strchr(cursor, '/');
        size_t segment_length = slash ? (size_t)(slash - cursor) : strlen(cursor);
        if (!segment_length || (segment_length == 1 && cursor[0] == '.')) {
            cursor = slash ? slash + 1 : cursor + segment_length;
            continue;
        }
        if (segment_length == 2 && cursor[0] == '.' && cursor[1] == '.') {
            if (used > 1) {
                if (resolved[used - 1] == '/')
                    used--;
                while (used > 1 && resolved[used - 1] != '/')
                    used--;
            }
        } else {
            char directory[PATH_MAX];
            size_t directory_length = used ? used : 1;
            if (!used)
                directory[0] = '.';
            else
                memcpy(directory, resolved, used);
            directory[directory_length] = '\0';
            DIR *entries = opendir(directory);
            const char *selected = cursor;
            size_t selected_length = segment_length;
            struct dirent *entry;
            while (entries && (entry = readdir(entries))) {
                if (oasp_ascii_equal_n(cursor, segment_length, entry->d_name)) {
                    selected = entry->d_name;
                    selected_length = strlen(entry->d_name);
                    break;
                }
            }
            if (used && resolved[used - 1] != '/')
                resolved[used++] = '/';
            if (used + selected_length >= sizeof(resolved)) {
                if (entries)
                    closedir(entries);
                return host;
            }
            memcpy(resolved + used, selected, selected_length);
            used += selected_length;
            if (entries)
                closedir(entries);
        }
        cursor = slash ? slash + 1 : cursor + segment_length;
    }
    resolved[used] = '\0';
    return oasp_runtime_copy(runtime, resolved, used);
}

static bool fso_folder_size(const char *path, uint64_t *total) {
    DIR *directory = opendir(path);
    if (!directory)
        return false;
    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char child[PATH_MAX];
        int length = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        struct stat status;
        if (length <= 0 || (size_t)length >= sizeof(child) || lstat(child, &status) != 0) {
            ok = false;
            continue;
        }
        if (S_ISDIR(status.st_mode)) {
            if (!fso_folder_size(child, total))
                ok = false;
        } else if (S_ISREG(status.st_mode)) {
            if (status.st_size < 0 || (uintmax_t)status.st_size > UINT64_MAX - *total) {
                ok = false;
                continue;
            }
            *total += (uint64_t)status.st_size;
        }
    }
    closedir(directory);
    return ok;
}

static oasp_value fso_date_value(oasp_runtime *runtime, time_t timestamp) {
    struct tm parts;
    struct tm *local = localtime(&timestamp);
    if (!local)
        return oasp_value_empty();
    parts = *local;
    char text[32];
    int length = snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
                          parts.tm_hour, parts.tm_min, parts.tm_sec);
    oasp_value result = oasp_value_empty();
    result.kind = OASP_VALUE_DATE;
    result.as.date.data = oasp_runtime_copy(runtime, text, (size_t)length);
    result.as.date.length = (size_t)length;
    return result;
}

static oasp_value fso_file_object(oasp_runtime *runtime, const char *path, oasp_component_kind kind, size_t line) {
    char *host = oasp_builtin_case_path(runtime, path);
    struct stat status;
    if (!host || stat(host, &status) != 0) {
        oasp_runtime_raise(runtime, 53, "file not found", "Scripting.FileSystemObject", line);
        return oasp_value_empty();
    }
    char canonical[PATH_MAX];
    if (kind == OASP_COMPONENT_FOLDER) {
        char *resolved = realpath(host, canonical);
        if (resolved)
            host = canonical;
    }
    oasp_file_info *info = oasp_runtime_alloc(runtime, sizeof(*info), _Alignof(oasp_file_info));
    if (!info)
        return oasp_value_empty();
    info->path.data = oasp_runtime_copy(runtime, host, strlen(host));
    info->path.length = strlen(host);
    info->modified = status.st_mtime;
    info->size = status.st_size;
    oasp_object *object = oasp_component_object(runtime, kind, kind == OASP_COMPONENT_FILE ? "Scripting.File" : "Scripting.Folder", info);
    return object ? oasp_component_object_value(object) : oasp_value_empty();
}

static oasp_value fso_folder_collection(oasp_runtime *runtime, const oasp_file_info *folder, bool directories, size_t line) {
    DIR *directory = opendir(folder->path.data);
    if (!directory)
        return oasp_value_empty();
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char path[PATH_MAX];
        int length = snprintf(path, sizeof(path), "%s/%s", folder->path.data, entry->d_name);
        struct stat status;
        if (length > 0 && (size_t)length < sizeof(path) && stat(path, &status) == 0 &&
            (directories ? S_ISDIR(status.st_mode) : S_ISREG(status.st_mode)))
            count++;
    }
    rewinddir(directory);
    oasp_component_collection *collection = oasp_runtime_alloc(runtime, sizeof(*collection), _Alignof(oasp_component_collection));
    if (!collection) {
        closedir(directory);
        return oasp_value_empty();
    }
    collection->count = count;
    collection->items = count ? oasp_runtime_alloc_values(runtime, count) : NULL;
    if (count && !collection->items) {
        closedir(directory);
        return oasp_value_empty();
    }
    size_t index = 0;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char path[PATH_MAX];
        int length = snprintf(path, sizeof(path), "%s/%s", folder->path.data, entry->d_name);
        struct stat status;
        if (length <= 0 || (size_t)length >= sizeof(path) || stat(path, &status) != 0 ||
            (directories ? !S_ISDIR(status.st_mode) : !S_ISREG(status.st_mode)))
            continue;
        collection->items[index++] = fso_file_object(runtime, path, directories ? OASP_COMPONENT_FOLDER : OASP_COMPONENT_FILE, line);
    }
    closedir(directory);
    oasp_object *object = oasp_component_object(runtime, directories ? OASP_COMPONENT_FOLDER_COLLECTION : OASP_COMPONENT_FILE_COLLECTION,
                                                directories ? "Scripting.Folders" : "Scripting.Files", collection);
    return object ? oasp_component_object_value(object) : oasp_value_empty();
}

static bool fso_text_file_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                               size_t line, oasp_value *result) {
    if (object->component_kind != OASP_COMPONENT_TEXT_FILE)
        return false;
    oasp_text_file *text_file = object->native;
    if (oasp_ascii_equal(member, "close")) {
        if (text_file->file)
            fclose(text_file->file);
        text_file->file = NULL;
        *result = oasp_value_empty();
        return true;
    }
    if (oasp_ascii_equal(member, "write") || oasp_ascii_equal(member, "writeline")) {
        char *text = argument_count ? oasp_value_format(runtime->arena, &arguments[0]) : "";
        if (!text_file->file || text_file->mode != 2)
            oasp_runtime_raise(runtime, 54, "bad file mode", object->component_name, line);
        else {
            size_t length = strlen(text);
            bool line_write = oasp_ascii_equal(member, "writeline");
            bool written = fwrite(text, 1, length, text_file->file) == length;
            if (written && line_write)
                written = fwrite("\r\n", 1, 2, text_file->file) == 2;
            if (!written || fflush(text_file->file) == EOF)
                oasp_runtime_raise(runtime, 70, "text file write failed", object->component_name, line);
        }
        *result = oasp_value_empty();
        return true;
    }
    if (oasp_ascii_equal(member, "readall")) {
        oasp_buffer buffer;
        if (!oasp_buffer_init(&buffer, 4096)) {
            *result = oasp_value_empty();
            return true;
        }
        char chunk[4096];
        size_t count;
        while (text_file->file && (count = fread(chunk, 1, sizeof(chunk), text_file->file)) != 0)
            (void)oasp_buffer_append(&buffer, chunk, count);
        *result = fso_slice_value(runtime, buffer.data, buffer.length);
        oasp_buffer_dispose(&buffer);
        return true;
    }
    return false;
}

bool oasp_builtin_fso_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                           size_t line, oasp_value *result) {
    if (!object)
        return false;
    if (fso_text_file_call(runtime, object, member, arguments, argument_count, line, result))
        return true;
    if (object->component_kind != OASP_COMPONENT_FSO)
        return false;
    char *path = argument_count ? oasp_value_format(runtime->arena, &arguments[0]) : NULL;
    if (oasp_ascii_equal(member, "fileexists") || oasp_ascii_equal(member, "folderexists")) {
        char *host = path ? oasp_builtin_case_path(runtime, path) : NULL;
        struct stat status;
        bool exists = host && stat(host, &status) == 0;
        if (exists)
            exists = oasp_ascii_equal(member, "fileexists") ? S_ISREG(status.st_mode) : S_ISDIR(status.st_mode);
        *result = oasp_value_bool(exists);
        return true;
    }
    if (oasp_ascii_equal(member, "deletefile")) {
        char *host = path ? oasp_builtin_case_path(runtime, path) : NULL;
        if (!host || unlink(host) != 0)
            oasp_runtime_raise(runtime, errno == ENOENT ? 53 : 70, errno == ENOENT ? "file not found" : "permission denied", object->component_name, line);
        *result = oasp_value_empty();
        return true;
    }
    if (oasp_ascii_equal(member, "getfile") || oasp_ascii_equal(member, "getfolder")) {
        *result = path ? fso_file_object(runtime, path, oasp_ascii_equal(member, "getfile") ? OASP_COMPONENT_FILE : OASP_COMPONENT_FOLDER, line)
                       : oasp_value_empty();
        return true;
    }
    if (!oasp_ascii_equal(member, "createtextfile") && !oasp_ascii_equal(member, "opentextfile"))
        return false;

    bool create = oasp_ascii_equal(member, "createtextfile");
    bool writable = create;
    bool create_missing = false;
    bool overwrite = create && (argument_count <= 1 || oasp_value_truthy(&arguments[1]));
    int64_t io_mode = 1;
    const char *mode = overwrite ? "wb" : "wbx";
    if (!create) {
        if (argument_count > 1 && !oasp_value_to_int(&arguments[1], &io_mode))
            io_mode = 0;
        create_missing = argument_count > 2 && oasp_value_truthy(&arguments[2]);
        writable = io_mode == 2 || io_mode == 8;
        mode = io_mode == 1 ? "rb" : io_mode == 2 ? "wb" : io_mode == 8 ? "ab" : NULL;
        if (!mode) {
            oasp_runtime_raise(runtime, 5, "invalid file mode", object->component_name, line);
            *result = oasp_value_empty();
            return true;
        }
    }
    char *host = path ? ((create || (writable && create_missing)) ? oasp_builtin_host_path(runtime, path)
                                                                   : oasp_builtin_case_path(runtime, path))
                      : NULL;
    FILE *file = host ? fopen(host, mode) : NULL;
    if (!file) {
        oasp_runtime_raise(runtime, errno == ENOENT ? 53 : 70, errno == ENOENT ? "file not found" : "cannot open text file",
                           object->component_name, line);
        *result = oasp_value_empty();
        return true;
    }
    oasp_text_file *text_file = oasp_runtime_alloc(runtime, sizeof(*text_file), _Alignof(oasp_text_file));
    if (!text_file) {
        fclose(file);
        *result = oasp_value_empty();
        return true;
    }
    text_file->file = file;
    text_file->mode = writable ? 2 : 1;
    if (!oasp_runtime_defer(runtime, text_file, fso_text_file_finalize, text_file)) {
        fclose(file);
        text_file->file = NULL;
        *result = oasp_value_empty();
        return true;
    }
    oasp_object *text_object = oasp_component_object(runtime, OASP_COMPONENT_TEXT_FILE, "Scripting.TextStream", text_file);
    *result = text_object ? oasp_component_object_value(text_object) : oasp_value_empty();
    return true;
}

bool oasp_builtin_fso_get(oasp_runtime *runtime, oasp_object *object, oasp_slice member, size_t line, oasp_value *result) {
    if (!object)
        return false;
    if (object->component_kind == OASP_COMPONENT_TEXT_FILE &&
        (oasp_ascii_equal(member, "close") || oasp_ascii_equal(member, "readall")))
        return oasp_builtin_fso_call(runtime, object, member, NULL, 0, line, result);
    if (object->component_kind == OASP_COMPONENT_FILE || object->component_kind == OASP_COMPONENT_FOLDER) {
        oasp_file_info *info = object->native;
        if (oasp_ascii_equal(member, "path"))
            *result = oasp_value_string(info->path.data, info->path.length);
        else if (oasp_ascii_equal(member, "name")) {
            const char *slash = strrchr(info->path.data, '/');
            const char *name = slash ? slash + 1 : info->path.data;
            *result = oasp_value_string(name, strlen(name));
        } else if (oasp_ascii_equal(member, "datelastmodified") || oasp_ascii_equal(member, "datecreated"))
            *result = fso_date_value(runtime, info->modified);
        else if (oasp_ascii_equal(member, "size")) {
            uint64_t size = object->component_kind == OASP_COMPONENT_FOLDER ? 0 : (info->size < 0 ? 0 : (uint64_t)info->size);
            if (object->component_kind == OASP_COMPONENT_FOLDER)
                (void)fso_folder_size(info->path.data, &size);
            *result = oasp_value_int((int64_t)size);
        } else if (object->component_kind == OASP_COMPONENT_FOLDER && oasp_ascii_equal(member, "files"))
            *result = fso_folder_collection(runtime, info, false, line);
        else if (object->component_kind == OASP_COMPONENT_FOLDER && (oasp_ascii_equal(member, "subfolders") || oasp_ascii_equal(member, "folders")))
            *result = fso_folder_collection(runtime, info, true, line);
        else
            return false;
        return true;
    }
    if (object->component_kind == OASP_COMPONENT_FILE_COLLECTION || object->component_kind == OASP_COMPONENT_FOLDER_COLLECTION) {
        oasp_component_collection *collection = object->native;
        if (oasp_ascii_equal(member, "count") || oasp_ascii_equal(member, "length")) {
            *result = oasp_value_int((int64_t)collection->count);
            return true;
        }
    }
    return false;
}
