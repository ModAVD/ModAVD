// Copyright 2026 Ji Mao Cai
// SPDX-License-Identifier: GPL-2.0-only
// External logging/path adapters for the standalone, real tempfile.c tests.
#include "android/utils/bufprint.h"
#include "android/utils/debug.h"
#include "android/utils/file_io.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* bufprint(char* buffer, char* end, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buffer, end - buffer, fmt, args);
    va_end(args);
    return n < 0 || n >= end - buffer ? end : buffer + n;
}

char* bufprint_temp_dir(char* buffer, char* end) {
    return bufprint(buffer, end, "%s", getenv("TEMPFILE_TEST_DIR"));
}

char* bufprint_temp_file(char* buffer, char* end, const char* suffix) {
    return bufprint(buffer, end, "%s/%s", getenv("TEMPFILE_TEST_DIR"), suffix);
}

int android_unlink(const char* path) {
    return remove(path);
}

void dwarning(const char* fmt, ...) {
    (void)fmt;
}
