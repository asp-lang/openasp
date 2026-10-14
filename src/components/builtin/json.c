// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include "builtin_internal.h"

#include <ctype.h>
#include <limits.h>
#include <json-c/json.h>
#include "json_compat.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <string.h>

static oasp_value json_slice_value(oasp_runtime *runtime, const char *data, size_t length) {
    char *copy = oasp_runtime_copy(runtime, data, length);
    return copy ? oasp_value_string(copy, length) : oasp_value_empty();
}

static const unsigned char *json_value_bytes(oasp_runtime *runtime, const oasp_value *value, size_t *length) {
    if (value->kind == OASP_VALUE_STRING) {
        *length = value->as.string.length;
        return (const unsigned char *)value->as.string.data;
    }
    char *text = oasp_value_format(runtime->arena, value);
    *length = text ? strlen(text) : 0;
    return (const unsigned char *)text;
}

static oasp_value oasp_json_import(oasp_runtime *runtime, struct json_object *json, size_t depth) {
    if (!json || depth > 128)
        return oasp_value_null();
    enum json_type type = json_object_get_type(json);
    if (type == json_type_null)
        return oasp_value_null();
    if (type == json_type_boolean)
        return oasp_value_bool(json_object_get_boolean(json));
    if (type == json_type_double)
        return oasp_value_float(json_object_get_double(json));
    if (type == json_type_int)
        return oasp_value_int(json_object_get_int64(json));
    if (type == json_type_string)
        return json_slice_value(runtime, json_object_get_string(json), (size_t)json_object_get_string_len(json));
    if (type == json_type_array) {
        size_t length = json_object_array_length(json);
        oasp_array *array = oasp_runtime_alloc_array(runtime);
        if (!array)
            return oasp_value_empty();
        memset(array, 0, sizeof(*array));
        array->rank = 1;
        array->length = length;
        array->bounds = oasp_runtime_alloc(runtime, sizeof(*array->bounds), _Alignof(int64_t));
        if (!array->bounds)
            return oasp_value_empty();
        array->bounds[0] = length ? (int64_t)length - 1 : -1;
        if (length) {
            array->items = oasp_runtime_alloc_values(runtime, length);
            if (!array->items)
                return oasp_value_empty();
            for (size_t i = 0; i < length; i++)
                array->items[i] = oasp_json_import(runtime, json_object_array_get_idx(json, i), depth + 1);
        }
        oasp_value value = oasp_value_empty();
        value.kind = OASP_VALUE_ARRAY;
        value.as.array = array;
        return value;
    }
    if (type == json_type_object) {
        oasp_dictionary *dictionary = oasp_builtin_dictionary_create(runtime, 0);
        if (!dictionary)
            return oasp_value_empty();
        oasp_object *object = oasp_component_object(runtime, OASP_COMPONENT_DICTIONARY, "OpenASP.JSON.Object", dictionary);
        if (!object)
            return oasp_value_empty();
        json_object_object_foreach(json, key, child) {
            oasp_value key_value = json_slice_value(runtime, key, strlen(key));
            oasp_value child_value = oasp_json_import(runtime, child, depth + 1);
            if (!oasp_builtin_dictionary_assign(runtime, object, &key_value, &child_value, 0))
                return oasp_value_empty();
        }
        return oasp_component_object_value(object);
    }
    return oasp_value_null();
}

static struct json_object *oasp_json_export(oasp_runtime *runtime, const oasp_value *value, size_t depth) {
    if (!value || depth > 128 || value->kind == OASP_VALUE_EMPTY || value->kind == OASP_VALUE_NULL || value->kind == OASP_VALUE_NOTHING)
        return json_object_new_null();
    if (value->kind == OASP_VALUE_BOOL)
        return json_object_new_boolean(value->as.boolean);
    if (value->kind == OASP_VALUE_INT)
        return json_object_new_int64(value->as.integer);
    if (value->kind == OASP_VALUE_FLOAT)
        return json_object_new_double(value->as.floating);
    if (value->kind == OASP_VALUE_STRING)
        return json_object_new_string_len(value->as.string.data, (int)value->as.string.length);
    if (value->kind == OASP_VALUE_DATE)
        return json_object_new_string_len(value->as.date.data, (int)value->as.date.length);
    if (value->kind == OASP_VALUE_ARRAY) {
        struct json_object *array = json_object_new_array();
        if (!array)
            return NULL;
        for (size_t i = 0; i < value->as.array->length; i++) {
            struct json_object *item = oasp_json_export(runtime, &value->as.array->items[i], depth + 1);
            if (json_object_array_add(array, item)) {
                if (item)
                    oasp_json_object_put(item);
                oasp_json_object_put(array);
                return NULL;
            }
        }
        return array;
    }
    if (value->kind == OASP_VALUE_OBJECT && value->as.object && value->as.object->component_kind == OASP_COMPONENT_DICTIONARY) {
        struct json_object *result = json_object_new_object();
        oasp_dictionary *dictionary = value->as.object->native;
        if (!result)
            return NULL;
        for (oasp_dictionary_entry *entry = dictionary ? dictionary->first : NULL; entry; entry = entry->next) {
            char *key = oasp_value_format(runtime->format_arena, &entry->key);
            struct json_object *child = oasp_json_export(runtime, &entry->value, depth + 1);
            if (!key || json_object_object_add(result, key, child)) {
                if (child)
                    oasp_json_object_put(child);
                oasp_json_object_put(result);
                return NULL;
            }
        }
        return result;
    }
    return json_object_new_null();
}

bool oasp_builtin_json_parse(oasp_runtime *runtime, const oasp_value *input, oasp_value *result) {
    size_t length = 0;
    const unsigned char *bytes = json_value_bytes(runtime, input, &length);
    if (!bytes || length > INT_MAX)
        return false;
    struct json_tokener *tokener = json_tokener_new_ex(128);
    if (!tokener)
        return false;
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
    struct json_object *json = json_tokener_parse_ex(tokener, (const char *)bytes, (int)length);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && isspace((unsigned char)bytes[consumed]))
        consumed++;
    bool ok = json_tokener_get_error(tokener) == json_tokener_success && json && consumed == length;
    if (ok && result)
        *result = oasp_json_import(runtime, json, 0);
    if (json)
        oasp_json_object_put(json);
    json_tokener_free(tokener);
    return ok;
}

bool oasp_builtin_json_call(oasp_runtime *runtime, oasp_slice member, oasp_value *arguments, size_t count, size_t line,
                            oasp_value *result) {
    if (oasp_ascii_equal(member, "validate") && count) {
        *result = oasp_value_bool(oasp_builtin_json_parse(runtime, &arguments[0], NULL));
        return true;
    }
    if (oasp_ascii_equal(member, "parse") && count) {
        if (!oasp_builtin_json_parse(runtime, &arguments[0], result))
            oasp_runtime_raise(runtime, 5, "invalid JSON input", "OpenASP.JSON", line);
        return true;
    }
    if ((oasp_ascii_equal(member, "minify") || oasp_ascii_equal(member, "stringify")) && count) {
        struct json_object *json = NULL;
        if (oasp_ascii_equal(member, "stringify"))
            json = oasp_json_export(runtime, &arguments[0], 0);
        else {
            oasp_value parsed;
            if (oasp_builtin_json_parse(runtime, &arguments[0], &parsed))
                json = oasp_json_export(runtime, &parsed, 0);
        }
        if (!json) {
            oasp_runtime_raise(runtime, 5, "invalid JSON input", "OpenASP.JSON", line);
            return true;
        }
        const char *text = json_object_to_json_string_ext(json, JSON_C_TO_STRING_PLAIN);
        *result = json_slice_value(runtime, text, strlen(text));
        oasp_json_object_put(json);
        return true;
    }
    if (oasp_ascii_equal(member, "escape") && count) {
        size_t length = 0;
        const unsigned char *bytes = json_value_bytes(runtime, &arguments[0], &length);
        struct json_object *json = json_object_new_string_len((const char *)bytes, (int)length);
        const char *quoted = json ? json_object_to_json_string_ext(json, JSON_C_TO_STRING_PLAIN) : NULL;
        size_t quoted_length = quoted ? strlen(quoted) : 0;
        *result = quoted_length >= 2 ? json_slice_value(runtime, quoted + 1, quoted_length - 2) : oasp_value_empty();
        if (json)
            oasp_json_object_put(json);
        return true;
    }
    return false;
}

static oasp_value oasp_crypto_hex(oasp_runtime *runtime, const unsigned char *bytes, size_t length) {
    static const char digits[] = "0123456789abcdef";
    char *text = oasp_runtime_alloc(runtime, length * 2 + 1, _Alignof(char));
    if (!text)
        return oasp_value_empty();
    for (size_t i = 0; i < length; i++) {
        text[i * 2] = digits[bytes[i] >> 4];
        text[i * 2 + 1] = digits[bytes[i] & 15];
    }
    text[length * 2] = '\0';
    return oasp_value_string(text, length * 2);
}

bool oasp_builtin_crypto_call(oasp_runtime *runtime, oasp_slice member, oasp_value *arguments, size_t count, size_t line,
                              oasp_value *result) {
    if (oasp_ascii_equal(member, "sha256") && count) {
        size_t length;
        const unsigned char *input = json_value_bytes(runtime, &arguments[0], &length);
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digest_length = 0;
        EVP_MD_CTX *context = EVP_MD_CTX_new();
        bool ok = context && EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1 && EVP_DigestUpdate(context, input, length) == 1 &&
                  EVP_DigestFinal_ex(context, digest, &digest_length) == 1;
        EVP_MD_CTX_free(context);
        if (ok)
            *result = oasp_crypto_hex(runtime, digest, digest_length);
        return true;
    }
    if (oasp_ascii_equal(member, "hmacsha256") && count > 1) {
        size_t key_length, input_length;
        const unsigned char *key = json_value_bytes(runtime, &arguments[0], &key_length);
        const unsigned char *input = json_value_bytes(runtime, &arguments[1], &input_length);
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digest_length = 0;
        if (key_length <= INT_MAX && HMAC(EVP_sha256(), key, (int)key_length, input, input_length, digest, &digest_length))
            *result = oasp_crypto_hex(runtime, digest, digest_length);
        return true;
    }
    bool encode = oasp_ascii_equal(member, "base64encode") || oasp_ascii_equal(member, "base64urlencode");
    bool decode = oasp_ascii_equal(member, "base64decode") || oasp_ascii_equal(member, "base64urldecode");
    if ((encode || decode) && count) {
        size_t length;
        const unsigned char *input = json_value_bytes(runtime, &arguments[0], &length);
        if (length > INT_MAX)
            return true;
        if (encode) {
            size_t capacity = 4 * ((length + 2) / 3);
            unsigned char *output = oasp_runtime_alloc(runtime, capacity + 1, _Alignof(char));
            int written = output ? EVP_EncodeBlock(output, input, (int)length) : -1;
            if (written >= 0 && oasp_ascii_equal(member, "base64urlencode")) {
                for (int i = 0; i < written; i++) {
                    if (output[i] == '+')
                        output[i] = '-';
                    else if (output[i] == '/')
                        output[i] = '_';
                }
                while (written && output[written - 1] == '=')
                    written--;
            }
            if (written >= 0)
                *result = oasp_value_string((const char *)output, (size_t)written);
            return true;
        }
        size_t padded = (length + 3) & ~(size_t)3;
        unsigned char *encoded = oasp_runtime_alloc(runtime, padded + 1, _Alignof(char));
        unsigned char *output = oasp_runtime_alloc(runtime, padded / 4 * 3 + 1, _Alignof(char));
        if (!encoded || !output)
            return true;
        memcpy(encoded, input, length);
        for (size_t i = 0; i < length; i++) {
            if (encoded[i] == '-')
                encoded[i] = '+';
            else if (encoded[i] == '_')
                encoded[i] = '/';
        }
        for (size_t i = length; i < padded; i++)
            encoded[i] = '=';
        encoded[padded] = '\0';
        int written = EVP_DecodeBlock(output, encoded, (int)padded);
        while (written > 0 && padded && encoded[padded - 1] == '=') {
            written--;
            padded--;
        }
        if (written >= 0)
            *result = oasp_value_string((const char *)output, (size_t)written);
        return true;
    }
    if (oasp_ascii_equal(member, "randombytes") || oasp_ascii_equal(member, "randomhex")) {
        int64_t requested = 32;
        if ((count && !oasp_value_to_int(&arguments[0], &requested)) || requested < 1 || requested > 1048576) {
            oasp_runtime_raise(runtime, 5, "invalid random byte count", "OpenASP.Crypto", line);
            return true;
        }
        unsigned char *bytes = oasp_runtime_alloc(runtime, (size_t)requested, _Alignof(char));
        if (bytes && RAND_bytes(bytes, (int)requested) == 1)
            *result = oasp_ascii_equal(member, "randomhex") ? oasp_crypto_hex(runtime, bytes, (size_t)requested)
                                                            : oasp_value_string((const char *)bytes, (size_t)requested);
        return true;
    }
    if (oasp_ascii_equal(member, "constanttimeequals") && count > 1) {
        size_t left_length, right_length;
        const unsigned char *left = json_value_bytes(runtime, &arguments[0], &left_length);
        const unsigned char *right = json_value_bytes(runtime, &arguments[1], &right_length);
        *result = oasp_value_bool(left_length == right_length && CRYPTO_memcmp(left, right, left_length) == 0);
        return true;
    }
    return false;
}
