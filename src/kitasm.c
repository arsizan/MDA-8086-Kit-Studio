/* kitasm.c - assemble MDA-8086 programs with the built-in JWasm and make .ABS files.
 *
 * The output was checked byte-for-byte against the lab's own tool chain
 * (TASM 1.0 + LINK 3.60 + MEXE2ABS) on the lab's sample programs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "kitasm.h"

/* ---- JWasm entry points (compiled with -Dexit=jw_exit) ---- */
extern char *ParseCmdline(const char **cmdline, int *numargs);
extern void  CmdlineFini(void);
extern int   AssembleModule(const char *name);

static jmp_buf jw_env;
static int     jw_env_valid = 0;

/* JWasm calls exit() only for problems outside of a module (out of memory, bad
 * command line). Never let that close the whole program. */
void jw_exit(int code)
{
    if (jw_env_valid)
        longjmp(jw_env, 1);
    exit(code);
}

static void join_path(char *out, int size, const char *dir, const char *name)
{
    int n;
    snprintf(out, size, "%s", dir);
    n = (int)strlen(out);
    if (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/' && n < size - 1) {
#ifdef _WIN32
        out[n++] = '\\';
#else
        out[n++] = '/';
#endif
        out[n] = 0;
    }
    snprintf(out + n, size - n, "%s", name);
}

static unsigned char *read_file(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;
    *size = 0;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    buf = (unsigned char *)malloc(n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (n && fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    buf[n] = 0;
    fclose(f);
    *size = n;
    return buf;
}

int kit_exe_to_code(const unsigned char *d, long n, unsigned char **code, int *len,
                    char *why, int whysize)
{
    long last, pages, hpar, size, start, org, i;
    *code = NULL;
    *len = 0;
    if (n < 32 || d[0] != 'M' || d[1] != 'Z') {
        snprintf(why, whysize, "The assembler did not make a valid program file.");
        return -1;
    }
    last  = d[2] | (d[3] << 8);
    pages = d[4] | (d[5] << 8);
    hpar  = d[8] | (d[9] << 8);
    size = pages * 512;
    if (last) size = size - 512 + last;
    if (size > n) size = n;
    start = hpar * 16;
    org = start + KIT_ORG;
    for (i = start; i < org && i < size; i++) {
        if (d[i] != 0) {
            snprintf(why, whysize, "The program does not start at 1000H (code or data was placed "
                     "below 1000H). Kit programs start at 0000:1000: put ORG 1000H right after .CODE "
                     "(or after CODE SEGMENT / ASSUME), before the first instruction. "
                     "A DATA SEGMENT ... ENDS block must come after the code.");
            return -1;
        }
    }
    if (size <= org) {
        snprintf(why, whysize, "No code at 1000H. Put ORG 1000H before the first instruction.");
        return -1;
    }
    if (size - org > KIT_MAXCODE) {
        snprintf(why, whysize, "Program is too big for the kit's memory.");
        return -1;
    }
    *len = (int)(size - org);
    *code = (unsigned char *)malloc(*len);
    if (!*code) { snprintf(why, whysize, "Out of memory."); return -1; }
    memcpy(*code, d + org, *len);
    return 0;
}

char *kit_make_abs(const unsigned char *code, int len, unsigned base)
{
    int off, i, n, sum;
    unsigned addr;
    char *out = (char *)malloc((len / 32 + 2) * 80 + 32), *p;
    if (!out) return NULL;
    p = out;
    for (off = 0; off < len; off += 32) {
        n = len - off < 32 ? len - off : 32;
        addr = (base + off) & 0xFFFF;
        sum = n + (addr >> 8) + (addr & 0xFF);
        p += sprintf(p, ":%02X%04X00", n, addr);
        for (i = 0; i < n; i++) {
            p += sprintf(p, "%02X", code[off + i]);
            sum += code[off + i];
        }
        p += sprintf(p, "%02X\r\n", (-sum) & 0xFF);
    }
    strcpy(p, ":00000001FF\r\n");
    return out;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int kit_parse_abs(const char *text, unsigned char **code, int *len, unsigned *base,
                  char *why, int whysize)
{
    const char *p = text;
    unsigned lo = 0xFFFFFFFF, hi = 0;
    unsigned char *img = NULL;
    int pass, lineno;
    *code = NULL; *len = 0; *base = 0;
    /* pass 0: check and find the address range, pass 1: copy the bytes */
    for (pass = 0; pass < 2; pass++) {
        p = text;
        lineno = 0;
        while (*p) {
            const char *e = p;
            unsigned char rec[300];
            int n = 0, sum = 0, i, hv, lv;
            lineno++;
            while (*e && *e != '\n') e++;
            while (p < e && (*p == ' ' || *p == '\t' || *p == 26)) p++;
            if (p < e && *p == ':') {
                const char *q = p + 1;
                while (q + 1 < e && (hv = hexval(q[0])) >= 0 && (lv = hexval(q[1])) >= 0 && n < 300) {
                    rec[n++] = (unsigned char)(hv * 16 + lv);
                    q += 2;
                }
                for (i = 0; i < n; i++) sum += rec[i];
                if (n < 5 || n != rec[0] + 5 || (sum & 0xFF) != 0) {
                    snprintf(why, whysize, "Line %d of the .ABS file is damaged.", lineno);
                    free(img);
                    return -1;
                }
                if (rec[3] == 0 && rec[0] > 0) {
                    unsigned a = (rec[1] << 8) | rec[2];
                    if (pass == 0) {
                        if (a < lo) lo = a;
                        if (a + rec[0] > hi) hi = a + rec[0];
                    } else {
                        memcpy(img + (a - lo), rec + 4, rec[0]);
                    }
                } else if (rec[3] == 1) {
                    break;
                } else if (rec[3] != 0) {
                    snprintf(why, whysize, "The .ABS file uses record type %02X, which the kit "
                             "loader here does not handle.", rec[3]);
                    free(img);
                    return -1;
                }
            }
            p = *e ? e + 1 : e;
        }
        if (pass == 0) {
            if (hi <= lo) {
                snprintf(why, whysize, "The .ABS file has no program bytes.");
                return -1;
            }
            img = (unsigned char *)malloc(hi - lo);
            if (!img) { snprintf(why, whysize, "Out of memory."); return -1; }
            memset(img, 0xFF, hi - lo);
        }
    }
    *code = img;
    *len = (int)(hi - lo);
    *base = lo;
    return 0;
}

char *kit_hexdump(const unsigned char *code, int len, unsigned base)
{
    int i;
    char *out = (char *)malloc((len / 16 + 2) * 64 + 16), *p;
    if (!out) return NULL;
    p = out;
    *p = 0;
    for (i = 0; i < len; i++) {
        if (i % 16 == 0) p += sprintf(p, "%s%04X:", i ? "\n" : "", (base + i) & 0xFFFF);
        p += sprintf(p, " %02X", code[i]);
    }
    if (len) strcpy(p, "\n");
    return out;
}

/* keep the assembler's messages but drop the ones that do not matter on the kit */
static char *clean_messages(char *raw, int *errors, int *warnings)
{
    char *out, *p, *q;
    *errors = *warnings = 0;
    if (!raw) return NULL;
    out = (char *)malloc(strlen(raw) + 1);
    if (!out) return raw;
    q = out;
    for (p = raw; *p; ) {
        char *e = p;
        int keep = 1, n;
        while (*e && *e != '\n') e++;
        n = (int)(e - p);
        if (n > 0 && p[n - 1] == '\r') n--;
        {
            char line[1024];
            int m = n < 1023 ? n : 1023;
            memcpy(line, p, m);
            line[m] = 0;
            if (strstr(line, "A4204")) keep = 0;      /* "No stack defined": not needed on the kit */
            else if (strstr(line, "A4203")) keep = 0; /* "No start label": the kit always starts at 1000H */
            else if (m == 0) keep = 0;
            else if (strstr(line, "Error A") || strstr(line, "Fatal error")) (*errors)++;
            else if (strstr(line, "Warning A")) (*warnings)++;
            if (keep) {
                memcpy(q, line, m);
                q += m;
                *q++ = '\n';
            }
        }
        p = *e ? e + 1 : e;
    }
    *q = 0;
    free(raw);
    if (q == out) { free(out); return NULL; }
    return out;
}

int kit_assemble(const char *src, const char *tmpdir, KitAsmResult *res)
{
    char exe[1024], err[1024], fo[1100], fw[1100], fsrc[1100];
    const char *argv[9];
    int numargs = 0;
    volatile int rc = 0, files = 0;
    long n;
    unsigned char *img;
    char *msgs;

    memset(res, 0, sizeof(*res));
    join_path(exe, sizeof exe, tmpdir, "kitstudio_out.exe");
    join_path(err, sizeof err, tmpdir, "kitstudio_out.err");
    remove(exe);
    remove(err);
    {   /* make sure the source file exists before starting the assembler */
        FILE *f = fopen(src, "rb");
        if (!f) {
            snprintf(res->problem, sizeof res->problem, "Cannot open the source file:\n%s", src);
            return 0;
        }
        fclose(f);
    }
    snprintf(fo, sizeof fo, "-Fo\"%s\"", exe);
    snprintf(fw, sizeof fw, "-Fw\"%s\"", err);
    snprintf(fsrc, sizeof fsrc, "\"%s\"", src);
    argv[0] = "";          /* contents of the JWASM environment variable */
    argv[1] = "-mz";       /* MZ executable, like LINK makes */
    argv[2] = "-Zm";       /* MASM 5.1 compatible, as the lab's samples need */
    argv[3] = "-q";        /* quiet: no banner / summary */
    argv[4] = "-eq";       /* messages only into the .err file, not on the console (Linux terminal) */
    argv[5] = fo;
    argv[6] = fw;
    argv[7] = fsrc;
    argv[8] = NULL;

    if (setjmp(jw_env) == 0) {
        char *name;
        jw_env_valid = 1;
        while ((name = ParseCmdline(argv, &numargs)) != NULL) {
            files++;
            rc = AssembleModule(name);
        }
        CmdlineFini();
    } else {
        rc = 0;
        snprintf(res->problem, sizeof res->problem, "The assembler stopped with a fatal error.");
    }
    jw_env_valid = 0;

    msgs = (char *)read_file(err, &n);
    res->messages = clean_messages(msgs, &res->errors, &res->warnings);
    remove(err);

    if (files == 0 && !res->problem[0])
        snprintf(res->problem, sizeof res->problem, "The assembler did not get the file name.");
    if (rc != 1 || res->errors) {
        remove(exe);
        if (!res->errors && !res->problem[0])
            snprintf(res->problem, sizeof res->problem, "Assembly failed.");
        return 0;
    }
    img = read_file(exe, &n);
    remove(exe);
    if (!img) {
        snprintf(res->problem, sizeof res->problem, "The assembler did not write its output file.");
        return 0;
    }
    if (kit_exe_to_code(img, n, &res->code, &res->len, res->problem, sizeof res->problem) != 0) {
        free(img);
        return 0;
    }
    free(img);
    res->ok = 1;
    return 1;
}

void kit_asm_free(KitAsmResult *res)
{
    free(res->messages);
    free(res->code);
    memset(res, 0, sizeof(*res));
}
