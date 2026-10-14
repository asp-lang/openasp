// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#define _POSIX_C_SOURCE 200809L

#include "ext_internal.h"
#include "ext_manifest.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool oasp_external_registry_create(oasp_external_registry **result, const oasp_external_module_config *configs, size_t count, bool require_sha256, char *error,
                                   size_t error_size) {
    if (!result)
        return false;
    *result = NULL;
    oasp_external_registry *registry = calloc(1, sizeof(*registry));
    if (!registry)
        return false;
    registry->modules = calloc(count, sizeof(*registry->modules));
    if (count && !registry->modules) goto oom;
    registry->module_count = count;
    for (size_t i = 0; i < count; i++) {
        if (configs[i].mode == OASP_EXTERNAL_MODE_ISOLATED) {
            (void)oasp_external_isolated_proxy_load(&configs[i], error, error_size);
            goto failed;
        }
        if (!oasp_external_manifest_validate_library(&configs[i], require_sha256, error, error_size))
            goto failed;
        oasp_external_module *module = &registry->modules[i];
        module->config = &configs[i];
        module->library = dlopen(configs[i].library, RTLD_NOW | RTLD_LOCAL);
        oasp_extension_entry entry = NULL;
        if (module->library) {
            void *symbol = dlsym(module->library, "openasp_extension");
            _Static_assert(sizeof(entry) == sizeof(symbol), "dlsym function pointer size mismatch");
            memcpy(&entry, &symbol, sizeof(entry));
        }
        module->descriptor = entry ? entry() : NULL;
        if (!module->descriptor || module->descriptor->struct_size < sizeof(uint32_t) * 3 ||
            module->descriptor->abi_major_required != OASP_EXTENSION_ABI_MAJOR ||
            module->descriptor->abi_minor_required > OASP_EXTENSION_ABI_MINOR) {
            snprintf(error, error_size, "external component '%s' has an incompatible OpenASP extension ABI", configs[i].id);
            goto failed;
        }
        if (module->descriptor->module_init && module->descriptor->module_init(oasp_external_host_api(), &module->module_state)) {
            snprintf(error, error_size, "external component '%s' module_init failed", configs[i].id);
            goto failed;
        }
        for (size_t class_index = 0; class_index < module->descriptor->class_count; class_index++) {
            const oasp_extension_class *class_definition = &module->descriptor->classes[class_index];
            bool allowed = false;
            for (size_t allowed_index = 0; allowed_index < configs[i].prog_id_count; allowed_index++)
                if (oasp_external_ascii_equal((oasp_slice){class_definition->prog_id, strlen(class_definition->prog_id)},
                                               configs[i].prog_ids[allowed_index]))
                    allowed = true;
            if (!allowed) {
                snprintf(error, error_size, "external component '%s' exposes non-whitelisted ProgID", configs[i].id);
                goto failed;
            }
            oasp_external_class *classes = realloc(registry->classes, (registry->class_count + 1) * sizeof(*classes));
            if (!classes) goto oom;
            registry->classes = classes;
            registry->classes[registry->class_count++] = (oasp_external_class){module, class_definition};
        }
        for (size_t allowed_index = 0; allowed_index < configs[i].prog_id_count; allowed_index++) {
            bool found = false;
            for (size_t class_index = 0; class_index < module->descriptor->class_count; class_index++)
                if (oasp_external_ascii_equal((oasp_slice){module->descriptor->classes[class_index].prog_id,
                                                            strlen(module->descriptor->classes[class_index].prog_id)},
                                               configs[i].prog_ids[allowed_index]))
                    found = true;
            if (!found) {
                snprintf(error, error_size, "external component '%s' whitelist contains unknown ProgID '%s'",
                         configs[i].id, configs[i].prog_ids[allowed_index]);
                goto failed;
            }
        }
    }
    *result = registry;
    return true;
oom:
    snprintf(error, error_size, "out of memory while loading external components");
failed:
    oasp_external_registry_destroy(registry);
    return false;
}

void oasp_external_registry_destroy(oasp_external_registry *registry) {
    if (!registry) return;
    oasp_external_registry_worker_shutdown(registry);
    for (size_t i = 0; i < registry->module_count; i++) {
        oasp_external_module *module = &registry->modules[i];
        if (module->descriptor && module->descriptor->module_shutdown)
            module->descriptor->module_shutdown(module->module_state);
        /*
         * Extensions are intentionally process-lifetime mappings. Request
         * finalizers may still reference class destroy functions while an
         * engine is being released, so dlclose here would create a UAF.
         */
    }
    free(registry->classes);
    free(registry->modules);
    free(registry);
}

bool oasp_external_registry_worker_init(oasp_external_registry *registry, char *error, size_t error_size) {
    if (!registry) return true;
    for (size_t i = 0; i < registry->module_count; i++) {
        oasp_external_module *module = &registry->modules[i];
        if (module->descriptor->worker_init && module->descriptor->worker_init(module->module_state, &module->worker_state)) {
            snprintf(error, error_size, "external component '%s' worker_init failed", module->config->id);
            oasp_external_registry_worker_shutdown(registry);
            return false;
        }
    }
    return true;
}

void oasp_external_registry_worker_shutdown(oasp_external_registry *registry) {
    if (!registry) return;
    for (size_t i = registry->module_count; i > 0; i--) {
        oasp_external_module *module = &registry->modules[i - 1];
        if (module->worker_state && module->descriptor->worker_shutdown)
            module->descriptor->worker_shutdown(module->worker_state);
        module->worker_state = NULL;
    }
}
