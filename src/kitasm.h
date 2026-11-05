/* kitasm.h - assemble MDA-8086 programs with the built-in JWasm and make .ABS files */
#ifndef KITASM_H
#define KITASM_H

#define KIT_ORG     0x1000      /* every kit program starts at 0000:1000 */
#define KIT_MAXCODE 0xE000      /* bytes 1000H..EFFFH (RAM ends at FFFFH) */

typedef struct {
    int            ok;          /* 1 = assembled without errors */
    int            errors;      /* number of "Error" lines */
    int            warnings;    /* number of "Warning" lines (A4204 ignored) */
    char          *messages;    /* assembler messages, '\n' separated (malloc'd, may be NULL) */
    unsigned char *code;        /* bytes for 0000:1000 onwards (malloc'd, NULL if failed) */
    int            len;         /* number of bytes in code */
    char           problem[512];/* short explanation when ok == 0 and errors == 0 */
    int           *lines;       /* source line of the instruction at each code byte (0 = none),
                                   len entries, from the listing (may be NULL) */
} KitAsmResult;

/* Assemble source file 'src'. 'tmpdir' must be a writable folder (with or
 * without trailing slash). Returns res->ok. Free with kit_asm_free(). */
int  kit_assemble(const char *src, const char *tmpdir, KitAsmResult *res);
void kit_asm_free(KitAsmResult *res);

/* Convert an MZ EXE image (as written by JWasm -mz or LINK) to the bytes at 1000H.
 * Returns 0 on success, otherwise writes a reason into 'why'. */
int  kit_exe_to_code(const unsigned char *exe, long n, unsigned char **code, int *len,
                     char *why, int whysize);

/* Make .ABS text (Intel HEX, 32 bytes per record, CRLF, end record) - same layout as
 * the lab's MEXE2ABS.EXE. Returns malloc'd string. */
char *kit_make_abs(const unsigned char *code, int len, unsigned base);

/* Parse .ABS / Intel HEX text into memory image starting at 'base' (type 00 records
 * only, checksums verified). Returns 0 on success. *code malloc'd. */
int  kit_parse_abs(const char *text, unsigned char **code, int *len, unsigned *base,
                   char *why, int whysize);

/* Human readable machine-code dump ("1000: B0 80 E6 ...", 16 per line). malloc'd. */
char *kit_hexdump(const unsigned char *code, int len, unsigned base);

#endif
