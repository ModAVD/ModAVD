#!/system/bin/sh
# Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later.
# inotifyd handler: grab.sh EVENT DIR NAME. When the game renames <uuid>.downloading to <uuid> (moved_to,
# event "y") and that uuid was captured right after the hard-kill reboot (crash dir has
# <uuid>.downloading), hard-link the finished file before the game unzips and deletes it.
# CRASH and GRAB directories come from the environment.
[ "$1" = "y" ] || exit 0
case "$3" in *.downloading) exit 0 ;; esac
[ -f "$CRASH/$3.downloading" ] || exit 0
ln "$2/$3" "$GRAB/$3" 2>/dev/null && echo "$(cut -d' ' -f1 /proc/uptime) grabbed $3" >> "$GRAB/grab.log"
