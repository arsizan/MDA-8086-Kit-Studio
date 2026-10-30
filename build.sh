#!/bin/sh
# Build KitStudio.exe (32-bit, runs on Windows XP SP3 ... Windows 11) with MinGW-w64.
# Linux:   apt install gcc-mingw-w64-i686-win32 binutils-mingw-w64-i686 ; sh build.sh
# Windows: use MSYS2 "mingw32" shell and set CC=gcc WINDRES=windres.
set -e
cd "$(dirname "$0")"
CC=${CC:-i686-w64-mingw32-gcc-win32}
WINDRES=${WINDRES:-i686-w64-mingw32-windres}
DEFS="-D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -D_WIN32_IE=0x0600"
mkdir -p build/obj
for f in $(cat jwasm/SOURCES.txt); do
  $CC -O2 -w -D__NT__ -DNDEBUG $DEFS -Dexit=jw_exit -Ijwasm/H -c jwasm/$f.c -o build/obj/jw_$f.o
done
for f in kitasm kitcore kitport_win kitsim kithint main; do
  $CC -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type $DEFS -c src/$f.c -o build/obj/$f.o
done
$WINDRES -i res/app.rc -o build/obj/app_res.o
$CC -O2 -s -mwindows -static-libgcc -o build/KitStudio.exe build/obj/*.o \
    -lcomctl32 -lcomdlg32 -lshell32 -lgdi32 -luser32 -ladvapi32 -lkernel32
ls -la build/KitStudio.exe
