/* kitui.h - what the Windows window (main.c) and the Linux window (main_gtk.c) share:
 * name and version, help texts, Assemble with its report, finding a line number. */
#ifndef KITUI_H
#define KITUI_H

#define APP_NAME    "MDA-8086 Kit Studio"
#define APP_VERSION "2.0"

/* kinds of output lines: KO_INFO, KO_OK, KO_WARN, KO_ERR, KO_KIT (kitcore.h) and */
#define KO_DUMP  9      /* machine code dump */
#define KO_HINT  10     /* plain-language hint for an assembler error */
#define KO_CHECK 11     /* kit check (model of the kit) */

/* show one message in the output pane (it may have several lines) */
typedef void (*KitUiOut)(int kind, const char *text);

extern const char KITUI_HELP[];    /* Help > How to use */
extern const char KITUI_CONN[];    /* Help > Connecting the kit */
extern const char KITUI_ABOUT[];   /* Help > About */

/* Assemble the saved file 'src' and report like Assemble (F7): assembler messages (at most
 * 3 lines of each error), hints, "OK: n bytes", machine code (not when quiet) and the kit
 * check. shown = file name used in the messages, source = program text (for the hints),
 * asm_path = the program's own file (NAME.ABS is saved next to it) or NULL.
 * Returns the .ABS text (malloc'd) or NULL if the program has errors. */
char *kitui_assemble(const char *src, const char *shown, const char *source, const char *tmpdir,
                     const char *asm_path, int quiet, KitUiOut out);

/* line number in "X.ASM(12) : Error ..." or "Hint (line 12): ...", 0 if none */
int kitui_error_line(const char *line);

/* file name without folders ('/' or '\') */
const char *kitui_file_part(const char *path);

#endif
