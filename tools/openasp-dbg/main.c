// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#define _POSIX_C_SOURCE 200809L

#include "dbg.h"
#include "file.h"
#include "module_image.h"
#include "version.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void oasp_dbg_usage(FILE *output, const char *program) {
    fprintf(output,
            "Usage: %s <page.asp> [--root DIR] [--query QUERY] [--body BODY] "
            "[--method METHOD] [--runtime-mode=batch|service] "
            "[--memory-limit=SIZE|-1] [-x COMMAND_FILE]\n",
            program);
    fprintf(output,
            "       %s connect (--unix-socket PATH | --tcp HOST:PORT --token TOKEN) --target PID\n",
            program);
}

static int oasp_dbg_connect_unix(const char *path) {
    int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address;
    if (descriptor < 0 || !path || strlen(path) >= sizeof(address.sun_path))
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (connect(descriptor, (struct sockaddr *)&address, sizeof(address))) {
        close(descriptor);
        return -1;
    }
    return descriptor;
}

static int oasp_dbg_connect_tcp(const char *host_port) {
    const char *colon = host_port ? strrchr(host_port, ':') : NULL;
    if (!colon || colon == host_port || !colon[1])
        return -1;
    char host[INET_ADDRSTRLEN];
    size_t host_length = (size_t)(colon - host_port);
    if (host_length >= sizeof(host))
        return -1;
    memcpy(host, host_port, host_length);
    host[host_length] = '\0';
    char *end = NULL;
    long port = strtol(colon + 1, &end, 10);
    if (!end || *end || port < 1 || port > 65535)
        return -1;
    int descriptor = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    if (descriptor < 0 || inet_pton(AF_INET, host, &address.sin_addr) != 1 ||
        connect(descriptor, (struct sockaddr *)&address, sizeof(address))) {
        if (descriptor >= 0)
            close(descriptor);
        return -1;
    }
    return descriptor;
}

static bool oasp_dbg_write_all(int descriptor, const void *data, size_t length) {
    const char *cursor = data;
    while (length) {
        ssize_t written = send(descriptor, cursor, length, 0);
        if (written <= 0)
            return false;
        cursor += written;
        length -= (size_t)written;
    }
    return true;
}

/* The remote backend transports the existing command/core stream unchanged. */
static int oasp_dbg_connect_remote(int argc, char **argv) {
    const char *unix_path = NULL;
    const char *tcp = NULL;
    const char *token = NULL;
    const char *target = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--unix-socket") && i + 1 < argc)
            unix_path = argv[++i];
        else if (!strcmp(argv[i], "--tcp") && i + 1 < argc)
            tcp = argv[++i];
        else if (!strcmp(argv[i], "--token") && i + 1 < argc)
            token = argv[++i];
        else if (!strcmp(argv[i], "--target") && i + 1 < argc)
            target = argv[++i];
        else {
            oasp_dbg_usage(stderr, argv[0]);
            return 2;
        }
    }
    if ((!unix_path && !tcp) || (unix_path && tcp) || !target || (tcp && !token)) {
        oasp_dbg_usage(stderr, argv[0]);
        return 2;
    }
    int descriptor = unix_path ? oasp_dbg_connect_unix(unix_path) : oasp_dbg_connect_tcp(tcp);
    if (descriptor < 0) {
        perror("openasp-dbg: cannot connect");
        return 1;
    }
    char handshake[512];
    int length = snprintf(handshake, sizeof(handshake), "%s%starget %s\n",
                          tcp ? "auth " : "", tcp ? token : "", target);
    if (length < 0 || (size_t)length >= sizeof(handshake) ||
        !oasp_dbg_write_all(descriptor, handshake, (size_t)length)) {
        close(descriptor);
        return 1;
    }
    for (;;) {
        struct pollfd descriptors[2] = {{.fd = STDIN_FILENO, .events = POLLIN}, {.fd = descriptor, .events = POLLIN}};
        if (poll(descriptors, 2, -1) <= 0)
            break;
        for (size_t i = 0; i < 2; i++) {
            if (!(descriptors[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            char buffer[4096];
            ssize_t received = i ? recv(descriptors[i].fd, buffer, sizeof(buffer), 0)
                                 : read(descriptors[i].fd, buffer, sizeof(buffer));
            int output = i ? STDOUT_FILENO : descriptor;
            if (received <= 0 || (i ? write(output, buffer, (size_t)received)
                                    : send(output, buffer, (size_t)received, 0)) <= 0) {
                close(descriptor);
                return i ? 0 : 1;
            }
        }
    }
    close(descriptor);
    return 0;
}

static bool oasp_dbg_parse_size(const char *text, size_t *result) {
    if (text && !strcmp(text, "-1")) {
        *result = 0;
        return true;
    }
    if (!text || !*text)
        return false;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    size_t multiplier = 1;
    if (*end == 'k' || *end == 'K')
        multiplier = 1024;
    else if (*end == 'm' || *end == 'M')
        multiplier = 1024 * 1024;
    else if (*end == 'g' || *end == 'G')
        multiplier = 1024 * 1024 * 1024;
    else if (*end)
        return false;
    if (*end)
        end++;
    if (*end || value > SIZE_MAX / multiplier)
        return false;
    *result = (size_t)value * multiplier;
    return *result != 0;
}

static bool oasp_dbg_has_oac_suffix(const char *path) {
    size_t length = path ? strlen(path) : 0;
    return length > 4 && !strcmp(path + length - 4, ".oac");
}

static int oasp_dbg_execute_oac(oasp_request *request, const char *path, const char *runtime_mode, oasp_dbg *debugger) {
    unsigned char *data = NULL;
    size_t size = 0;
    char error[512];
    if (!oasp_tool_read_file(path, &data, &size, error, sizeof(error))) {
        fprintf(stderr, "openasp-dbg: %s\n", error);
        return -1;
    }
    oasp_module_image image;
    if (!oasp_module_image_decode(NULL, data, size, &image)) {
        fprintf(stderr, "openasp-dbg: '%s' is not a valid OAC image\n", path);
        free(data);
        return -1;
    }
    if (!image.has_debug_info) {
        fprintf(stderr,
                "openasp-dbg: '%s' has no source map; rebuild it with "
                "--gen-dbg-info=on\n",
                path);
        oasp_module_image_dispose(&image);
        free(data);
        return -1;
    }
    oasp_slice source_path;
    if (!oasp_module_image_path(&image, &source_path)) {
        oasp_module_image_dispose(&image);
        free(data);
        return -1;
    }
    char *source_name = malloc(source_path.length + 1);
    if (!source_name) {
        oasp_module_image_dispose(&image);
        free(data);
        return -1;
    }
    memcpy(source_name, source_path.data, source_path.length);
    source_name[source_path.length] = '\0';
    oasp_runtime runtime;
    bool service = !strcmp(runtime_mode, "service");
    bool initialized = service ? oasp_runtime_init_service(&runtime, request, NULL)
                               : (oasp_runtime_init(&runtime, request), true);
    bool executed = false;
    if (initialized) {
        oasp_request_set_path(request, source_name);
        runtime.debug_source_name = source_name;
        runtime.debug_source = image.debug_source;
        oasp_runtime_set_debugger(&runtime, &debugger->interface);
        executed = oasp_runtime_execute_bytecode(&runtime, &image.bytecode);
        if (service)
            oasp_allocator_collect(runtime.allocator, true);
        oasp_runtime_dispose(&runtime);
    }
    free(source_name);
    oasp_module_image_dispose(&image);
    free(data);
    return executed ? 0 : -1;
}

int main(int argc, char **argv) {
    oasp_print_version(stderr);
    if (argc > 1 && !strcmp(argv[1], "connect"))
        return oasp_dbg_connect_remote(argc, argv);
    const char *page = NULL;
    const char *root = ".";
    const char *query = "";
    const char *body = "";
    const char *method = "GET";
    const char *runtime_mode = "batch";
    const char *command_path = NULL;
    size_t memory_limit = 128u * 1024u * 1024u;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) {
            oasp_dbg_usage(stdout, argv[0]);
            return 0;
        }
        if (!strcmp(argv[i], "--root") && i + 1 < argc)
            root = argv[++i];
        else if (!strcmp(argv[i], "--query") && i + 1 < argc)
            query = argv[++i];
        else if (!strcmp(argv[i], "--body") && i + 1 < argc)
            body = argv[++i];
        else if (!strcmp(argv[i], "--method") && i + 1 < argc)
            method = argv[++i];
        else if (!strcmp(argv[i], "-x") && i + 1 < argc)
            command_path = argv[++i];
        else if (!strncmp(argv[i], "--runtime-mode=", 15))
            runtime_mode = argv[i] + 15;
        else if (!strncmp(argv[i], "--memory-limit=", 15)) {
            if (!oasp_dbg_parse_size(argv[i] + 15, &memory_limit)) {
                oasp_dbg_usage(stderr, argv[0]);
                return 2;
            }
        } else if (!strncmp(argv[i], "--shell-enable=", 15))
            (void)setenv("OPENASP_SHELL_ENABLE", argv[i] + 15, 1);
        else if (!strncmp(argv[i], "--process-enable=", 17))
            (void)setenv("OPENASP_PROCESS_ENABLE", argv[i] + 17, 1);
        else if (argv[i][0] != '-')
            page = argv[i];
        else {
            oasp_dbg_usage(stderr, argv[0]);
            return 2;
        }
    }
    if (!page || (strcmp(runtime_mode, "batch") && strcmp(runtime_mode, "service"))) {
        oasp_dbg_usage(stderr, argv[0]);
        return 2;
    }
    FILE *commands = command_path ? fopen(command_path, "r") : stdin;
    if (!commands) {
        fprintf(stderr, "openasp-dbg: cannot open command file '%s'\n", command_path);
        return 1;
    }
    oasp_request *request = oasp_request_create(root, query, body, method);
    if (!request) {
        fputs("openasp-dbg: out of memory\n", stderr);
        if (commands != stdin)
            fclose(commands);
        return 1;
    }
    oasp_request_limits limits = {.memory_limit = memory_limit};
    if (oasp_request_set_limits(request, &limits)) {
        fprintf(stderr, "openasp-dbg: %s\n", oasp_request_error(request));
        oasp_request_destroy(request);
        if (commands != stdin)
            fclose(commands);
        return 1;
    }
    oasp_dbg debugger;
    oasp_dbg_init(&debugger, commands, stdout);
    oasp_gc_stats stats = {0};
    int result;
    if (oasp_dbg_has_oac_suffix(page))
        result = oasp_dbg_execute_oac(request, page, runtime_mode, &debugger);
    else
        result = !strcmp(runtime_mode, "service")
                     ? oasp_render_file_service_debug(request, page, NULL, &stats,
                                                      &debugger.interface)
                     : oasp_render_file_debug(request, page, &debugger.interface);
    if (!result && !debugger.quit) {
        size_t length = 0;
        const char *output = oasp_request_output(request, &length);
        if (length) {
            if (debugger.attached)
                fputs("[Inferior output]\n", stdout);
            else
                fprintf(stdout, "[Detached process %ld output]\n", (long)getpid());
            fwrite(output, 1, length, stdout);
            if (output[length - 1] != '\n')
                fputc('\n', stdout);
        }
        if (debugger.attached)
            fputs("[Inferior exited normally]\n", stdout);
    } else if (result && !debugger.quit) {
        fprintf(stderr, "openasp-dbg: cannot render %s\n", page);
        if (oasp_request_failed(request))
            fprintf(stderr, "openasp-dbg: %s\n", oasp_request_error(request));
    }
    bool command_error = debugger.command_error;
    bool quit = debugger.quit;
    oasp_dbg_dispose(&debugger);
    oasp_request_destroy(request);
    if (commands != stdin)
        fclose(commands);
    return ((result && !quit) || command_error) ? 1 : 0;
}
