// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "builtin_internal.h"
#include "debugger.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

bool oasp_builtin_feature_enabled(const char *name) {
    const char *value = getenv(name);
    return value && (!strcasecmp(value, "on") || !strcasecmp(value, "true") || !strcmp(value, "1"));
}

static void oasp_process_argv_dispose(char **argv) {
    if (!argv)
        return;
    for (size_t i = 0; argv[i]; i++)
        free(argv[i]);
    free(argv);
}

static char **oasp_process_argv(const char *executable, const char *arguments) {
    size_t capacity = 8;
    size_t count = 0;
    char **argv = calloc(capacity, sizeof(*argv));
    if (!argv)
        return NULL;
    argv[count++] = strdup(executable);
    if (!argv[0])
        goto failed;
    const char *cursor = arguments ? arguments : "";
    while (*cursor) {
        while (isspace((unsigned char)*cursor))
            cursor++;
        if (!*cursor)
            break;
        oasp_buffer token = {0};
        if (!oasp_buffer_init(&token, 64))
            goto failed;
        char quote = '\0';
        while (*cursor && (quote || !isspace((unsigned char)*cursor))) {
            char character = *cursor++;
            if (!quote && (character == '\'' || character == '"')) {
                quote = character;
                continue;
            }
            if (quote && character == quote) {
                quote = '\0';
                continue;
            }
            if (character == '\\' && *cursor && (quote != '\'' || *cursor == '\'' || *cursor == '\\'))
                character = *cursor++;
            if (!oasp_buffer_append_byte(&token, character)) {
                oasp_buffer_dispose(&token);
                goto failed;
            }
        }
        if (count + 1 >= capacity) {
            capacity *= 2;
            char **grown = realloc(argv, capacity * sizeof(*argv));
            if (!grown) {
                oasp_buffer_dispose(&token);
                goto failed;
            }
            argv = grown;
        }
        argv[count] = strdup(token.data);
        oasp_buffer_dispose(&token);
        if (!argv[count])
            goto failed;
        count++;
        argv[count] = NULL;
    }
    return argv;

failed:
    oasp_process_argv_dispose(argv);
    return NULL;
}

static int64_t oasp_monotonic_milliseconds(void) {
    struct timespec time_value;
    if (clock_gettime(CLOCK_MONOTONIC, &time_value))
        return 0;
    return (int64_t)time_value.tv_sec * 1000 + time_value.tv_nsec / 1000000;
}

static void oasp_process_set_status(oasp_process *process, pid_t pid, int status) {
    process->last_pid = pid;
    process->last_error = 0;
    process->pid = 0;
    if (WIFEXITED(status)) {
        process->exit_code = WEXITSTATUS(status);
        process->term_signal = 0;
    } else if (WIFSIGNALED(status)) {
        process->term_signal = WTERMSIG(status);
        process->exit_code = 128 + process->term_signal;
    }
}

static int oasp_process_wait(oasp_process *process, pid_t pid, int64_t timeout_ms) {
    int64_t deadline = timeout_ms >= 0 ? oasp_monotonic_milliseconds() + timeout_ms : 0;
    for (;;) {
        int status = 0;
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited > 0) {
            oasp_process_set_status(process, waited, status);
            return (int)waited;
        }
        if (waited < 0) {
            process->last_error = errno;
            return -2;
        }
        if (!timeout_ms || (timeout_ms > 0 && oasp_monotonic_milliseconds() >= deadline))
            return -1;
        struct timespec delay = {0, 1000000};
        (void)nanosleep(&delay, NULL);
    }
}

static bool oasp_process_spawn(oasp_process *process, const char *executable, const char *arguments, const char *directory) {
    char **argv = oasp_process_argv(executable, arguments);
    if (!argv)
        return false;
    pid_t pid = fork();
    if (!pid) {
        if (directory && *directory && chdir(directory))
            _exit(126);
        execvp(executable, argv);
        _exit(127);
    }
    oasp_process_argv_dispose(argv);
    if (pid < 0)
        return false;
    process->pid = pid;
    process->exit_code = -1;
    process->term_signal = 0;
    process->timed_out = false;
    return true;
}

static void oasp_shell_write_error(int fd, int error_number) {
    const unsigned char *data = (const unsigned char *)&error_number;
    size_t written = 0;
    while (written < sizeof(error_number)) {
        ssize_t count = write(fd, data + written, sizeof(error_number) - written);
        if (count > 0)
            written += (size_t)count;
        else if (count < 0 && errno == EINTR)
            continue;
        else
            break;
    }
}

bool oasp_builtin_shell_execute(const char *executable, const char *arguments, const char *directory, int *launch_error) {
    char **argv = oasp_process_argv(executable, arguments);
    if (!argv) {
        *launch_error = errno ? errno : ENOMEM;
        return false;
    }
    int error_pipe[2];
    if (pipe(error_pipe)) {
        *launch_error = errno;
        oasp_process_argv_dispose(argv);
        return false;
    }
    if (fcntl(error_pipe[1], F_SETFD, FD_CLOEXEC) < 0) {
        *launch_error = errno;
        close(error_pipe[0]);
        close(error_pipe[1]);
        oasp_process_argv_dispose(argv);
        return false;
    }
    pid_t launcher = fork();
    if (launcher < 0) {
        *launch_error = errno;
        close(error_pipe[0]);
        close(error_pipe[1]);
        oasp_process_argv_dispose(argv);
        return false;
    }
    if (!launcher) {
        close(error_pipe[0]);
        if (setsid() < 0) {
            oasp_shell_write_error(error_pipe[1], errno);
            _exit(127);
        }
        pid_t child = fork();
        if (child < 0) {
            oasp_shell_write_error(error_pipe[1], errno);
            _exit(127);
        }
        if (child > 0)
            _exit(0);
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDIN_FILENO);
            (void)dup2(null_fd, STDOUT_FILENO);
            (void)dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
        long maximum = sysconf(_SC_OPEN_MAX);
        if (maximum < 0 || maximum > 1048576)
            maximum = 65536;
        for (int fd = 3; fd < maximum; fd++)
            if (fd != error_pipe[1])
                close(fd);
        if (directory && *directory && chdir(directory)) {
            oasp_shell_write_error(error_pipe[1], errno);
            _exit(127);
        }
        execvp(executable, argv);
        oasp_shell_write_error(error_pipe[1], errno);
        _exit(127);
    }
    close(error_pipe[1]);
    while (waitpid(launcher, NULL, 0) < 0 && errno == EINTR) {
    }
    ssize_t received;
    do {
        received = read(error_pipe[0], launch_error, sizeof(*launch_error));
    } while (received < 0 && errno == EINTR);
    close(error_pipe[0]);
    oasp_process_argv_dispose(argv);
    if (!received)
        return true;
    if (received != (ssize_t)sizeof(*launch_error))
        *launch_error = EIO;
    return false;
}

static ssize_t oasp_process_write(int fd, const void *data, size_t length) {
    sigset_t blocked;
    sigset_t previous;
    sigset_t pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    bool was_pending = sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) < 0)
        return -1;
    ssize_t result = write(fd, data, length);
    int saved_errno = errno;
    if (result < 0 && saved_errno == EPIPE && !was_pending) {
        int signal_number = 0;
        (void)sigwait(&blocked, &signal_number);
    }
    (void)sigprocmask(SIG_SETMASK, &previous, NULL);
    errno = saved_errno;
    return result;
}

static bool oasp_process_capture(oasp_runtime *runtime, oasp_process *process, const char *executable, const char *arguments, const char *directory,
                                 const char *input, size_t input_length, int64_t timeout_ms, size_t maximum) {
    int input_pipe[2] = {-1, -1};
    int output_pipe[2] = {-1, -1};
    int error_pipe[2] = {-1, -1};
    if (pipe(input_pipe))
        return false;
    if (pipe(output_pipe)) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        return false;
    }
    if (pipe(error_pipe)) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        return false;
    }
    char **argv = oasp_process_argv(executable, arguments);
    if (!argv) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        close(error_pipe[0]);
        close(error_pipe[1]);
        return false;
    }
    pid_t pid = fork();
    if (!pid) {
        (void)dup2(input_pipe[0], STDIN_FILENO);
        (void)dup2(output_pipe[1], STDOUT_FILENO);
        (void)dup2(error_pipe[1], STDERR_FILENO);
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        close(error_pipe[0]);
        close(error_pipe[1]);
        if (directory && *directory && chdir(directory))
            _exit(126);
        execvp(executable, argv);
        _exit(127);
    }
    oasp_process_argv_dispose(argv);
    if (pid < 0) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        close(error_pipe[0]);
        close(error_pipe[1]);
        return false;
    }
    close(input_pipe[0]);
    close(output_pipe[1]);
    close(error_pipe[1]);
    process->pid = pid;
    int input_flags = fcntl(input_pipe[1], F_GETFL);
    int output_flags = fcntl(output_pipe[0], F_GETFL);
    int error_flags = fcntl(error_pipe[0], F_GETFL);
    bool descriptors_ok = input_flags >= 0 && output_flags >= 0 && error_flags >= 0 &&
                          fcntl(input_pipe[1], F_SETFL, input_flags | O_NONBLOCK) == 0 &&
                          fcntl(output_pipe[0], F_SETFL, output_flags | O_NONBLOCK) == 0 &&
                          fcntl(error_pipe[0], F_SETFL, error_flags | O_NONBLOCK) == 0;
    size_t written = 0;
    oasp_buffer output = {0};
    oasp_buffer error = {0};
    bool buffers_ok = descriptors_ok && oasp_buffer_init(&output, 256) && oasp_buffer_init(&error, 256);
    bool input_open = true;
    bool output_open = true;
    bool error_open = true;
    bool completed = false;
    int status = 0;
    int64_t started = oasp_monotonic_milliseconds();
    if (!input_length) {
        close(input_pipe[1]);
        input_pipe[1] = -1;
        input_open = false;
    }
    while (buffers_ok && (!completed || input_open || output_open || error_open)) {
        struct pollfd descriptors[3] = {
            {input_open ? input_pipe[1] : -1, POLLOUT | POLLHUP, 0},
            {output_open ? output_pipe[0] : -1, POLLIN | POLLHUP, 0},
            {error_open ? error_pipe[0] : -1, POLLIN | POLLHUP, 0}
        };
        int ready;
        do {
            ready = poll(descriptors, 3, 10);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0) {
            buffers_ok = false;
            break;
        }
        if (input_open && descriptors[0].revents & POLLOUT) {
            ssize_t count = oasp_process_write(input_pipe[1], input + written, input_length - written);
            if (count > 0)
                written += (size_t)count;
            else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK && errno != EPIPE)
                buffers_ok = false;
            if (written == input_length || (count < 0 && errno == EPIPE)) {
                close(input_pipe[1]);
                input_pipe[1] = -1;
                input_open = false;
            }
        }
        if (input_open && descriptors[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            close(input_pipe[1]);
            input_pipe[1] = -1;
            input_open = false;
        }
        for (size_t i = 1; i < 3; i++) {
            if (!(descriptors[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
                continue;
            for (;;) {
                char chunk[4096];
                ssize_t count = read(descriptors[i].fd, chunk, sizeof(chunk));
                if (count > 0) {
                    oasp_buffer *target = i == 2 ? &error : &output;
                    size_t available = maximum > target->length ? maximum - target->length : 0;
                    size_t keep = (size_t)count < available ? (size_t)count : available;
                    if (keep && !oasp_buffer_append(target, chunk, keep))
                        buffers_ok = false;
                    continue;
                }
                if (!count) {
                    if (i == 2)
                        error_open = false;
                    else
                        output_open = false;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    buffers_ok = false;
                }
                break;
            }
        }
        if (!completed) {
            pid_t waited = waitpid(pid, &status, WNOHANG);
            completed = waited == pid;
            if (waited < 0 && errno != EINTR)
                buffers_ok = false;
        }
        if (timeout_ms >= 0 && oasp_monotonic_milliseconds() - started >= timeout_ms) {
            if (!completed) {
                (void)kill(pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                }
                completed = true;
                process->timed_out = true;
                process->exit_code = -1;
                process->term_signal = SIGKILL;
            }
            break;
        }
    }
    if (input_pipe[1] >= 0)
        close(input_pipe[1]);
    close(output_pipe[0]);
    close(error_pipe[0]);
    if (!completed) {
        (void)kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
    if (!process->timed_out)
        oasp_process_set_status(process, pid, status);
    process->standard_output = (oasp_slice){oasp_runtime_copy(runtime, output.data, output.length), output.length};
    process->standard_error = (oasp_slice){oasp_runtime_copy(runtime, error.data, error.length), error.length};
    oasp_buffer_dispose(&output);
    oasp_buffer_dispose(&error);
    process->pid = 0;
    return buffers_ok;
}

void oasp_builtin_process_finalize(void *context) {
    oasp_process *process = context;
    if (process->pid > 0) {
        (void)kill(process->pid, SIGTERM);
        (void)waitpid(process->pid, NULL, 0);
        process->pid = 0;
    }
}

bool oasp_builtin_process_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t count,
                               size_t line, oasp_value *result) {
    oasp_process *process = object->native;
    if (!oasp_builtin_feature_enabled("OPENASP_PROCESS_ENABLE")) {
        oasp_runtime_raise(runtime, 70, "OpenASP.Process is disabled", object->component_name, line);
        return true;
    }
    if (oasp_ascii_equal(member, "run") && count) {
        char *executable = oasp_value_format(runtime->arena, &arguments[0]);
        char *command_arguments = count > 1 ? oasp_value_format(runtime->arena, &arguments[1]) : "";
        char *directory = count > 2 ? oasp_value_format(runtime->arena, &arguments[2]) : "";
        char *input = count > 3 ? oasp_value_format(runtime->arena, &arguments[3]) : "";
        int64_t timeout = process->timeout_ms;
        int64_t maximum = (int64_t)process->max_output_bytes;
        if (count > 4)
            (void)oasp_value_to_int(&arguments[4], &timeout);
        if (count > 5)
            (void)oasp_value_to_int(&arguments[5], &maximum);
        process->timed_out = false;
        process->standard_output = (oasp_slice){"", 0};
        process->standard_error = (oasp_slice){"", 0};
        if (!executable || !*executable || timeout < 0 || maximum <= 0 ||
            !oasp_process_capture(runtime, process, executable, command_arguments, directory, input, input ? strlen(input) : 0, timeout, (size_t)maximum))
            oasp_runtime_raise(runtime, 5, "cannot run process", object->component_name, line);
        *result = oasp_component_object_value(object);
        return true;
    }
    if (oasp_ascii_equal(member, "start") && count) {
        if (process->pid > 0 && oasp_process_wait(process, process->pid, 0) == -1) {
            oasp_runtime_raise(runtime, 5, "Start requires an idle Process object", object->component_name, line);
            return true;
        }
        char *executable = oasp_value_format(runtime->arena, &arguments[0]);
        char *command_arguments = count > 1 ? oasp_value_format(runtime->arena, &arguments[1]) : "";
        char *directory = count > 2 ? oasp_value_format(runtime->arena, &arguments[2]) : "";
        if (!executable || !*executable || !oasp_process_spawn(process, executable, command_arguments, directory))
            oasp_runtime_raise(runtime, 5, "cannot start process", object->component_name, line);
        *result = oasp_component_object_value(object);
        return true;
    }
    if (oasp_ascii_equal(member, "wait")) {
        int64_t timeout = process->timeout_ms;
        if (count)
            (void)oasp_value_to_int(&arguments[0], &timeout);
        if (process->pid <= 0)
            oasp_runtime_raise(runtime, 5,
                               "Wait requires a child started by this "
                               "Process object",
                               object->component_name, line);
        else {
            int waited = oasp_process_wait(process, process->pid, timeout);
            *result = oasp_value_int(waited > 0 ? process->exit_code : -1);
        }
        return true;
    }
    if (oasp_ascii_equal(member, "terminate") || oasp_ascii_equal(member, "kill")) {
        int64_t signal_number = SIGTERM;
        if (count)
            (void)oasp_value_to_int(&arguments[0], &signal_number);
        bool success = process->pid > 0 && kill(process->pid, (int)signal_number) == 0;
        if (!success && process->pid <= 0)
            oasp_runtime_raise(runtime, 5,
                               "Terminate requires a running child started "
                               "by this Process object",
                               object->component_name, line);
        *result = oasp_value_bool(success);
        return true;
    }
    if (oasp_ascii_equal(member, "fork")) {
        oasp_runtime_debug_prepare_fork(runtime);
        pid_t pid = fork();
        oasp_runtime_debug_forked(runtime, pid);
        if (!pid) {
            oasp_runtime_reset_fork_children(runtime);
            *result = oasp_value_int(0);
        } else if (pid < 0) {
            oasp_runtime_raise(runtime, 5, "cannot fork process", object->component_name, line);
        } else if (!oasp_runtime_register_fork_child(runtime, pid)) {
            oasp_runtime_raise(runtime, 7, "cannot track fork child", object->component_name, line);
        } else {
            *result = oasp_value_int(pid);
        }
        return true;
    }
    if (oasp_ascii_equal(member, "waitpid")) {
        int64_t requested = -1;
        int64_t timeout = -1;
        if (count)
            (void)oasp_value_to_int(&arguments[0], &requested);
        if (count > 1)
            (void)oasp_value_to_int(&arguments[1], &timeout);
        int waited = oasp_process_wait(process, (pid_t)requested, timeout);
        if (waited > 0 && process->term_signal)
            process->exit_code = -1;
        *result = oasp_value_int(waited == -1 ? 0 : waited);
        return true;
    }
    if (oasp_ascii_equal(member, "signal") || oasp_ascii_equal(member, "killpid")) {
        int64_t pid = 0;
        int64_t signal_number = SIGTERM;
        if (count)
            (void)oasp_value_to_int(&arguments[0], &pid);
        if (count > 1)
            (void)oasp_value_to_int(&arguments[1], &signal_number);
        *result = oasp_value_bool(pid > 0 && kill((pid_t)pid, (int)signal_number) == 0);
        return true;
    }
    if (oasp_ascii_equal(member, "exit")) {
        int64_t exit_code = 0;
        if (count)
            (void)oasp_value_to_int(&arguments[0], &exit_code);
        _exit((int)exit_code);
    }
    return false;
}

bool oasp_builtin_process_get(oasp_object *object, oasp_slice member, oasp_value *result) {
    if (object->component_kind != OASP_COMPONENT_PROCESS)
        return false;
    oasp_process *process = object->native;
    if (oasp_ascii_equal(member, "stdout") || oasp_ascii_equal(member, "standardoutput"))
        *result = oasp_value_string(process->standard_output.data, process->standard_output.length);
    else if (oasp_ascii_equal(member, "stderr") || oasp_ascii_equal(member, "standarderror"))
        *result = oasp_value_string(process->standard_error.data, process->standard_error.length);
    else if (oasp_ascii_equal(member, "exitcode"))
        *result = oasp_value_int(process->exit_code);
    else if (oasp_ascii_equal(member, "timedout"))
        *result = oasp_value_bool(process->timed_out);
    else if (oasp_ascii_equal(member, "pid"))
        *result = oasp_value_int(process->pid);
    else if (oasp_ascii_equal(member, "lastpid"))
        *result = oasp_value_int(process->last_pid);
    else if (oasp_ascii_equal(member, "termsignal"))
        *result = oasp_value_int(process->term_signal);
    else if (oasp_ascii_equal(member, "lasterror"))
        *result = oasp_value_int(process->last_error);
    else if (oasp_ascii_equal(member, "isrunning"))
        *result = oasp_value_bool(process->pid > 0 && oasp_process_wait(process, process->pid, 0) == -1);
    else if (oasp_ascii_equal(member, "timeout"))
        *result = oasp_value_int(process->timeout_ms);
    else if (oasp_ascii_equal(member, "maxoutputbytes"))
        *result = oasp_value_int((int64_t)process->max_output_bytes);
    else
        return false;
    return true;
}

bool oasp_builtin_process_set(oasp_runtime *runtime, oasp_object *object, oasp_slice member, const oasp_value *value, size_t line) {
    if (object->component_kind != OASP_COMPONENT_PROCESS)
        return false;
    oasp_process *process = object->native;
    int64_t integer = 0;
    if (oasp_ascii_equal(member, "timeout")) {
        if (oasp_value_to_int(value, &integer) && integer > 0)
            process->timeout_ms = integer;
        else
            oasp_runtime_raise(runtime, 5, "invalid process timeout", object->component_name, line);
        return true;
    }
    if (oasp_ascii_equal(member, "maxoutputbytes")) {
        if (oasp_value_to_int(value, &integer) && integer > 0)
            process->max_output_bytes = (size_t)integer;
        else
            oasp_runtime_raise(runtime, 5, "invalid maximum process output size", object->component_name, line);
        return true;
    }
    return false;
}

bool oasp_builtin_shell_call(oasp_runtime *runtime, oasp_object *object, oasp_slice member, oasp_value *arguments, size_t argument_count,
                             size_t line, oasp_value *result) {
    if (object->component_kind != OASP_COMPONENT_SHELL)
        return false;
    if (!oasp_builtin_feature_enabled("OPENASP_SHELL_ENABLE")) {
        oasp_runtime_raise(runtime, 70, "Shell.Application is disabled", object->component_name, line);
        *result = oasp_value_empty();
        return true;
    }
    if (!oasp_ascii_equal(member, "shellexecute"))
        return false;
    char *executable = argument_count ? oasp_value_format(runtime->arena, &arguments[0]) : NULL;
    const char *trimmed = executable;
    while (trimmed && isspace((unsigned char)*trimmed))
        trimmed++;
    if (!trimmed || !*trimmed) {
        oasp_runtime_raise(runtime, 5, "ShellExecute requires a non-empty executable path", object->component_name, line);
        *result = oasp_value_empty();
        return true;
    }
    char *arguments_text = argument_count > 1 ? oasp_value_format(runtime->arena, &arguments[1]) : "";
    char *directory = argument_count > 2 ? oasp_value_format(runtime->arena, &arguments[2]) : "";
    char *operation = argument_count > 3 ? oasp_value_format(runtime->arena, &arguments[3]) : "";
    while (operation && isspace((unsigned char)*operation))
        operation++;
    size_t operation_length = operation ? strlen(operation) : 0;
    while (operation_length && isspace((unsigned char)operation[operation_length - 1]))
        operation_length--;
    if (operation_length && !oasp_ascii_equal_n(operation, operation_length, "open")) {
        char message[256];
        (void)snprintf(message, sizeof(message), "ShellExecute only supports the open operation; received: %.*s",
                       (int)operation_length, operation);
        oasp_runtime_raise(runtime, 5, message, object->component_name, line);
    } else {
        int launch_error = 0;
        if (!oasp_builtin_shell_execute(executable, arguments_text ? arguments_text : "",
                                        directory ? directory : "", &launch_error)) {
            char message[256];
            (void)snprintf(message, sizeof(message), "ShellExecute failed: cannot launch executable: %s (errno=%d)",
                           strerror(launch_error), launch_error);
            oasp_runtime_raise(runtime, 5, message, object->component_name, line);
        }
    }
    *result = oasp_value_empty();
    return true;
}
