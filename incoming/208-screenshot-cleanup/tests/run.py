# Copyright 2026 Ji Mao Cai
# SPDX-License-Identifier: Apache-2.0
"""Compile unchanged production function bodies against deterministic mock APIs.

No Android image, emulator, GPU, third-party Python package, or network is needed.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def block(source, signature, optional=False):
    start = source.find(signature)
    if start < 0:
        if optional:
            return ""
        raise ValueError(f"Missing production definition: {signature}")
    opening = source.index("{", start)
    token = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]')
    depth = 0
    for match in token.finditer(source, opening):
        if match[0] == "{":
            depth += 1
        elif match[0] == "}":
            depth -= 1
            if depth == 0:
                end = match.end()
                if source[end:end + 1] == ";":
                    end += 1
                return source[start:end]
    raise ValueError(f"Unclosed production definition: {signature}")


def compiler():
    cxx = os.environ.get("CXX")
    if cxx:
        return Path(shutil.which(cxx) or cxx), None
    if os.name != "nt":
        cxx = shutil.which("c++") or shutil.which("clang++")
        if not cxx:
            raise RuntimeError("Set CXX to a C++17 compiler")
        return Path(cxx), None
    installed = Path(os.environ.get("ProgramFiles(x86)", "")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    if not installed.is_file():
        raise RuntimeError("Set CXX, or install the Visual Studio C++ build tools")
    root = Path(subprocess.check_output([str(installed), "-latest", "-products", "*", "-property", "installationPath"], text=True).strip())
    matches = sorted((root / "VC/Tools/MSVC").glob("*/bin/Hostx64/x64/cl.exe"))
    if not matches:
        raise RuntimeError("Visual Studio C++ compiler not found")
    return matches[-1], root / "VC/Auxiliary/Build/vcvars64.bat"


def replacements(repo, name):
    def read(path):
        return (repo / path).read_text(encoding="utf-8")
    if name == "wgl":
        source = read("host/gl/glestranslator/egl/egl_os_api_wgl.cpp")
        return {
            "BACKLOG_LOG_KIND": "QueueFull" if "AllocationPaused" not in source else "AllocationPaused",
            "WGL_FAILURE_KIND": ("#define HAS_WGL_FAILURE_KINDS 1\n" + block(source, "enum class WglFailure")) if "enum class WglFailure" in source else "",
            "CREATE_RESULT": block(source, "    enum class PbufferCreateError", optional=True) + ("\n" + re.search(r"    using PbufferCreation[^;]+;", source)[0] if "using PbufferCreation" in source else ""),
            "ABANDON_PBUFFER": block(source, "    void abandonPbufferLocked(", optional=True),
            "EXPIRE_PBUFFERS": block(source, "    void expirePbuffersLocked(", optional=True),
            "WGL_LOG_LIMITER": ("#define HAS_WGL_FAILURE_LIMITER 1\n" + block(source, "class WglFailureLog")) if "class WglFailureLog" in source else "",
            "PENDING_TYPE": ("#define HAS_BOUNDED_PBUFFERS 1\n" + block(source, "    struct PendingPbuffer")) if "    struct PendingPbuffer" in source else "",
            "QUEUE_PENDING": block(source, "    void queuePbufferForDeletionLocked(", optional=True),
            "PBUFFER_LIMITS": "\n".join(re.findall(r"    static constexpr [^;]+(?:kPbufferBacklogBudget|kPbufferRetryBatch|kMaxPbufferRetries|kPbufferLifetime)[^;]*;", source)),
            "WIN_SURFACE": block(source, "class WinSurface :"),
            "CREATE_SURFACE": block(source, "    virtual EglOS::Surface* createPbufferSurface("),
            "RELEASE_SURFACE": block(source, "    virtual bool releasePbuffer("),
            "CREATE_IMPL": block(source, "    PbufferCreation createPbufferSurfaceImpl(") if "    PbufferCreation createPbufferSurfaceImpl(" in source else block(source, "    EglOS::Surface* createPbufferSurfaceImpl("),
            "LOG_STATS": block(source, "    void logPbufferStatsLocked(", optional=True),
        }
    if name == "cleanup":
        source = read("host/gl/color_buffer_gl.cpp")
        resize = read("host/gl/texture_resize.cpp")
        yuv = read("host/gl/yuv_converter.cpp")
        return {
            "SURFACE_DTOR": block(read("host/gl/display_surface_gl.cpp"), "DisplaySurfaceGl::~DisplaySurfaceGl()"),
            "CLEANUP": block(source, "struct ColorBufferGlCleanup"),
            "CB_CTOR": block(source, "ColorBufferGl::ColorBufferGl("),
            "CB_DTOR": block(source, "ColorBufferGl::~ColorBufferGl()"),
            "RESIZER_DTOR": block(resize, "TextureResize::~TextureResize()"),
            "GENERIC_DTOR": block(resize, "TextureResize::GenericResizer::~GenericResizer()"),
            "YUV_DTOR": block(yuv, "YUVConverter::~YUVConverter()"),
            "READ_PIXELS": block(source, "bool ColorBufferGl::readPixels("),
            "CONVERT": block(source, "static void convertRgbaToRgbPixels("),
            "RESIZER_ABANDON": block(read("host/gl/texture_resize.h"), "    void abandonGlResources()"),
            "GENERIC_ABANDON": block(read("host/gl/texture_resize.h").split("class GenericResizer", 1)[1], "void abandonGlResources()"),
            "YUV_ABANDON": block(read("host/gl/yuv_converter.h"), "void abandonGlResources()"),
        }
    if name == "diagnostics":
        source = read("host/render_control.cpp")
        fb = read("host/frame_buffer.cpp")
        return {
            "READ_IMPL": block(fb, "bool FrameBuffer::Impl::readColorBuffer("),
            "READ_DEPRECATED_IMPL": block(fb, "bool FrameBuffer::Impl::readColorBufferDeprecated("),
            "READ_PUBLIC": block(fb, "bool FrameBuffer::readColorBuffer("),
            "READ_DEPRECATED_PUBLIC": block(fb, "bool FrameBuffer::readColorBufferDeprecated("),
            "GUEST": block(source, "static void rcReadColorBuffer("),
            "DMA": block(source, "static int rcReadColorBufferDMA("),
            "SCREENSHOT": block(fb, "int FrameBuffer::getScreenshot("),
        }
    raise ValueError(name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True, help="Patched gfxstream source checkout")
    parser.add_argument("--work", type=Path, help="Build output directory, outside the source checkout")
    parser.add_argument("--only", choices=["wgl", "cleanup", "diagnostics"])
    parser.add_argument("--case", choices=["pool-fallback", "initial-dc", "full-recovery"], help="Run one WGL creation regression")
    args = parser.parse_args()
    if args.case and args.only != "wgl":
        parser.error("--case requires --only wgl")
    repo = args.repo.resolve()
    work = (args.work or Path(tempfile.mkdtemp(prefix="gfxstream-208-tests-"))).resolve()
    if work.is_relative_to(repo):
        parser.error("--work must be outside the source checkout")
    work.mkdir(parents=True, exist_ok=True)
    stubs = work / "stubs/gfxstream/common"
    stubs.mkdir(parents=True, exist_ok=True)
    (stubs / "logging.h").write_text("#pragma once\n", encoding="utf-8")
    fixture_dir = Path(__file__).resolve().parent
    cxx, vcvars = compiler()
    results = {}
    for name in [args.only] if args.only else ["wgl", "cleanup", "diagnostics"]:
        text = (fixture_dir / f"{name}.cpp.in").read_text(encoding="utf-8")
        for marker, definition in replacements(repo, name).items():
            text = text.replace(f"@{marker}@", definition)
        if re.search(r"@[A-Z_]+@", text):
            raise ValueError("Unexpanded fixture marker")
        cpp = work / f"{name}.cpp"
        exe = work / (name + (".exe" if os.name == "nt" else ""))
        cpp.write_text(text, encoding="utf-8")
        include = [fixture_dir, work / "stubs", repo / "host/include", repo / "host/gl", repo / "third_party/opengl/include"]
        if cxx.name.lower() in ["cl", "cl.exe", "clang-cl", "clang-cl.exe"]:
            command = [str(cxx), "/nologo", "/std:c++17", "/EHsc", "/W4", *[f"/I{p}" for p in include], str(cpp), f"/Fe:{exe}", f"/Fo:{work / (name + '.obj')}"]
        else:
            command = [str(cxx), "-std=c++17", "-Wall", "-Wextra", "-pthread", *[f"-I{p}" for p in include], str(cpp), "-o", str(exe)]
        if vcvars:
            script = work / f"build-{name}.cmd"
            script.write_text('@echo off\ncall "' + str(vcvars) + '" >nul\nif errorlevel 1 exit /b 1\n' + subprocess.list2cmdline(command) + "\n", encoding="utf-8")
            command = ["cmd.exe", "/d", "/c", str(script)]
        build_env = dict(os.environ, VSLANG="1033")
        built = subprocess.run(command, cwd=work, env=build_env, capture_output=True)
        for attribute in ["stdout", "stderr"]:
            data = getattr(built, attribute)
            try:
                decoded = data.decode("utf-8")
            except UnicodeDecodeError:
                decoded = data.decode("mbcs" if os.name == "nt" else "utf-8", errors="replace")
            setattr(built, attribute, decoded)
        if built.returncode:
            print(built.stdout + built.stderr)
            built.check_returncode()
        run = subprocess.run([str(exe), *([args.case] if args.case else [])], cwd=work, capture_output=True, text=True, errors="replace", timeout=30)
        print(run.stdout, end="")
        if run.returncode:
            print(run.stderr)
            run.check_returncode()
        results[name] = {"output": run.stdout, "compiler_output": built.stdout + built.stderr}
    (work / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(f"Validated {len(results)} production-code probes; results in {work}")


if __name__ == "__main__":
    main()
