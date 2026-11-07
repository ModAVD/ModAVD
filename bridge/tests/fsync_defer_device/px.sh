#!/system/bin/sh
# Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later.
# print "r g b" of screen pixels: px.sh x1 y1 [x2 y2 ...]  (raw screencap: 16-byte header, RGBA)
R=/data/local/tmp/t96/px.raw
screencap "$R"
W=$(od -An -tu4 -N4 "$R" | tr -d ' ')
while [ $# -ge 2 ]; do
    off=$((16 + ($2 * W + $1) * 4))
    echo "$1,$2 $(od -An -tu1 -j $off -N3 "$R" | tr -s ' ' | sed 's/^ //')"
    shift 2
done
