// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#define _POSIX_C_SOURCE 200809L

#include "cli_shell.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *shell_path(void) {
    const char *shell = getenv("SHELL");
    if (shell && shell[0] == '/' && access(shell, X_OK) == 0)
        return shell;
    return "/bin/sh";
}

static bool ignore_terminal_signals(struct sigaction *old_interrupt, struct sigaction *old_quit) {
    struct sigaction ignore;
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGINT, &ignore, old_interrupt) != 0)
        return false;
    if (sigaction(SIGQUIT, &ignore, old_quit) == 0)
        return true;
    int saved_error = errno;
    (void)sigaction(SIGINT, old_interrupt, NULL);
    errno = saved_error;
    return false;
}

static void restore_terminal_signals(const struct sigaction *old_interrupt, const struct sigaction *old_quit) {
    (void)sigaction(SIGINT, old_interrupt, NULL);
    (void)sigaction(SIGQUIT, old_quit, NULL);
}

static void reset_child_terminal_signals(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGQUIT, &action, NULL);
}

int oasp_cli_shell_execute(const char *command, size_t length) {
    if (!command) {
        errno = EINVAL;
        return -1;
    }
    while (length && (command[length - 1] == '\n' || command[length - 1] == '\r'))
        length--;
    while (length && (*command == ' ' || *command == '\t')) {
        command++;
        length--;
    }
    if (!length) {
        fputs("openasp-cli: shell command is empty; enter ! followed by a command\n", stderr);
        return -1;
    }
    if (memchr(command, '\0', length)) {
        fputs("openasp-cli: shell command contains a NUL byte\n", stderr);
        return -1;
    }

    char *owned = malloc(length + 1);
    if (!owned) {
        fputs("openasp-cli: cannot allocate shell command\n", stderr);
        return -1;
    }
    memcpy(owned, command, length);
    owned[length] = '\0';

    struct sigaction old_interrupt;
    struct sigaction old_quit;
    if (!ignore_terminal_signals(&old_interrupt, &old_quit)) {
        fprintf(stderr, "openasp-cli: cannot prepare shell signals: %s\n", strerror(errno));
        free(owned);
        return -1;
    }

    const char *shell = shell_path();
    fflush(NULL);
    pid_t child = fork();
    if (child == 0) {
        reset_child_terminal_signals();
        execl(shell, shell, "-c", owned, (char *)NULL);
        fprintf(stderr, "openasp-cli: cannot execute shell %s: %s\n", shell, strerror(errno));
        _exit(127);
    }
    if (child < 0) {
        int saved_error = errno;
        restore_terminal_signals(&old_interrupt, &old_quit);
        fprintf(stderr, "openasp-cli: cannot start shell: %s\n", strerror(saved_error));
        free(owned);
        errno = saved_error;
        return -1;
    }

    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    int saved_error = errno;
    restore_terminal_signals(&old_interrupt, &old_quit);
    free(owned);
    if (waited < 0) {
        fprintf(stderr, "openasp-cli: cannot wait for shell command: %s\n", strerror(saved_error));
        errno = saved_error;
        return -1;
    }
    if (WIFEXITED(status)) {
        int exit_status = WEXITSTATUS(status);
        return exit_status;
    }
    if (WIFSIGNALED(status)) {
        int signal_number = WTERMSIG(status);
        return 128 + signal_number;
    }
    return -1;
}
