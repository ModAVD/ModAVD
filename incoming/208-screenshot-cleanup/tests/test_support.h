// Copyright 2026 Ji Mao Cai
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

inline std::mutex logMutex;
inline std::vector<std::string> logs;
inline void capture_log(const char* format, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    std::lock_guard<std::mutex> lock(logMutex);
    logs.emplace_back(buffer);
}
inline void check(bool condition, const char* text, int line) {
    if (condition) return;
    std::fprintf(stderr, "FAILED line %d: %s\n", line, text);
    const size_t start = logs.size() > 5 ? logs.size() - 5 : 0;
    for (size_t i = start; i < logs.size(); ++i) std::fprintf(stderr, "%s\n", logs[i].c_str());
    std::exit(1);
}
#define CHECK(condition) check((condition), #condition, __LINE__)
#define GFXSTREAM_INFO(...) capture_log(__VA_ARGS__)
#define GFXSTREAM_ERROR(...) capture_log(__VA_ARGS__)
#define GFXSTREAM_WARNING(...) capture_log(__VA_ARGS__)
#define GFXSTREAM_DEBUG(...) capture_log(__VA_ARGS__)
#define GL_SCOPED_DEBUG_GROUP(...) ((void)0)
