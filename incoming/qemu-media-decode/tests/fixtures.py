"""Create deterministic testsrc2 clips with FFmpeg 7.1."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("directory", type=Path)
parser.add_argument("--ffmpeg", default="ffmpeg")
args = parser.parse_args()
args.directory.mkdir(parents=True, exist_ok=True)

def ffmpeg(*arguments):
    subprocess.run([args.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y", *arguments],
                   check=True, cwd=args.directory)

for encoder, name, size, count in (
    ("libvpx-vp9", "sample.ivf", "128x96", 24),
    ("libvpx", "vp8.ivf", "128x96", 24),
    ("libx264", "sample.h264", "128x96", 24),
    ("libx264", "key.h264", "1920x1080", 1),
    ("libx265", "key.hevc", "1920x1080", 1),
):
    ffmpeg("-f", "lavfi", "-i", f"testsrc2=size={size}:rate=12", "-frames:v", str(count),
           "-pix_fmt", "yuv420p", "-g", "24", "-threads", "1", "-c:v", encoder, "-bf", "0", name)
ffmpeg("-i", "sample.ivf", "-c", "copy", "sample.webm")
ffmpeg("-framerate", "12", "-i", "sample.h264", "-c", "copy", "sample.mp4")
ffmpeg("-i", "sample.mp4", "-c", "copy", "-bsf:v", "h264_mp4toannexb",
       "-f", "image2", "access-%02d.h264")
