// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#define _POSIX_C_SOURCE 200809L

#include "ext_internal.h"
#include "ext_manifest.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool valid_name(const char *name, size_t maximum) {
    size_t length = name ? strnlen(name, maximum + 1) : 0;
    return length && length <= maximum;
}

bool oasp_external_descriptor_validate(const oasp_external_module_config *config, const oasp_extension_descriptor *descriptor, char *error, size_t error_size) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor) ||
        descriptor->abi_major_required != OASP_EXTENSION_ABI_MAJOR ||
        descriptor->abi_minor_required > OASP_EXTENSION_ABI_MINOR) {
        snprintf(error, error_size, "external component '%s' requires Extension ABI v2", config->id);
        return false;
    }
    if (!valid_name(descriptor->module_name, 127) || !valid_name(descriptor->module_version, 63) ||
        !descriptor->class_count || descriptor->class_count > 4096 || !descriptor->classes) {
        snprintf(error, error_size, "external component '%s' has an invalid descriptor", config->id);
        return false;
    }
    if ((descriptor->required_capabilities & config->capabilities) != descriptor->required_capabilities) {
        snprintf(error, error_size, "external component '%s' requires capabilities not granted by its manifest", config->id);
        return false;
    }
    for (size_t i = 0; i < descriptor->class_count; i++) {
        const oasp_extension_class *class_definition = &descriptor->classes[i];
        if (class_definition->struct_size < sizeof(*class_definition) ||
            !valid_name(class_definition->prog_id, 255) || !valid_name(class_definition->canonical_name, 255) ||
            !class_definition->class_id ||
            (!(class_definition->flags & OASP_EXT_CLASS_INTERNAL) && !class_definition->create) ||
            !class_definition->destroy ||
            !class_definition->invoke || class_definition->member_count > 65535 ||
            (class_definition->member_count && !class_definition->members) ||
            ((class_definition->flags & OASP_EXT_CLASS_PERSISTABLE) &&
             (!class_definition->create || !class_definition->snapshot || !class_definition->restore))) {
            snprintf(error, error_size, "external component '%s' class %zu is invalid", config->id, i);
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            const oasp_extension_class *previous = &descriptor->classes[j];
            if (class_definition->class_id == previous->class_id ||
                oasp_external_ascii_equal((oasp_slice){class_definition->prog_id, strlen(class_definition->prog_id)},
                                          previous->prog_id)) {
                snprintf(error, error_size, "external component '%s' has duplicate class identifiers", config->id);
                return false;
            }
        }
        for (size_t j = 0; j < class_definition->member_count; j++) {
            const oasp_extension_member *member = &class_definition->members[j];
            if (member->struct_size < sizeof(*member) || !valid_name(member->name, 255) || !member->member_id ||
                !(member->flags & (OASP_EXT_MEMBER_GET | OASP_EXT_MEMBER_SET | OASP_EXT_MEMBER_CALL))) {
                snprintf(error, error_size, "external component '%s' class '%s' has an invalid member",
                         config->id, class_definition->prog_id);
                return false;
            }
            for (size_t k = 0; k < j; k++) {
                const oasp_extension_member *previous = &class_definition->members[k];
                if (member->member_id == previous->member_id ||
                    oasp_external_ascii_equal((oasp_slice){member->name, strlen(member->name)}, previous->name)) {
                    snprintf(error, error_size, "external component '%s' class '%s' has duplicate members",
                             config->id, class_definition->prog_id);
                    return false;
                }
            }
        }
    }
    return true;
}

static size_t table_capacity(size_t count) {
    size_t capacity = 8;
    while (capacity < count * 2 && capacity <= SIZE_MAX / 2)
        capacity *= 2;
    return capacity;
}

bool oasp_external_class_prepare(oasp_external_class *external_class, char *error, size_t error_size) {
    size_t count = external_class->class_definition->member_count;
    external_class->member_slot_count = table_capacity(count);
    external_class->member_slots = calloc(external_class->member_slot_count, sizeof(*external_class->member_slots));
    if (!external_class->member_slots) {
        snprintf(error, error_size, "out of memory while indexing external component members");
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        const oasp_extension_member *member = &external_class->class_definition->members[i];
        uint64_t hash = oasp_external_ascii_hash(member->name, strlen(member->name));
        size_t slot = (size_t)hash & (external_class->member_slot_count - 1);
        while (external_class->member_slots[slot].member)
            slot = (slot + 1) & (external_class->member_slot_count - 1);
        external_class->member_slots[slot] = (oasp_external_member_slot){hash, member};
    }
    return true;
}

void oasp_external_class_dispose(oasp_external_class *external_class) {
    free(external_class->member_slots);
    external_class->member_slots = NULL;
    external_class->member_slot_count = 0;
}

const oasp_extension_member *oasp_external_member_find(const oasp_external_class *external_class, oasp_slice member) {
    if (!external_class || !external_class->member_slot_count)
        return NULL;
    uint64_t hash = oasp_external_ascii_hash(member.data, member.length);
    size_t slot = (size_t)hash & (external_class->member_slot_count - 1);
    for (size_t checked = 0; checked < external_class->member_slot_count; checked++) {
        const oasp_external_member_slot *entry = &external_class->member_slots[slot];
        if (!entry->member)
            return NULL;
        if (entry->hash == hash && oasp_external_ascii_equal(member, entry->member->name))
            return entry->member;
        slot = (slot + 1) & (external_class->member_slot_count - 1);
    }
    return NULL;
}

oasp_external_class *oasp_external_class_find(const oasp_external_registry *registry, oasp_slice prog_id) {
    if (!registry || !registry->class_slot_count)
        return NULL;
    uint64_t hash = oasp_external_ascii_hash(prog_id.data, prog_id.length);
    size_t slot = (size_t)hash & (registry->class_slot_count - 1);
    for (size_t checked = 0; checked < registry->class_slot_count; checked++) {
        const oasp_external_class_slot *entry = &registry->class_slots[slot];
        if (!entry->class_definition)
            return NULL;
        if (entry->hash == hash &&
            oasp_external_ascii_equal(prog_id, entry->class_definition->class_definition->prog_id))
            return entry->class_definition;
        slot = (slot + 1) & (registry->class_slot_count - 1);
    }
    return NULL;
}

oasp_external_class *oasp_external_class_find_id(const oasp_external_registry *registry, oasp_external_module *module, uint32_t class_id) {
    if (!registry || !module || !class_id)
        return NULL;
    for (size_t i = 0; i < registry->class_count; i++)
        if (registry->classes[i].module == module &&
            registry->classes[i].class_definition->class_id == class_id)
            return &registry->classes[i];
    return NULL;
}

const oasp_extension_class *oasp_external_module_class_find(const oasp_external_module *module, uint32_t class_id) {
    if (!module || !module->descriptor || !class_id)
        return NULL;
    for (size_t i = 0; i < module->descriptor->class_count; i++)
        if (module->descriptor->classes[i].class_id == class_id)
            return &module->descriptor->classes[i];
    return NULL;
}

static bool class_allowed(const oasp_external_module_config *config, const char *prog_id) {
    for (size_t i = 0; i < config->prog_id_count; i++)
        if (oasp_external_ascii_equal((oasp_slice){prog_id, strlen(prog_id)}, config->prog_ids[i]))
            return true;
    return false;
}

static bool registry_index_classes(oasp_external_registry *registry, char *error, size_t error_size) {
    registry->class_slot_count = table_capacity(registry->class_count);
    registry->class_slots = calloc(registry->class_slot_count, sizeof(*registry->class_slots));
    if (!registry->class_slots)
        return false;
    for (size_t i = 0; i < registry->class_count; i++) {
        oasp_external_class *external_class = &registry->classes[i];
        if (external_class->class_definition->flags & OASP_EXT_CLASS_INTERNAL)
            continue;
        const char *prog_id = external_class->class_definition->prog_id;
        uint64_t hash = oasp_external_ascii_hash(prog_id, strlen(prog_id));
        size_t slot = (size_t)hash & (registry->class_slot_count - 1);
        while (registry->class_slots[slot].class_definition) {
            if (registry->class_slots[slot].hash == hash &&
                oasp_external_ascii_equal((oasp_slice){prog_id, strlen(prog_id)},
                                          registry->class_slots[slot].class_definition->class_definition->prog_id)) {
                snprintf(error, error_size, "duplicate external ProgID '%s'", prog_id);
                return false;
            }
            slot = (slot + 1) & (registry->class_slot_count - 1);
        }
        registry->class_slots[slot] = (oasp_external_class_slot){hash, external_class};
    }
    return true;
}

bool oasp_external_registry_create(oasp_external_registry **result, const oasp_external_module_config *configs, size_t count, bool require_sha256, char *error,
                                   size_t error_size) {
    if (!result)
        return false;
    *result = NULL;
    oasp_external_registry *registry = calloc(1, sizeof(*registry));
    if (!registry)
        return false;
    registry->modules = calloc(count, sizeof(*registry->modules));
    if (count && !registry->modules)
        goto oom;
    registry->module_count = count;
    for (size_t i = 0; i < count; i++) {
        if (!oasp_external_manifest_validate_library(&configs[i], require_sha256, error, error_size))
            goto failed;
        oasp_external_module *module = &registry->modules[i];
        module->config = &configs[i];
        oasp_ext_error extension_error = {.struct_size = sizeof(extension_error)};
        if (configs[i].mode == OASP_EXTERNAL_MODE_ISOLATED) {
            if (!oasp_external_isolated_proxy_load(module, error, error_size))
                goto failed;
        } else {
            module->library = dlopen(configs[i].library, RTLD_NOW | RTLD_LOCAL);
            oasp_extension_entry entry = NULL;
            if (module->library) {
                void *symbol = dlsym(module->library, "openasp_extension");
                _Static_assert(sizeof(entry) == sizeof(symbol), "dlsym function pointer size mismatch");
                memcpy(&entry, &symbol, sizeof(entry));
            }
            module->descriptor = entry ? entry() : NULL;
        }
        if (!oasp_external_descriptor_validate(&configs[i], module->descriptor, error, error_size))
            goto failed;
        if (!module->isolated && module->descriptor->module_init &&
            module->descriptor->module_init(oasp_external_host_api(), configs[i].capabilities,
                                            &module->module_state, &extension_error) != OASP_EXT_OK) {
            snprintf(error, error_size, "external component '%s' module_init failed: %s", configs[i].id,
                     extension_error.message ? extension_error.message : "unspecified error");
            goto failed;
        }
        for (size_t class_index = 0; class_index < module->descriptor->class_count; class_index++) {
            const oasp_extension_class *class_definition = &module->descriptor->classes[class_index];
            if (!(class_definition->flags & OASP_EXT_CLASS_INTERNAL) &&
                !class_allowed(&configs[i], class_definition->prog_id)) {
                snprintf(error, error_size, "external component '%s' exposes non-whitelisted ProgID '%s'",
                         configs[i].id, class_definition->prog_id);
                goto failed;
            }
            oasp_external_class *classes = realloc(registry->classes, (registry->class_count + 1) * sizeof(*classes));
            if (!classes)
                goto oom;
            registry->classes = classes;
            oasp_external_class *external_class = &registry->classes[registry->class_count++];
            *external_class = (oasp_external_class){.module = module, .class_definition = class_definition};
            if (!oasp_external_class_prepare(external_class, error, error_size))
                goto failed;
        }
        for (size_t allowed_index = 0; allowed_index < configs[i].prog_id_count; allowed_index++) {
            bool found = false;
            for (size_t class_index = 0; class_index < module->descriptor->class_count; class_index++)
                found |= !(module->descriptor->classes[class_index].flags & OASP_EXT_CLASS_INTERNAL) &&
                         oasp_external_ascii_equal(
                    (oasp_slice){module->descriptor->classes[class_index].prog_id,
                                 strlen(module->descriptor->classes[class_index].prog_id)},
                    configs[i].prog_ids[allowed_index]);
            if (!found) {
                snprintf(error, error_size, "external component '%s' whitelist contains unknown ProgID '%s'",
                         configs[i].id, configs[i].prog_ids[allowed_index]);
                goto failed;
            }
        }
    }
    if (!registry_index_classes(registry, error, error_size))
        goto oom;
    *result = registry;
    return true;

oom:
    snprintf(error, error_size, "out of memory while loading external components");
failed:
    oasp_external_registry_destroy(registry);
    return false;
}

void oasp_external_registry_destroy(oasp_external_registry *registry) {
    if (!registry)
        return;
    oasp_external_registry_worker_shutdown(registry);
    for (size_t i = 0; i < registry->module_count; i++) {
        oasp_external_module *module = &registry->modules[i];
        if (module->isolated)
            oasp_external_isolated_proxy_shutdown(module);
        else if (module->descriptor && module->descriptor->module_shutdown)
            module->descriptor->module_shutdown(module->module_state);
    }
    for (size_t i = 0; i < registry->class_count; i++)
        oasp_external_class_dispose(&registry->classes[i]);
    free(registry->class_slots);
    free(registry->classes);
    free(registry->modules);
    free(registry);
}

bool oasp_external_registry_worker_init(oasp_external_registry *registry, char *error, size_t error_size) {
    if (!registry)
        return true;
    for (size_t i = 0; i < registry->module_count; i++) {
        oasp_external_module *module = &registry->modules[i];
        if (module->isolated) {
            module->worker_started = true;
            continue;
        }
        oasp_ext_error extension_error = {.struct_size = sizeof(extension_error)};
        if (module->descriptor->worker_init &&
            module->descriptor->worker_init(module->module_state, &module->worker_state, &extension_error) != OASP_EXT_OK) {
            snprintf(error, error_size, "external component '%s' worker_init failed: %s", module->config->id,
                     extension_error.message ? extension_error.message : "unspecified error");
            oasp_external_registry_worker_shutdown(registry);
            return false;
        }
        module->worker_started = true;
    }
    return true;
}

void oasp_external_registry_worker_shutdown(oasp_external_registry *registry) {
    if (!registry)
        return;
    for (size_t i = registry->module_count; i > 0; i--) {
        oasp_external_module *module = &registry->modules[i - 1];
        if (!module->isolated && module->worker_started && module->descriptor && module->descriptor->worker_shutdown)
            module->descriptor->worker_shutdown(module->worker_state);
        module->worker_state = NULL;
        module->worker_started = false;
    }
}
