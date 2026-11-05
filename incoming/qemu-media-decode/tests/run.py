"""Build the focused Windows media test in an x64 VS developer shell."""
import argparse
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--src", type=Path, required=True, help="mas37 AOSP source root")
parser.add_argument("--qemu", type=Path, help="optional QEMU checkout under test")
parser.add_argument("--out", type=Path, required=True)
parser.add_argument("ivf", type=Path, help="sample.ivf in the directory created by fixtures.py")
args = parser.parse_args()
src = args.src.resolve()
qemu = (args.qemu or src / "external/qemu").resolve()
aemu = src / "hardware/google/aemu"
gfx = src / "hardware/google/gfxstream"
media = qemu / "android/emu/media/src/android/emulation"
common = src / "prebuilts/android-emulator-build/common"
def prebuilt(name):
    return common / name / "windows_msvc-x86_64"

includes = [
    qemu / "android/emu/host-common/include", qemu / "android/emu/media/src",
    qemu / "android/android-emu-base", qemu / "android/emu/base/include",
    aemu / "base/include", aemu / "host-common/include", aemu / "windows/includes",
    gfx / "host/include", gfx / "host/features/include", gfx / "third_party/opengl/include",
    src / "external/abseil-cpp",
    prebuilt("libvpx") / "include", prebuilt("ffmpeg") / "include",
]
units = [
    "MediaVpxDecoder", "MediaVpxDecoderGeneric", "MediaVpxVideoHelper",
    "MediaH264Decoder", "MediaH264DecoderDefault", "MediaH264DecoderGeneric",
    "MediaHevcDecoder", "MediaHevcDecoderDefault", "MediaHevcDecoderGeneric",
    "MediaFfmpegVideoHelper", "MediaVideoHelper", "MediaSnapshotHelper",
    "MediaSnapshotState", "MediaHostRenderer", "MediaTexturePool",
    "H264NaluParser", "HevcNaluParser", "VpxFrameParser",
    "VpxPingInfoParser", "H264PingInfoParser", "HevcPingInfoParser",
]
test = Path(__file__).with_name("media_test.cpp").resolve()
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=True)
exe = out / "media_test.exe"
llvm = src / "prebuilts/clang/host/windows-x86/clang-r596125/bin"
cmd = [llvm / "clang-cl.exe", "/nologo", "/std:c++17", "/EHsc", "/MD", "/Gy", "/O1",
       "/D_CRT_SECURE_NO_WARNINGS", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN",
       "-Wno-return-type-c-linkage", "-Wno-extern-c-compat", "-Wno-c++11-narrowing",
       "/Ddprint(...)=((void)0)", "/Dderror(...)=abort()", "/Ddfatal(...)=abort()",
       *("/I" + str(p) for p in includes), test,
       *(media / (u + ".cpp") for u in units),
       *(aemu / "base" / (u + ".cpp") for u in ("Stream", "MemStream")), "/c"]
subprocess.run(cmd, cwd=out, check=True)
objects = [out / (name + ".obj") for name in [test.stem, *units, "Stream", "MemStream"]]
subprocess.run([llvm / "lld-link.exe", "/nologo", "/OPT:REF", "/OUT:" + str(exe), *objects,
                prebuilt("libvpx") / "lib/libvpx.lib",
                *(prebuilt("ffmpeg") / "lib" / (u + ".lib") for u in ("libavcodec", "libavutil", "libswresample")),
                "mfplat.lib", "mfuuid.lib", "strmiids.lib", "ole32.lib", "user32.lib", "bcrypt.lib"],
               cwd=out, check=True)
failed = []
cpu_outputs = {}
for mode in ("default", "h264", "fallback", "replay", "owners", "restored"):
    result = subprocess.run([exe, mode, args.ivf.resolve()], cwd=out, stdout=subprocess.PIPE, text=True)
    print(result.stdout, end="", flush=True)
    if result.returncode:
        failed.append((mode, hex(result.returncode & 0xffffffff)))
    elif mode in ("default", "fallback"):
        cpu_outputs[mode] = re.search(r"^frames=(\d+) pixel hash=([0-9a-f]+)$", result.stdout, re.M).groups()
if len(cpu_outputs) == 2 and cpu_outputs["default"] != cpu_outputs["fallback"]:
    failed.append(("VP9 output differs after fallback", cpu_outputs))
if failed:
    raise SystemExit(f"Failed cases: {failed}")
