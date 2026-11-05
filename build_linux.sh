#!/bin/sh
# Build the Linux program of MDA-8086 Kit Studio (GTK 3).
#
#   sh build_linux.sh native  -> build/kitstudio with this PC's gcc and GTK 3 (for development;
#                                needs: gcc, pkg-config, libgtk-3-dev)
#   sh build_linux.sh         -> build/kitstudio-x86_64 and build/kitstudio-i386 for release:
#        zig cc against glibc 2.17 (runs on Linux from 2014 on), plain x86-64 / i686 CPU code
#        (any Intel or AMD processor), GTK 3.10 API only (the compiler warns about newer calls).
#        Needs: zig (pip install ziglang), pkg-config + libgtk-3-dev (headers), the 32-bit GTK 3
#        libraries (libgtk-3-0:i386) and the 32-bit glibconfig.h from the libglib2.0-dev:i386
#        package: I386_GLIBCONFIG_DIR=<folder with glibconfig.h> sh build_linux.sh
set -e
cd "$(dirname "$0")"
MODULES="kitasm kitcore kitport_posix kitsim kithint kitui kitbuiltin kitview main_gtk"
GTKVER="-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_3_10 -DGDK_VERSION_MAX_ALLOWED=GDK_VERSION_3_10"
GTKVER="$GTKVER -DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_40 -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_40"
JWFLAGS="-O2 -w -D__UNIX__ -DNDEBUG -Dexit=jw_exit -Ijwasm/H"
WFLAGS="-O2 -Wall -Wextra -Wno-unused-parameter"
GTK_CFLAGS=$(pkg-config --cflags gtk+-3.0)

# $1 = output name, $2 = compiler, $3 = CPU flags, $4 = libraries, $5 = GTK include flags
build() {
  obj=build/obj_$1
  mkdir -p "$obj"
  for f in $(cat jwasm/SOURCES.txt); do
    $2 $3 $JWFLAGS -c jwasm/$f.c -o "$obj/jw_$f.o"
  done
  for f in $MODULES; do
    extra=""
    if [ "$f" = main_gtk ]; then extra="$GTKVER $5"; fi
    $2 $3 $WFLAGS $extra -c src/$f.c -o "$obj/$f.o"
  done
  $2 $3 -o "build/$1" "$obj"/*.o $4
  ls -la "build/$1"
}

if [ "$1" = native ]; then
  build kitstudio "${CC:-gcc}" "" "$(pkg-config --libs gtk+-3.0) -lpthread -lm" "$GTK_CFLAGS"
  exit 0
fi

ZIG=${ZIG:-"python3 -m ziglang"}
# GTK's header folders, without the system's own C library folders (zig brings glibc 2.17's)
GTK_NOSYS=$(echo "$GTK_CFLAGS" | tr ' ' '\n' |
            grep -v -e '^-I/usr/include/x86_64-linux-gnu$' -e 'dbus-1.0' -e '^-pthread$' | tr '\n' ' ')
LIBS="gtk-3 gdk-3 gdk_pixbuf-2.0 pango-1.0 gio-2.0 gobject-2.0 glib-2.0"
libpaths() { for l in $LIBS; do printf '%s ' "$1/lib$l.so.0"; done; printf '%s ' "$1/libcairo.so.2"; }

# 64-bit PCs
build kitstudio-x86_64 "$ZIG cc -target x86_64-linux-gnu.2.17" "-mcpu=x86_64 -fno-sanitize=undefined" \
      "$(libpaths /usr/lib/x86_64-linux-gnu) -lpthread -lm -Wl,--allow-shlib-undefined -s" "$GTK_NOSYS"

# 32-bit PCs
I386=${I386_GLIBCONFIG_DIR:-/usr/lib/i386-linux-gnu/glib-2.0/include}
if [ ! -f "$I386/glibconfig.h" ]; then
  echo "32-bit glibconfig.h not found in $I386 (set I386_GLIBCONFIG_DIR)"; exit 1
fi
GTK_I386=$(echo "$GTK_NOSYS" | sed "s#-I/usr/lib/x86_64-linux-gnu/glib-2.0/include#-I$I386#")
build kitstudio-i386 "$ZIG cc -target x86-linux-gnu.2.17" "-mcpu=i686 -fno-sanitize=undefined" \
      "$(libpaths /usr/lib/i386-linux-gnu) -lpthread -lm -Wl,--allow-shlib-undefined -s" "$GTK_I386"
