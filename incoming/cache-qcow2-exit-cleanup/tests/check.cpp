// Copyright 2026 Ji Mao Cai
// SPDX-License-Identifier: GPL-2.0-only
#include "android/utils/tempfile.h"

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

static std::string StringFormat(const char* fmt, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    assert(n >= 0 && n < sizeof(buffer));
    return buffer;
}

static void write(const fs::path& path, const char* content = "owned") {
    std::ofstream out(path, std::ios::binary);
    out << content;
    assert(out.good());
}

static size_t countFiles(const fs::path& dir) {
    return std::distance(fs::directory_iterator(dir), fs::directory_iterator());
}

static void str_reset(char** target, const char* value) {
    char* copy = nullptr;
    if (value) {
        copy = static_cast<char*>(malloc(strlen(value) + 1));
        assert(copy);
        strcpy(copy, value);
    }
    free(*target);
    *target = copy;
}

static void str_reset_nocopy(char** target, char* value) {
    free(*target);
    *target = value;
}

static void str_reset_null(char** target) { str_reset(target, nullptr); }
static void dwarning(const char*, ...) {}
static void android_panic(const char*, ...) {}
static int path_exists(const char* path) { return path && fs::exists(path); }
static int android_createEmptyExt4Image(const char* path, uint64_t, const char*) {
    write(path, "raw-cache fixture");
    return 0;
}
#define D(...) ((void)0)

struct Hardware {
    int disk_cachePartition = 1;
    char* disk_cachePartition_path = nullptr;
    uint64_t disk_cachePartition_size = 0;
};
struct Options {
    bool no_cache = false;
    bool read_only = false;
    char* cache = nullptr;
};
static char* avdInfo_getCachePath(void*) {
    char* result = nullptr;
    str_reset(&result, getenv("PERMANENT_CACHE"));
    return result;
}
static char* avdInfo_getDefaultCachePath(void* avd) {
    return avdInfo_getCachePath(avd);
}

// Generated verbatim from main-common.c and main.cpp by test_cleanup.py.
#include "cache_flow.inc"

int main(int argc, char** argv) {
    assert(argc == 3);
    std::string scenario = argv[1];
    fs::path dir = argv[2];
    std::string path = (dir / "owned.qcow2").string();
    if (scenario == "register") {
        assert(!tempfile_register_path(nullptr));
        assert(!tempfile_register_path(""));
        assert(countFiles(dir) == 0);
        std::string mutablePath = path;
        TempFile* temp = tempfile_register_path(mutablePath.c_str());
        assert(temp);
        mutablePath.assign("different path");
        assert(path == tempfile_path(temp));
        assert(countFiles(dir) == 0);  // Registration must not create anything.
        write(path);                 // The file may appear after registration.
    } else if (scenario == "existing") {
        write(path, "preserve contents");
        write(dir / "unrelated", "sentinel");
        assert(tempfile_register_path(path.c_str()));
        assert(countFiles(dir) == 2);
        std::ifstream input(path);
        std::string content;
        std::getline(input, content);
        assert(content == "preserve contents");
    } else if (scenario == "close") {
        write(path);
        TempFile* temp = tempfile_register_path(path.c_str());
        assert(temp);
        tempfile_close(temp);
        tempfile_close(temp);
        assert(!fs::exists(path));
        tempfile_unref_and_close_file(temp);
        tempfile_unref_and_close(path.c_str());
    } else if (scenario == "missing") {
        assert(tempfile_register_path(path.c_str()));
    } else if (scenario == "duplicate") {
        assert(tempfile_register_path(path.c_str()));
        assert(tempfile_register_path(path.c_str()));
        assert(countFiles(dir) == 0);
        write(path);
    } else if (scenario == "raw" || scenario == "extension") {
        TempFile* temp = scenario == "raw" ? tempfile_create()
                                          : tempfile_create_with_ext(".ext");
        assert(temp && fs::exists(tempfile_path(temp)));
        assert(countFiles(dir) == 1);
        if (scenario == "extension") {
            assert(fs::path(tempfile_path(temp)).extension() == ".ext");
        }
    } else {
        Hardware hw;
        Options opts;
        if (scenario == "disabled" || scenario == "disabled-empty") {
            hw.disk_cachePartition = 0;
        }
        if (scenario == "disabled-empty") {
            str_reset(&hw.disk_cachePartition_path, "");
        }
        if (scenario == "no-cache") opts.no_cache = true;
        if (scenario == "read-only") opts.read_only = true;
        bool permanent = scenario == "default" || scenario == "read-only" ||
                         scenario == "default-missing";
        assert(permanent || scenario == "disabled" ||
               scenario == "disabled-empty" || scenario == "no-cache");
        if (permanent && scenario != "default-missing") {
            write(getenv("PERMANENT_CACHE"), "permanent raw");
        }
        assert(setupCache(&hw, &opts) == 0);
        std::string raw = hw.disk_cachePartition_path;
        std::string overlay = cacheDrivePath(&hw);
        assert(overlay == raw + ".qcow2");
        assert(fs::exists(raw));
        // Only QEMU's drive creation is replaced: create its exact sidecar path.
        assert(!fs::exists(overlay));
        write(overlay, "qcow2 fixture");
        if (permanent) {
            assert(raw == getenv("PERMANENT_CACHE"));
        } else {
            assert(raw != getenv("PERMANENT_CACHE"));
        }
        free(hw.disk_cachePartition_path);
        free(opts.cache);
    }
    return 0;  // Exercise the real C runtime atexit callback, in a child process.
}
