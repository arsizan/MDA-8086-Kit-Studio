# MDA-8086 Kit Studio - source

One 32-bit Win32 program (C, no runtime besides Windows' own msvcrt.dll).
Runs on Windows XP SP3, 7, 8, 10, 11 (32 and 64 bit).

## Parts
- `src/main.c`        - window, editor, output pane, menus, worker thread
- `src/kitasm.c`      - built-in assembler wrapper (JWasm -> MZ image -> .ABS, like MEXE2ABS)
- `src/kitcore.c`     - MDA-8086 serial monitor protocol (find kit, L download, G run, commands)
- `src/kitport_win.c` - COM ports on Windows (registry + DOS device list, 9600 8N1)
- `src/kitport_posix.c` - same API on Linux, only for tests
- `jwasm/`            - JWasm 2.21 assembler sources (Sybase Open Watcom Public License 1.0, see jwasm/License.html);
                        compiled with `-Dexit=jw_exit` so a fatal assembler error cannot close the program
- `res/`              - icon, manifest (common controls 6, DPI aware, asInvoker), version info
- `tests/`            - fake MDA-8086 kit on a pseudo-terminal, protocol/assembler/GUI tests

## Build
Linux (Ubuntu/Debian):
    sudo apt install gcc-mingw-w64-i686-win32 binutils-mingw-w64-i686
    sh build.sh                 # -> build/KitStudio.exe

Windows: install MSYS2, open the "MSYS2 MINGW32" shell, `pacman -S mingw-w64-i686-gcc`, then
    CC=gcc WINDRES=windres sh build.sh
(MSYS2's newer runtime may not run on XP; the Linux build above links to msvcrt.dll and does.)

## Tests that were run
- assembler: 13 programs (6 lab samples + K27, K27_A, K27_REV, K28, SIZAN, SIZAN_LR, JOY):
  bytes identical to the lab's TASM 1.0 + LINK 3.60 + MEXE2ABS output (tests/cmpabs.py)
- protocol under Wine (Windows serial API) against tests/fakekit.py: kit ready, dead port first,
  wait for RESET, no-wait failure, Stop while waiting (tests/run_proto_tests.py)
- GUI under Wine + Xvfb: assemble, run, examples menu, kit command with RESET wait,
  error line jump, Stop, port choice, Save As, Send .ABS (tests/gui_test.py)
- the same L/G protocol and 2 ms character gap were used on the lab's real kit (kit.ps1 log)
