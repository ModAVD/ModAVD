// Copyright 2026 Ji Mao Cai
// SPDX-License-Identifier: GPL-2.0-only
#include "windows-temp-images.h"
#include <windows.h>

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
    const auto avd = fs::path(argv[1]).u8string();
    const auto temp = fs::path(argv[2]).u8string();
    if (argc == 4 && std::wstring(argv[3]) == L"recover") {
        const auto probe = "recovery-probe-" + std::to_string(GetCurrentProcessId());
        return create(avd, temp, probe, Kind::HardwareIni).empty() ? 4 : 0;
    }
    if (argc != 5) return 2;
    if (std::wstring(argv[4]) == L"lazy") {
        if (takeReservedOverlay("permanent.qcow2") || release("permanent.qcow2")) return 10;
        std::cout << "idle" << std::endl;
        std::string command;
        if (!std::getline(std::cin, command) || command != "allocate") return 0;
    }
    if (std::wstring(argv[4]) == L"pin-only") {
        const auto pin = create(avd, temp, "directory-probe", Kind::HardwareIni);
        if (pin.empty() || !release(pin.c_str()) || fs::exists(fs::u8path(pin))) return 4;
        std::cout << "ready" << std::endl;
        std::string command;
        std::getline(std::cin, command);
        return 0;
    }
    auto stem = fs::path(argv[3]).u8string();
    auto cache = create(avd, temp, stem, Kind::CachePair);
    if (std::wstring(argv[4]) == L"collision") return cache.empty() ? 0 : 3;
    if (cache.empty()) return 4;
    auto ini = create(avd, temp, stem + "-ini", Kind::HardwareIni);
    auto overlay = create(avd, temp, stem + "-drive", Kind::DriveOverlay);
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
    if (command == "allocate-again") {
        if (create(avd, temp, stem + "-later", Kind::HardwareIni).empty()) return 4;
        std::cout << "ready-again" << std::endl;
        std::getline(std::cin, command);
    }
    if (command == "retarget") {
        if (!create(avd + "-other", temp, stem + "-other", Kind::HardwareIni).empty() ||
            !create(avd, temp + "-other", stem + "-other", Kind::HardwareIni).empty()) return 11;
    }
    if (command == "rewrite") {
        std::ofstream out(fs::u8path(cache), std::ios::binary | std::ios::trunc);
        out << "second write";
        out.close();
        if (!out) return 8;
    }
    if (command == "release" && (!release(overlay.c_str()) || fs::exists(fs::u8path(overlay)))) return 7;
    return 0;
}
