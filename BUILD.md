# MDA-8086 Kit Studio 1.1 - source

One 32-bit Win32 program (C, no runtime besides Windows' own msvcrt.dll).
Runs on Windows XP SP3, 7, 8, 10, 11 (32 and 64 bit).

## Parts
- `src/main.c`        - window, editor, output pane, menus, worker thread; Examples menu = the
                        .ASM files in the `examples` folder next to the exe (read when the menu opens)
- `src/kitasm.c`      - built-in assembler wrapper (JWasm -> MZ image -> .ABS, like MEXE2ABS)
- `src/kithint.c`     - plain-language hints for common assembler errors (looks at the error line)
- `src/kitsim.c`      - kit check: 8086 interpreter with 8086 clock counts + model of the kit's
                        8255s (dot matrix 18H/1AH/1CH/1EH, 7-segment 19H/1FH); describes what the
                        displays would show and warns about unknown ports, missing 8255 setup,
                        INT 21H, 80186+ instructions, running off the end of the program
- `src/kitcore.c`     - MDA-8086 serial monitor protocol (find kit, L download, G run, commands)
- `src/kitport_win.c` - COM ports on Windows (registry + DOS device list, 9600 8N1)
- `src/kitport_posix.c` - same API on Linux, only for tests
- `jwasm/`            - JWasm 2.21 assembler sources (Sybase Open Watcom Public License 1.0, see jwasm/License.html);
                        compiled with `-Dexit=jw_exit` so a fatal assembler error cannot close the program
- `res/`              - icon, manifest (common controls 6, DPI aware, asInvoker), version info
- `tests/`            - fake MDA-8086 kit on a pseudo-terminal, protocol/assembler/GUI tests,
                        `tests/sim/` kit-check tests (CPU compared with Unicorn, fuzzing)

## Build
Linux (Ubuntu/Debian):
    sudo apt install gcc-mingw-w64-i686-win32 binutils-mingw-w64-i686
    sh build.sh                 # -> build/KitStudio.exe

Windows: install MSYS2, open the "MSYS2 MINGW32" shell, `pacman -S mingw-w64-i686-gcc`, then
    CC=gcc WINDRES=windres sh build.sh
(MSYS2's newer runtime may not run on XP; the Linux build above links to msvcrt.dll and does.)

## Tests that were run (1.1)
- assembler: 13 programs (6 lab samples + K27, K27_A, K27_REV, K28, SIZAN, SIZAN_LR, JOY):
  bytes identical to the lab's TASM 1.0 + LINK 3.60 + MEXE2ABS output (tests/cmpabs.py)
- templates: .MODEL SMALL forms (with/without .STACK/.DATA, data above or below the code,
  CS: tables) assembled with the app and with the lab's TASM 1.0 + LINK 3.60 in DOSBox:
  identical bytes; code always at 1000H
- kit check CPU: over 30,000 random single 8086 instructions compared with Unicorn (QEMU):
  no differences (flags the 8086 leaves undefined are masked; 8086-only behaviour such as
  PUSHF bits 12-15 and PUSH SP is kept); 25 whole programs x 300,000 instructions: same
  registers and memory (the 3 that end with INT 3 / INT 21H stop at the INT by design)
  (tests/sim/difftest.py, progtest.py)
- kit check robustness: 9,000 random byte programs and 320 random looping display programs
  under AddressSanitizer/UBSan: no faults; slowest case 18 million instructions in ~0.1 s
- GUI under Wine + Xvfb with the fake kit: empty editor at start, Examples menu from the folder,
  File > New empty, assemble with kit check, run, RESET wait, kit command, error hint, Stop
  (tests/gui_test.py)
- the same L/G protocol and 2 ms character gap were used on the lab's real kit (kit.ps1 log)
