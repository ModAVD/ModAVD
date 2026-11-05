// Copyright 2026 Ji Mao Cai
// SPDX-License-Identifier: GPL-2.0-only
#include "windows-temp-images.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) return 2;
    namespace fs = std::filesystem;
    using namespace android::tempimages;
    if (argc == 5 && std::wstring(argv[4]) == L"race") {
        std::cout << "waiting" << std::endl;
        std::string signal;
        if (!std::getline(std::cin, signal) || signal != "go") return 9;
    }
    start(fs::path(argv[1]).u8string(), fs::path(argv[2]).u8string());
    if (argc == 4 && std::wstring(argv[3]) == L"recover") return 0;
    if (argc != 5) return 2;
    auto stem = fs::path(argv[3]).u8string();
    auto cache = create(stem, Kind::CachePair);
    if (std::wstring(argv[4]) == L"collision") return cache.empty() ? 0 : 3;
    if (cache.empty()) return 4;
    auto ini = create(stem + "-ini", Kind::HardwareIni);
    auto overlay = create(stem + "-drive", Kind::DriveOverlay);
    if (ini.empty() || overlay.empty()) return 4;
    auto sidecar = cache + ".qcow2";
    if (!takeReservedOverlay(sidecar.c_str()) || takeReservedOverlay(sidecar.c_str()) ||
        takeReservedOverlay(overlay.c_str())) return 5;
    for (auto& path : {cache, sidecar, ini, overlay}) {
        std::ofstream out(fs::u8path(path), std::ios::binary | std::ios::trunc);
        out << "created by " << stem;
        out.close();
        if (!out) return 6;
    }
    if (std::wstring(argv[4]) == L"paths") {
        std::cout << cache << '\n' << overlay << '\n';
    }
    std::cout << "ready" << std::endl;
    std::string command;
    std::getline(std::cin, command);
    if (command == "rewrite") {
        std::ofstream out(fs::u8path(cache), std::ios::binary | std::ios::trunc);
        out << "second write";
        out.close();
        if (!out) return 8;
    }
    if (command == "release" && (!release(overlay.c_str()) || fs::exists(fs::u8path(overlay)))) return 7;
    return 0;
}
