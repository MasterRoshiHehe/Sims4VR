#!/usr/bin/env bash
# Usage: logtail.sh LOG [-n N] [-t tag1,tag2] [-g regex]
# Prints the session header (tag [session] lines + first game_addresses line), then the last N lines (default 80) of the chosen tags.
# Logs: D:\Modding\Sims 4\mods\Sims 4 VR Claude logs (sims4vr.log, sims4vr_python.log). Python lines are tagged [py].
log=$1; shift; n=80; tags=; re=
while getopts n:t:g: o; do case $o in n) n=$OPTARG;; t) tags=$OPTARG;; g) re=$OPTARG;; esac; done
[ -f "$log" ] || { echo "no such log: $log" >&2; exit 1; }
echo "== header"
grep -m8 -E '^\[[0-9:.]+\] \[session\]' "$log"
grep -m1 -E '\[game_addresses\]' "$log"
echo "== last $n${tags:+ ($tags)}"
pat=
[ -n "$tags" ] && pat="^\\[[0-9:.]+\\] \\[($(echo "$tags" | tr ',' '|'))\\]"
{ if [ -n "$pat" ]; then grep -E "$pat" "$log"; else cat "$log"; fi; } | { if [ -n "$re" ]; then grep -E "$re"; else cat; fi; } | tail -n "$n"
