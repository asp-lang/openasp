// Copyright (c) 2026 https://asp-lang.org
// SPDX-License-Identifier: MIT

#define _XOPEN_SOURCE 700

#include "line_editor.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

static bool write_all(const char *data, size_t length) {
    while (length) {
        ssize_t written = write(STDOUT_FILENO, data, length);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        data += (size_t)written;
        length -= (size_t)written;
    }
    return true;
}

static size_t previous_character(const char *line, size_t cursor) {
    if (!cursor)
        return 0;
    cursor--;
    while (cursor && ((unsigned char)line[cursor] & 0xc0u) == 0x80u)
        cursor--;
    return cursor;
}

static size_t next_character(const char *line, size_t length, size_t cursor) {
    if (cursor >= length)
        return length;
    cursor++;
    while (cursor < length && ((unsigned char)line[cursor] & 0xc0u) == 0x80u)
        cursor++;
    return cursor;
}

static size_t display_columns(const char *data, size_t length, size_t initial) {
    mbstate_t state = {0};
    size_t columns = initial;
    size_t offset = 0;
    while (offset < length) {
        if ((unsigned char)data[offset] == 0x1bu && offset + 1 < length &&
            data[offset + 1] == '[') {
            offset += 2;
            while (offset < length) {
                unsigned char byte = (unsigned char)data[offset++];
                if (byte >= 0x40u && byte <= 0x7eu)
                    break;
            }
            memset(&state, 0, sizeof(state));
            continue;
        }
        if (data[offset] == '\t') {
            columns += 8 - columns % 8;
            offset++;
            memset(&state, 0, sizeof(state));
            continue;
        }
        wchar_t character;
        size_t consumed = mbrtowc(&character, data + offset, length - offset, &state);
        if (consumed == (size_t)-1 || consumed == (size_t)-2) {
            columns++;
            offset++;
            memset(&state, 0, sizeof(state));
            continue;
        }
        if (!consumed)
            consumed = 1;
        int width = wcwidth(character);
        if (width > 0)
            columns += (size_t)width;
        offset += consumed;
    }
    return columns;
}

static bool refresh_line(const char *prompt, const char *line, size_t length, size_t cursor) {
    static const char clear_to_end[] = "\x1b[K";
    if (!write_all("\r", 1) || !write_all(prompt, strlen(prompt)) ||
        !write_all(line, length) || !write_all(clear_to_end, sizeof(clear_to_end) - 1))
        return false;
    size_t prompt_columns = display_columns(prompt, strlen(prompt), 0);
    size_t end_columns = display_columns(line, length, prompt_columns);
    size_t cursor_columns = display_columns(line, cursor, prompt_columns);
    size_t move_left = end_columns - cursor_columns;
    if (!move_left)
        return true;
    char sequence[32];
    int sequence_length = snprintf(sequence, sizeof(sequence), "\x1b[%zuD", move_left);
    return sequence_length > 0 && (size_t)sequence_length < sizeof(sequence) &&
           write_all(sequence, (size_t)sequence_length);
}

static bool reserve_line(char **line, size_t *capacity, size_t required) {
    if (*capacity >= required)
        return true;
    size_t next = *capacity ? *capacity : 128;
    while (next < required) {
        if (next > SIZE_MAX / 2) {
            errno = ENOMEM;
            return false;
        }
        next *= 2;
    }
    char *resized = realloc(*line, next);
    if (!resized) {
        errno = ENOMEM;
        return false;
    }
    *line = resized;
    *capacity = next;
    return true;
}

static bool read_byte(unsigned char *byte) {
    for (;;) {
        ssize_t count = read(STDIN_FILENO, byte, 1);
        if (count == 1)
            return true;
        if (count == 0)
            return false;
        if (errno != EINTR)
            return false;
    }
}

static bool byte_available(void) {
    struct pollfd descriptor = {.fd = STDIN_FILENO, .events = POLLIN};
    int result;
    do {
        result = poll(&descriptor, 1, 50);
    } while (result < 0 && errno == EINTR);
    return result > 0 && (descriptor.revents & POLLIN) != 0;
}

ssize_t oasp_line_editor_readline(const char *prompt, char **line, size_t *capacity) {
    if (!prompt || !line || !capacity) {
        errno = EINVAL;
        return -1;
    }
    struct termios saved;
    if (tcgetattr(STDIN_FILENO, &saved) != 0)
        return -1;
    struct termios editing = saved;
    editing.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
    editing.c_iflag &= (tcflag_t)~(ICRNL | IXON);
    editing.c_cc[VMIN] = 1;
    editing.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &editing) != 0)
        return -1;

    size_t length = 0;
    size_t cursor = 0;
    bool ok = reserve_line(line, capacity, 2) && write_all(prompt, strlen(prompt));
    bool eof = false;
    while (ok) {
        unsigned char byte;
        if (!read_byte(&byte)) {
            eof = true;
            break;
        }
        if (byte == '\r' || byte == '\n') {
            ok = reserve_line(line, capacity, length + 2);
            if (ok) {
                (*line)[length++] = '\n';
                (*line)[length] = '\0';
                ok = write_all("\r\n", 2);
            }
            break;
        }
        if (byte == 3) {
            length = 0;
            cursor = 0;
            ok = reserve_line(line, capacity, 2);
            if (ok) {
                (*line)[length++] = '\n';
                (*line)[length] = '\0';
                ok = write_all("^C\r\n", 4);
            }
            break;
        }
        if (byte == 4) {
            if (!length) {
                eof = true;
                break;
            }
            if (cursor < length) {
                size_t next = next_character(*line, length, cursor);
                memmove(*line + cursor, *line + next, length - next);
                length -= next - cursor;
                (*line)[length] = '\0';
                ok = refresh_line(prompt, *line, length, cursor);
            }
            continue;
        }
        if (byte == 127 || byte == 8) {
            if (cursor) {
                size_t previous = previous_character(*line, cursor);
                memmove(*line + previous, *line + cursor, length - cursor);
                length -= cursor - previous;
                cursor = previous;
                (*line)[length] = '\0';
                ok = refresh_line(prompt, *line, length, cursor);
            }
            continue;
        }
        if (byte == 27) {
            unsigned char first;
            unsigned char second;
            if (!byte_available() || !read_byte(&first) || first != '[' ||
                !byte_available() || !read_byte(&second))
                continue;
            if (second == 'D')
                cursor = previous_character(*line, cursor);
            else if (second == 'C')
                cursor = next_character(*line, length, cursor);
            else if (second == 'H')
                cursor = 0;
            else if (second == 'F')
                cursor = length;
            else if (second == '3' && byte_available()) {
                unsigned char final;
                if (read_byte(&final) && final == '~' && cursor < length) {
                    size_t next = next_character(*line, length, cursor);
                    memmove(*line + cursor, *line + next, length - next);
                    length -= next - cursor;
                    (*line)[length] = '\0';
                }
            }
            ok = refresh_line(prompt, *line, length, cursor);
            continue;
        }
        if (byte < 32 && byte != '\t')
            continue;
        if (!reserve_line(line, capacity, length + 2)) {
            ok = false;
            break;
        }
        memmove(*line + cursor + 1, *line + cursor, length - cursor);
        (*line)[cursor++] = (char)byte;
        (*line)[++length] = '\0';
        ok = refresh_line(prompt, *line, length, cursor);
    }

    int saved_error = errno;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &saved) != 0 && ok)
        ok = false;
    if (!ok) {
        errno = saved_error ? saved_error : EIO;
        return -1;
    }
    if (eof) {
        (void)write_all("\r\n", 2);
        return -1;
    }
    return (ssize_t)length;
}
