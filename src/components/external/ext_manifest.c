// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#define _POSIX_C_SOURCE 200809L

#include "ext_manifest.h"

#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static bool sha256_matches(const char *path, const char *expected) {
    if (!expected || !*expected)
        return true;
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (!context || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(context);
        fclose(file);
        return false;
    }
    unsigned char data[16384], digest[EVP_MAX_MD_SIZE];
    size_t count;
    bool valid = true;
    while ((count = fread(data, 1, sizeof(data), file)) != 0)
        if (EVP_DigestUpdate(context, data, count) != 1) {
            valid = false;
            break;
        }
    valid = valid && !ferror(file);
    fclose(file);
    unsigned int digest_length = 0;
    valid = valid && EVP_DigestFinal_ex(context, digest, &digest_length) == 1;
    EVP_MD_CTX_free(context);
    static const char hex[] = "0123456789abcdef";
    char text[EVP_MAX_MD_SIZE * 2 + 1];
    for (size_t i = 0; i < digest_length; i++) {
        text[i * 2] = hex[digest[i] >> 4];
        text[i * 2 + 1] = hex[digest[i] & 15];
    }
    text[digest_length * 2] = '\0';
    return valid && !strcmp(text, expected);
}

bool oasp_external_manifest_validate_library(const oasp_external_module_config *module, bool require_sha256, char *error, size_t error_size) {
    struct stat status;
    const char *path = module ? module->library : NULL;
    if (!path || path[0] != '/' || lstat(path, &status) || !S_ISREG(status.st_mode) || (status.st_mode & 022)) {
        snprintf(error, error_size, "external component library '%s' must be an absolute, non-writable regular file",
                 path ? path : "");
        return false;
    }
    if ((require_sha256 && (!module->sha256 || !*module->sha256)) || !sha256_matches(path, module->sha256)) {
        snprintf(error, error_size, "external component library '%s' failed SHA-256 validation", path);
        return false;
    }
    return true;
}
