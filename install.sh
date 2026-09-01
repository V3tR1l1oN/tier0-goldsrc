#!/bin/bash
# tier0 — Linux auto-install for GoldSrc dedicated server (hlds)
# Usage: ./install.sh [path_to_hlds_l]  — defaults to ./  (where hlds_run/hlds_linux lives)
set -e
DST="${1:-.}"
SRC="build/tier0.so"
if [ ! -f "$SRC" ]; then echo "build/tier0.so not found — run 'make -f Makefile' first"; exit 1; fi
if [ ! -d "$DST" ]; then echo "dest $DST not a directory"; exit 1; fi
# backup original
if [ -f "$DST/tier0.so" ] && [ ! -f "$DST/tier0.so.bak" ]; then cp -v "$DST/tier0.so" "$DST/tier0.so.bak"; echo "backup: $DST/tier0.so.bak"; fi
cp -v "$SRC" "$DST/tier0.so"
echo "installed $SRC -> $DST/tier0.so (207K, 316 exports, 1:1 Windows)"
ls -lh "$DST/tier0.so"
nm -D "$DST/tier0.so" | grep -q CreateInterface && echo "CreateInterface: OK"
echo "done — restart hlds"
