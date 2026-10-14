// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#include <openasp/ext_api.h>

#include <stdlib.h>

enum {
    MEMBER_VERSION = 1,
    MEMBER_SCALE = 2,
    MEMBER_ADD = 3,
    MEMBER_CAPTURE = 4,
    MEMBER_CAPTURED_COUNT = 5
};

typedef struct {
    int64_t scale;
    const oasp_value_handle *captured;
} acme_math;

static const oasp_host_api *host;

static int module_init(const oasp_host_api *api, void **state) {
    host = api;
    *state = NULL;
    return 0;
}

static int create(void *worker_state, void **instance, oasp_ext_error *error) {
    (void)worker_state;
    (void)error;
    acme_math *math = calloc(1, sizeof(*math));
    if (!math)
        return -1;
    math->scale = 1;
    *instance = math;
    return 0;
}

static void destroy(void *instance) {
    acme_math *math = instance;
    if (math && math->captured)
        host->value_release(math->captured);
    free(instance);
}

static int get(void *instance, uint32_t member_id, oasp_value_handle **result, oasp_ext_error *error) {
    acme_math *math = instance;
    (void)error;
    if (member_id == MEMBER_VERSION)
        *result = host->value_new_string("1.0.0", 5);
    else if (member_id == MEMBER_SCALE)
        *result = host->value_new_i64(math->scale);
    else if (member_id == MEMBER_CAPTURED_COUNT && math->captured) {
        oasp_ext_bytes name = {"Count", 5};
        if (host->value_object_get(math->captured, name.data, name.length, result, error))
            return -1;
    }
    else
        return -1;
    (void)math;
    return 0;
}

static int set(void *instance, uint32_t member_id, const oasp_value_handle *value, oasp_ext_error *error) {
    acme_math *math = instance;
    if (member_id != MEMBER_SCALE || host->value_get_i64(value, &math->scale)) {
        error->code = 5;
        error->message = "Scale requires an integer";
        error->source = "Acme.Math";
        return -1;
    }
    return 0;
}

static int call(void *instance, uint32_t member_id, const oasp_value_handle *const *args, size_t argc, oasp_value_handle **result,
                oasp_ext_error *error) {
    acme_math *math = instance;
    int64_t left, right;
    if (member_id == MEMBER_ADD && argc == 2 &&
        !host->value_get_i64(args[0], &left) && !host->value_get_i64(args[1], &right)) {
        *result = host->value_new_i64((left + right) * math->scale);
        return *result ? 0 : -1;
    }
    if (member_id == MEMBER_CAPTURE && argc == 1) {
        if (math->captured)
            host->value_release(math->captured);
        if (host->value_retain(args[0])) {
            math->captured = NULL;
            return -1;
        }
        math->captured = args[0];
        *result = host->value_new_bool(1);
        return *result ? 0 : -1;
    }
    error->code = 5;
    error->message = "Add requires two integers";
    error->source = "Acme.Math";
    return -1;
}

static void trace(void *instance, oasp_trace_visitor visit, void *context) {
    acme_math *math = instance;
    if (math && math->captured)
        visit(context, math->captured);
}

static const oasp_extension_member members[] = {
    {sizeof(oasp_extension_member), "Version", MEMBER_VERSION},
    {sizeof(oasp_extension_member), "Scale", MEMBER_SCALE},
    {sizeof(oasp_extension_member), "Add", MEMBER_ADD},
    {sizeof(oasp_extension_member), "Capture", MEMBER_CAPTURE},
    {sizeof(oasp_extension_member), "CapturedCount", MEMBER_CAPTURED_COUNT},
};

static const oasp_extension_class classes[] = {{
    sizeof(oasp_extension_class), "Acme.Math", "Acme.Math", members, sizeof(members) / sizeof(members[0]),
    create, destroy, get, set, call, trace
}};

static const oasp_extension_descriptor descriptor = {
    sizeof(oasp_extension_descriptor), OASP_EXTENSION_ABI_MAJOR, OASP_EXTENSION_ABI_MINOR,
    "acme-math", "1.0.0", 0, module_init, NULL, NULL, NULL, classes, sizeof(classes) / sizeof(classes[0])
};

const oasp_extension_descriptor *openasp_extension(void) {
    return &descriptor;
}
