#!/bin/sh
# Cross-compile Azkar Notifier for Windows x64 (MinGW-w64). On Windows with MinGW: same flags with gcc/windres.
set -e
cd "$(dirname "$0")"
CC=${CC:-x86_64-w64-mingw32-gcc}
RC=${RC:-x86_64-w64-mingw32-windres}
[ -f app.ico ] || python3 make_icon.py
$RC -c 65001 -i app.rc -o app_res.o
$CC -std=c11 -Os -s -municode -mwindows -DUNICODE -D_UNICODE \
    -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -o AzkarNotifier.exe main.c app_res.o \
    -lgdi32 -luser32 -lshell32 -ladvapi32 -lcomctl32
ls -la AzkarNotifier.exe
