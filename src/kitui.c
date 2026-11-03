/* kitui.c - parts shared by the Windows and the Linux program (see kitui.h) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include "kitasm.h"
#include "kitcore.h"
#include "kitsim.h"
#include "kithint.h"
#include "kitui.h"

/* ------------------------------------------------------------------ texts */
const char KITUI_HELP[] =
"HOW TO USE\n\n"
"1. Write a program, or start from the Templates menu:\n"
"     Template 1 - short form: everything in .CODE, tables read with CS:\n"
"     Template 2 - book style: tables in .DATA, MOV AX,@DATA / MOV DS,AX\n"
"   The Examples menu has ready programs for the dot matrix and the\n"
"   7-segment display. File > Open opens your own .ASM files.\n"
"   Kit programs start at address 1000H: put ORG 1000H before the first\n"
"   instruction. The kit has no DOS, so INT 21H does not work there;\n"
"   programs talk to the hardware with OUT and usually loop forever.\n\n"
"2. Assemble (F7): errors are shown in red with the line number.\n"
"   Double-click an error to jump to that line.\n"
"   The machine code is shown too (useful for keypad entry and lab reports),\n"
"   and NAME.ABS is saved next to your NAME.ASM.\n"
"   Kit check: the program also runs for a few seconds on a built-in model of\n"
"   the kit. The output shows what the dot matrix / 7-segment would show and\n"
"   warns about ports that are not on the kit, a missing 8255 setup, INT 21H.\n\n"
"3. Run on Kit (F5): assembles, finds the kit, sends the program (L command)\n"
"   and starts it at 0000:1000 (G command).\n"
"   On the kit: SELECT MODE switch = PC. If the kit does not answer,\n"
"   press RESET on the kit while the program waits.\n"
"   To run another program, press RESET on the kit first.\n\n"
"4. Kit command box (bottom): type monitor commands and press Enter, e.g.\n"
"      D 0000:1000 40      (show memory)\n"
"      R                   (show registers)\n"
"      G 0000:1000         (start a program)\n\n"
"Kit > Send .ABS file sends a ready .ABS file (e.g. the lab's sample programs).";

#ifdef _WIN32
const char KITUI_CONN[] =
"CONNECTING THE KIT\n\n"
"The MDA-8086 talks to the PC through RS-232 serial: the 9-pin (2 rows) cable.\n\n"
"- PC with a COM port: plug the cable in (9 pins, 2 rows).\n"
"  Note: the VGA monitor port has 15 pins in 3 rows - it is NOT the same.\n"
"- Laptop / PC without COM port: use a USB-to-RS232 (DB9) adapter.\n"
"  FTDI or CH340 adapters work best; cheap PL2303 copies often fail on Windows 10/11.\n"
"  The adapter shows up as COMx and this program finds it automatically.\n"
"- HDMI, VGA or display adapters cannot carry data. They will not work.\n\n"
"Settings used: 9600 baud, 8 data bits, no parity, 1 stop bit.\n"
"Port 'Auto' tries every COM port and keeps the one where the kit answers.\n"
"Close PuTTY or other terminal programs - only one program can use a port.\n\n"
"Help > Kit log shows every byte sent to and received from the kit\n"
"(useful when something goes wrong with the kit).";
#else
const char KITUI_CONN[] =
"CONNECTING THE KIT\n\n"
"The MDA-8086 talks to the PC through RS-232 serial: the 9-pin (2 rows) cable.\n\n"
"- PC with a serial (COM) port: plug the cable in (9 pins, 2 rows).\n"
"  On Linux it is /dev/ttyS0, /dev/ttyS1, ...\n"
"  Note: the VGA monitor port has 15 pins in 3 rows - it is NOT the same.\n"
"- Laptop / PC without a serial port: use a USB-to-RS232 (DB9) adapter.\n"
"  It shows up as /dev/ttyUSB0 (FTDI, CH340, PL2303) or /dev/ttyACM0.\n"
"  Linux has the drivers built in; this program finds the port by itself.\n"
"- HDMI, VGA or display adapters cannot carry data. They will not work.\n\n"
"Permission: serial ports belong to the group 'dialout' (Arch, Manjaro:\n"
"'uucp'). If a port cannot be opened, run this once in a terminal:\n"
"      sudo usermod -aG dialout $USER\n"
"then log out and log in again (or restart the PC).\n\n"
"Settings used: 9600 baud, 8 data bits, no parity, 1 stop bit.\n"
"Port 'Auto' tries every serial port and keeps the one where the kit answers.\n"
"Close minicom, screen, PuTTY or other terminal programs - only one\n"
"program can use a port.\n\n"
"Help > Kit log shows every byte sent to and received from the kit\n"
"(useful when something goes wrong with the kit).";
#endif

const char KITUI_ABOUT[] =
APP_NAME " " APP_VERSION "\n\n"
"Write, assemble and run programs on the Midas MDA-8086 trainer kit.\n"
"Windows: XP, 7, 8, 10, 11 (32 and 64 bit).\n"
"Linux: 64 and 32 bit PCs (Intel or AMD) with GTK 3.\n\n"
"Built-in assembler: JWasm (MASM compatible), Sybase Open Watcom\n"
"Public License 1.0 - source included with this program.\n"
"Its output for kit programs matches the lab's TASM + LINK + MEXE2ABS.\n\n"
"The kit check after Assemble runs the program on a built-in 8086\n"
"model of the kit (dot matrix and 7-segment), without the kit.";

/* ------------------------------------------------------------------ helpers */
const char *kitui_file_part(const char *path)
{
    const char *a = strrchr(path, '\\'), *b = strrchr(path, '/');
    if (b && (!a || b > a)) a = b;
    return a ? a + 1 : path;
}

static void sayf(KitUiOut out, int kind, const char *fmt, ...)
{
    char buf[1600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    out(kind, buf);
}

/* several lines; a line that starts with '!' is a warning */
static void lines(KitUiOut out, int kind, const char *text)
{
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        int n = e ? (int)(e - p) : (int)strlen(p);
        char line[1024];
        if (n > (int)sizeof line - 1) n = (int)sizeof line - 1;
        memcpy(line, p, n);
        line[n] = 0;
        if (line[0] == '!') sayf(out, KO_WARN, "! %s", line + 1);
        else if (line[0]) out(kind, line);
        if (!e) break;
        p = e + 1;
    }
}

static int same_start(const char *a, const char *b, size_t n)   /* case does not matter */
{
    for (; n; n--, a++, b++) {
        if (!*a || tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    }
    return 1;
}

static int write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    size_t n = strlen(text);
    if (!f) return 0;
    if (fwrite(text, 1, n, f) != n) { fclose(f); return 0; }
    return fclose(f) == 0;
}

/* ------------------------------------------------------------------ assemble */
char *kitui_assemble(const char *src, const char *shown, const char *source, const char *tmpdir,
                     const char *asm_path, int quiet, KitUiOut out)
{
    KitAsmResult r;
    char *abs;
    unsigned t0 = kp_ms();

    kit_assemble(src, tmpdir, &r);
    if (r.messages) {
        /* one mistake can give the same error on many lines: show 3 of each kind, count the rest */
        char codes[48][8];
        int counts[48], ncodes = 0, hidden = 0;
        char *p = r.messages;
        size_t sl = strlen(src);
        while (*p) {
            char *e = strchr(p, '\n');
            char save = 0, code[8] = "";
            const char *a;
            int k, show = 1;
            if (e) { save = *e; *e = 0; }
            a = strstr(p, "Error A");
            if (!a) a = strstr(p, "Warning A");
            if (a) {
                a = strchr(a, 'A');
                snprintf(code, sizeof code, "%.5s", a);
                for (k = 0; k < ncodes && strcmp(codes[k], code) != 0; k++) ;
                if (k == ncodes && ncodes < 48) { strcpy(codes[ncodes], code); counts[ncodes++] = 0; }
                if (k < ncodes && ++counts[k] > 3) { show = 0; hidden++; }
            }
            if (show) {
                int kind = strstr(p, "Error") ? KO_ERR : (strstr(p, "Warning") ? KO_WARN : KO_INFO);
                if (sl && same_start(p, src, sl))     /* "/long/path/X.ASM(7) : ..." -> "X.ASM(7) : ..." */
                    sayf(out, kind, "%s%s", shown, p + sl);
                else
                    out(kind, p);
            }
            if (!e) break;
            *e = save;
            p = e + 1;
        }
        if (hidden)
            sayf(out, r.errors ? KO_ERR : KO_WARN, "(%d more line(s) with the same error are not shown)", hidden);
    }
    if (r.errors)
        sayf(out, KO_ERR, "%d error(s). Double-click an error line to jump to it, fix it and try again.", r.errors);
    if (r.messages && source) {             /* plain-language hints for the common mistakes */
        char *hints = kit_hints(r.messages, source);
        if (hints) { lines(out, KO_HINT, hints); free(hints); }
    }
    if (!r.ok) {
        if (!r.errors && r.problem[0]) out(KO_ERR, r.problem);
        kit_asm_free(&r);
        return NULL;
    }
    abs = kit_make_abs(r.code, r.len, KIT_ORG);
    if (asm_path && abs) {          /* also write NAME.ABS next to the program, like MEXE2ABS */
        char absPath[4200], *dot;
        const char *name;
        snprintf(absPath, sizeof absPath, "%s", asm_path);
        name = kitui_file_part(absPath);
        dot = strrchr(absPath, '.');
        if (dot && dot > name) *dot = 0;
        strncat(absPath, ".ABS", sizeof absPath - strlen(absPath) - 1);
        if (write_file(absPath, abs))
            sayf(out, KO_OK, "OK: %d bytes of code at 0000:1000  (%u ms). Saved %s",
                 r.len, (unsigned)(kp_ms() - t0), kitui_file_part(absPath));
        else
            sayf(out, KO_OK, "OK: %d bytes of code at 0000:1000.", r.len);
    } else {
        sayf(out, KO_OK, "OK: %d bytes of code at 0000:1000.", r.len);
    }
    if (!quiet) {
        char *dump = kit_hexdump(r.code, r.len, KIT_ORG);
        if (dump) {
            out(KO_INFO, "Machine code (address: bytes) - also for keypad entry:");
            out(KO_DUMP, dump);
            free(dump);
        }
    }
    {   /* kit check: run the program on the model of the kit (Assemble: with pictures,
           Run on Kit: only the warnings) */
        KitSimReport rep;
        char *text;
        kit_simulate(r.code, r.len, KIT_ORG, quiet ? 4.0 : 12.0, &rep);
        text = kit_sim_text(&rep, !quiet);
        if (text) { lines(out, KO_CHECK, text); free(text); }
    }
    kit_asm_free(&r);
    return abs;
}

/* ------------------------------------------------------------------ error lines */
int kitui_error_line(const char *line)
{
    const char *p = strstr(line, ") :");
    const char *h = strstr(line, "(line ");
    int num = 0;
    if (h) {
        for (h += 6; *h >= '0' && *h <= '9'; h++) num = num * 10 + (*h - '0');
        return (*h == ')' && num > 0) ? num : 0;
    }
    if (p) {
        const char *q = p - 1;
        int mul = 1;
        while (q >= line && *q >= '0' && *q <= '9') { num += (*q - '0') * mul; mul *= 10; q--; }
        if (q < line || *q != '(' || num <= 0) return 0;
        return num;
    }
    return 0;
}
