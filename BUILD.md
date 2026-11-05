# MDA-8086 Kit Studio 3.0 - source

Windows: one 32-bit Win32 program (C, no runtime besides Windows' own msvcrt.dll);
runs on Windows XP SP3, 7, 8, 10, 11 (32 and 64 bit).
Linux: one GTK 3 program for 64-bit (x86_64) and 32-bit (i386) PCs, Intel or AMD; built against
glibc 2.17 and the GTK 3.10 API, so it runs on Linux from about 2014 on.

## Parts
- `src/main.c`        - Windows window: editor, output pane, menus, worker thread, Simulator panel
                        (right side, or its own window: Simulator > Separate window; drawn with
                        GDI+ loaded at run time, text with GDI)
- `src/main_gtk.c`    - Linux window (GTK 3): the same menus, buttons and behaviour (editor with
                        undo/redo, output pane, double-click to the error line, file drop),
                        Simulator panel drawn with cairo, next line marked yellow
- `src/kitview.c`     - the Simulator panel's picture, shared by both windows: status, dot matrix,
                        LEDs + 7-segment, LCD 16x2, registers + flags, 8255 ports; one layout
                        (580 x 640, scaled to the panel) drawn through 4 callbacks (rect, circle,
                        polygon, text); light colours, every LED with an outline
- `src/kitui.c`       - shared by both windows: Assemble and its report (messages, hints, machine
                        code, kit check), help texts, line numbers in messages, the Simulator's
                        control (loads the editor's program again only when the text changed,
                        runs it in real time at the chosen speed, steps, finds the source line)
- `src/kitbuiltin.c`  - built-in examples and templates (Examples / Templates menus, no folder);
                        made by `tools/gen_builtin.py` from the .ASM files in `src/builtin`
                        (the script also makes `src/icon_png.h`, the Linux window icon)
- `src/kitasm.c`      - built-in assembler wrapper (JWasm -> MZ image -> .ABS, like MEXE2ABS);
                        reads JWasm's listing (-Fl) for the source line of every code byte
- `src/kithint.c`     - plain-language hints for common assembler errors (looks at the error line)
- `src/kitsim.c`      - kit check: 8086 interpreter with 8086 clock counts + model of the kit's
                        8255s (dot matrix 18H/1AH/1CH/1EH, 7-segment 19H/1FH); describes what the
                        displays would show and warns about unknown ports, missing 8255 setup,
                        INT 21H, 80186+ instructions, running off the end of the program.
                        Also the Simulator's live machine (KitMachine: run, step, reset, view):
                        LED brightness from how long each LED was on (scanned dot matrix),
                        HD44780 subset for the LCD (00H command, 04H data, 02H status)
- `src/kitcore.c`     - MDA-8086 serial monitor protocol (find kit, L download, G run, commands);
                        words such as "Execute" are searched only in the visible characters the
                        kit sent (a NUL byte cannot hide them); every byte sent/received goes to
                        the kit log (Help > Kit log): Windows `%TEMP%\KitStudio\kit_log.txt`,
                        Linux `~/.cache/kitstudio/kit_log.txt`
- `src/kitport_win.c` - COM ports on Windows (registry + DOS device list, 9600 8N1)
- `src/kitport_posix.c` - serial ports on Linux: /dev/ttyS* with a UART behind it, /dev/ttyUSB*,
                        /dev/ttyACM*; used by one program at a time, 9600 8N1 raw, DTR/RTS on,
                        a clear message when the user is not in the port's group (dialout)
- `jwasm/`            - JWasm 2.21 assembler sources (Sybase Open Watcom Public License 1.0, see jwasm/License.html);
                        compiled with `-Dexit=jw_exit` so a fatal assembler error cannot close the program
- `res/`              - icon, manifest (common controls 6, DPI aware, asInvoker), version info
- `linux/`            - `kitstudio` (starts the 64- or 32-bit program, says how to get GTK 3 if it
                        is missing) and `install.sh` (menu entry for the user, no root needed)
- `tests/`            - fake MDA-8086 kit on a pseudo-terminal, protocol/assembler/GUI tests
                        (`gui_test.py` Windows under Wine, `gui_test_linux.py` Linux),
                        `tests/sim/` kit-check tests (CPU compared with Unicorn, fuzzing)

## Build
Windows program (built on Linux):
    sudo apt install gcc-mingw-w64-i686-win32 binutils-mingw-w64-i686
    sh build.sh                 # -> build/KitStudio.exe
  (or on Windows: MSYS2 "MINGW32" shell, `pacman -S mingw-w64-i686-gcc`,
   `CC=gcc WINDRES=windres sh build.sh`; MSYS2's newer runtime may not run on XP)

Linux program:
    sh build_linux.sh native    # -> build/kitstudio with this PC's gcc + GTK 3 (libgtk-3-dev)
    sh build_linux.sh           # -> build/kitstudio-x86_64 and build/kitstudio-i386 (release)
  The release build uses zig (pip install ziglang) for glibc 2.17 and plain x86-64 / i686 code,
  the GTK 3 headers (cairo is linked too), the 32-bit GTK 3 libraries (libgtk-3-0:i386) and the 32-bit glibconfig.h
  from the libglib2.0-dev:i386 package (see the top of build_linux.sh).
  Files are saved with CR LF line ends on Linux too, as the lab's DOS/Windows tools expect.

Built-in examples/templates: change the .ASM files in src/builtin, then `python3 tools/gen_builtin.py`.

## Tests that were run (3.0)
Kept short on purpose (3.0 adds the Simulator; the kit protocol code did not change).
- Simulator machine: three-colour rows, letter A, letter A scrolling, 7-segment 0-9 and
  Template 2 show the expected pictures; the instruction -> source line map was checked on them
- assembler: the 13 programs still give bytes identical to the lab's TASM + LINK + MEXE2ABS
  (the listing option changes nothing in the code)
- Windows under Wine + Xvfb (1024 x 768): the Simulator opens in its own window by itself,
  letter A runs, F6 works from the main window, Simulator menu (Separate window checked)
- Linux under Xvfb, native and both release programs (x86_64; i386 with the 32-bit GTK 3):
  examples run in the simulator, Pause + Step mark the next line yellow, the separate window
- release programs: need at most GLIBC_2.17; libraries: GTK 3, GDK, GLib/GIO/GObject, Pango,
  gdk-pixbuf, cairo (every GTK 3 install has it) and libc; GTK API <= 3.10 (compiler checked)
- not run again: tests/gui_test.py and tests/gui_test_linux.py (their menu positions are from 2.0;
  the new Simulator menu moved Examples, Templates and Help to the right)

## Tests that were run (2.0)
- built-in examples 1, 3, 4 (7-segment 0-9, letter A, A scrolling): machine code identical to
  the programs run on the lab's kit (K28, K27_A, K27 in the kit log of 28 Oct); example 2
  (three-colour rows) and both templates assemble without errors; the kit check draws the rows
  red / green / yellow
- kit check: the "almost all LEDs are lit" warning no longer comes for a full picture in several
  colours, and still comes for one colour left on everywhere (red port not set to 0FFH, letter
  with 1 = on); everything else it prints is unchanged on 30 programs
- assembler: the 13 programs still give bytes identical to the lab's TASM + LINK + MEXE2ABS
  (JWasm now gets -eq: messages only in its .err file, nothing on a Linux terminal)
- kit protocol: the 9 scenarios (incl. NUL bytes) pass with the new Linux port code
- Windows GUI under Wine + Xvfb (tests/gui_test.py, fake kit sending NUL bytes): Templates menu,
  built-in examples, kit check, run, RESET wait, kit command, error hint, Stop, kit log
- Linux GUI under Xvfb (tests/gui_test_linux.py, fake kit sending NUL bytes), 20 checks each for
  the native build and for both release programs (x86_64; i386 with the 32-bit GTK 3): templates,
  three-colour picture, File > New, program arrives in the kit, RESET wait, kit command in
  capitals, error line + hint, Undo, Save As (CR LF) + NAME.ABS, Stop, Help > Kit log,
  "save changes?" when closing, no warnings on the console
- release programs: need at most GLIBC_2.17; need only GTK 3, GLib, Pango, gdk-pixbuf and libc;
  the 32-bit code uses no MMX/SSE (i686), the 64-bit code no AVX/SSE3+ (only the `rep bsf`
  form of tzcnt, which every x86-64 runs); GTK API <= 3.10 and GLib <= 2.40 (compiler checked)
- bug found by the Linux test and fixed in both windows: Assemble of a saved file copied the
  file name onto itself (snprintf with the same buffer), which emptied it with Linux's C library

## Tests that were run (1.1.1)
- 1.1.1 fixes "The kit did not confirm the start (no "Execute Address")", which the lab's kit
  test showed right after the kit's "Execute Address = 0000:1000" (the program did run)
- kit protocol (tests/run_proto_tests.py, fake kit on a pseudo-terminal): 9 scenarios - kit ready,
  dead port first, RESET wait, no wait, Stop, and NUL bytes from the kit (in the G answer,
  30 ms after the download prompt, after every CR LF, all together with a RESET wait):
  all pass with the Linux build and with the Windows build under Wine. The 1.1 code fails the
  4 NUL scenarios: the program arrives in the kit, then the same false warning as in the lab
  (or "Kit not found" when every line ends with a NUL)
- GUI under Wine + Xvfb (tests/gui_test.py), with the normal fake kit and with
  FAKEKIT_ARGS="--nul=g,late": all pass; the kit log shows the NUL bytes and 3 confirmed runs;
  Help > Kit log opens it in Notepad

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
