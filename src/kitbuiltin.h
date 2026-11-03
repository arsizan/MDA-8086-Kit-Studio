/* kitbuiltin.h - example programs and templates built into the program (no files needed).
 * The texts come from the .ASM files in src/builtin (tools/gen_builtin.py makes kitbuiltin.c). */
#ifndef KITBUILTIN_H
#define KITBUILTIN_H

typedef struct {
    const char *menu;       /* text in the Examples / Templates menu */
    const char *name;       /* file name shown for the copy; NULL = a new untitled program */
    const char *text;       /* the program, '\n' line ends */
} KitBuiltin;

extern const KitBuiltin kit_examples[];
extern const int kit_nexamples;
extern const KitBuiltin kit_templates[];
extern const int kit_ntemplates;

#endif
