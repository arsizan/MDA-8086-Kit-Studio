/* kithint.c - plain-language hints for the assembler's most common error messages.
 * Each hint looks at the error code and at the program line it points to. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "kithint.h"

#define MAXOPS 3

typedef struct {
    char mnem[16];
    char op[MAXOPS][64];
    int nops;
} Insn;

/* copy line 'num' (1-based) of 'src', upper case, without comment */
static void get_line(const char *src, int num, char *out, int size)
{
    const char *p = src;
    int n = 1, k = 0, quote = 0;
    out[0] = 0;
    if (!src || num < 1) return;
    while (*p && n < num) { if (*p == '\n') n++; p++; }
    if (n != num) return;
    for (; *p && *p != '\n' && *p != '\r' && k < size - 1; p++) {
        if (*p == '\'' || *p == '"') quote = !quote;
        if (*p == ';' && !quote) break;
        out[k++] = (char)toupper((unsigned char)*p);
    }
    out[k] = 0;
}

static char *skip_ws(char *p) { while (*p == ' ' || *p == '\t') p++; return p; }

static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
}

/* split "LABEL: MNEM a, b" into mnemonic and operands */
static void parse(const char *line, Insn *in)
{
    char buf[256], *p, *q;
    int i;
    memset(in, 0, sizeof *in);
    snprintf(buf, sizeof buf, "%s", line);
    p = skip_ws(buf);
    q = p;
    while (*q && (isalnum((unsigned char)*q) || *q == '_' || *q == '@' || *q == '$' || *q == '?')) q++;
    if (*q == ':') p = skip_ws(q + 1);                  /* label */
    for (i = 0; *p && *p != ' ' && *p != '\t' && i < (int)sizeof in->mnem - 1; p++)
        in->mnem[i++] = *p;
    in->mnem[i] = 0;
    p = skip_ws(p);
    while (*p && in->nops < MAXOPS) {
        int k = 0, depth = 0;
        while (*p && (*p != ',' || depth) && k < 63) {
            if (*p == '[' || *p == '(') depth++;
            if (*p == ']' || *p == ')') depth--;
            in->op[in->nops][k++] = *p++;
        }
        in->op[in->nops][k] = 0;
        trim(in->op[in->nops]);
        in->nops++;
        if (*p == ',') p = skip_ws(p + 1);
    }
}

/* value of a plain number like 12, 0FFH, 1010B; -1 if not a number */
static long number(const char *s)
{
    char buf[64];
    int n;
    long v = 0;
    const char *p;
    snprintf(buf, sizeof buf, "%s", s);
    trim(buf);
    n = (int)strlen(buf);
    if (!n || !isdigit((unsigned char)buf[0])) return -1;
    if (buf[n - 1] == 'H') {
        for (p = buf; p < buf + n - 1; p++) {
            if (!isxdigit((unsigned char)*p)) return -1;
            v = v * 16 + (isdigit((unsigned char)*p) ? *p - '0' : *p - 'A' + 10);
        }
        return v;
    }
    if (buf[n - 1] == 'B') {
        for (p = buf; p < buf + n - 1; p++) { if (*p != '0' && *p != '1') return -1; v = v * 2 + (*p - '0'); }
        return v;
    }
    if (buf[n - 1] == 'D') n--;
    for (p = buf; p < buf + n; p++) { if (!isdigit((unsigned char)*p)) return -1; v = v * 10 + (*p - '0'); }
    return v;
}

static int is_seg(const char *s) { return !strcmp(s, "CS") || !strcmp(s, "DS") || !strcmp(s, "ES") || !strcmp(s, "SS"); }
static int is_acc(const char *s) { return !strcmp(s, "AL") || !strcmp(s, "AX"); }
static int is_mem(const char *s) { return strchr(s, '[') != NULL || !strncmp(s, "BYTE PTR", 8) || !strncmp(s, "WORD PTR", 8); }
static int in_list(const char *s, const char *const *list)
{
    for (; *list; list++) if (!strcmp(s, *list)) return 1;
    return 0;
}

static const char *hint_for(int code, const char *msg, const Insn *in)
{
    static const char *const shifts[] = { "SHL", "SHR", "SAL", "SAR", "ROL", "ROR", "RCL", "RCR", NULL };
    static const char *const muls[] = { "MUL", "IMUL", "DIV", "IDIV", NULL };
    static const char *const not86[] = { "PUSHA", "POPA", "PUSHAD", "POPAD", "ENTER", "LEAVE", "BOUND",
                                         "INSB", "INSW", "OUTSB", "OUTSW", "MOVZX", "MOVSX", "PUSHFD",
                                         "POPFD", "CDQ", "CWDE", "BSWAP", "SETZ", "SETNZ", "SETE",
                                         "SETNE", "LOOPD", "JECXZ", NULL };
    switch (code) {
    case 2030:
        if (in_list(in->mnem, shifts) && in->nops == 2 && number(in->op[1]) > 1)
            return "The 8086 shifts only by 1 or by CL: MOV CL,n then SHL AL,CL (same for SHR, ROL, ...).";
        if (!strcmp(in->mnem, "PUSH") && in->nops == 1 && number(in->op[0]) >= 0)
            return "The 8086 cannot PUSH a number: MOV AX,n then PUSH AX.";
        if (!strcmp(in->mnem, "IMUL") && in->nops > 1)
            return "The 8086 IMUL takes one operand: IMUL BL (AL*BL -> AX) or IMUL BX (AX*BX -> DX:AX).";
        return "This instruction (or this form of it) does not exist on the kit's 8086.";
    case 2209:
        if (in_list(in->mnem, not86))
            return "This instruction does not exist on the 8086 (it came with the 80186/386).";
        return NULL;
    case 2049:
        if (!strcmp(in->mnem, "MOV") && in->nops == 2) {
            if (is_seg(in->op[0]) && is_seg(in->op[1]))
                return "Copy segment registers through a register: MOV AX,DS then MOV ES,AX.";
            if (is_seg(in->op[0]))
                return "A segment register cannot take a number: MOV AX,value then MOV DS,AX.";
            if (is_mem(in->op[0]) && is_mem(in->op[1]))
                return "MOV cannot copy memory to memory: go through a register (MOV AL,[SI] / MOV [DI],AL).";
        }
        if (!strcmp(in->mnem, "OUT") && in->nops == 2) {
            if (number(in->op[0]) > 0xFF)
                return "Ports above 0FFH need DX: MOV DX,port then OUT DX,AL.";
            if (!is_acc(in->op[1]))
                return "OUT sends only AL (or AX): MOV AL,value then OUT port,AL.";
        }
        if (!strcmp(in->mnem, "IN") && in->nops == 2) {
            if (number(in->op[1]) > 0xFF)
                return "Ports above 0FFH need DX: MOV DX,port then IN AL,DX.";
            if (!is_acc(in->op[0]))
                return "IN reads only into AL (or AX): IN AL,port then MOV BL,AL.";
        }
        if (in_list(in->mnem, muls) && in->nops == 1 && number(in->op[0]) >= 0)
            return "MUL/DIV need a register or memory, not a number: MOV BL,n then MUL BL.";
        return NULL;
    case 2048:
        return "Both operands must be the same size: AL..DH are 8-bit, AX..DX/SI/DI/BP/SP are 16-bit, "
               "an 8-bit number must be 0..255 (FFH) and a 16-bit number 0..65535 (FFFFH).";
    case 2033:
        return "Inside [ ] only BX, BP, SI and DI can be used, e.g. MOV AL,[SI] or MOV AL,[BX+SI].";
    case 2182:
    case 4073:
        return "Say the size of the memory operand: BYTE PTR or WORD PTR, e.g. INC BYTE PTR [SI].";
    case 2082:
        return "Instructions must be inside a code segment: .MODEL SMALL with .CODE, "
               "or CODE SEGMENT ... CODE ENDS.";
    case 2099:
        return "The last line of the program must be END.";
    case 2102:
        return "This name is not defined anywhere: check the spelling; a label is defined once, "
               "with a colon (L1:), and a table with DB.";
    case 2056:
        return "The same name is defined twice: give every label and table its own name.";
    case 2053:
        return "LOOP and conditional jumps reach only about 127 bytes on the 8086: make the loop "
               "shorter (e.g. move a long part into a CALL subroutine).";
    }
    (void)msg;
    return NULL;
}

char *kit_hints(const char *messages, const char *source)
{
    const char *p = messages;
    char *out = NULL;
    size_t len = 0;
    const char *seen[32];
    int nseen = 0;
    if (!messages) return NULL;
    while (*p) {
        const char *e = strchr(p, '\n'), *paren, *acode;
        char line[512], src[256], msg[400];
        int n = e ? (int)(e - p) : (int)strlen(p), lineno = 0, code = 0, i, dup = 0;
        const char *h;
        Insn in;
        if (n > (int)sizeof line - 1) n = (int)sizeof line - 1;
        memcpy(line, p, n);
        line[n] = 0;
        p = e ? e + 1 : p + strlen(p);
        acode = strstr(line, "Error A");
        if (!acode) acode = strstr(line, "Warning A");
        if (!acode) continue;
        acode = strchr(acode, 'A');
        code = atoi(acode + 1);
        paren = strstr(line, ") :");
        if (paren) {
            const char *q = paren - 1;
            int mul = 1;
            while (q > line && isdigit((unsigned char)*q)) { lineno += (*q - '0') * mul; mul *= 10; q--; }
            if (*q != '(') lineno = 0;
        }
        get_line(source, lineno, src, sizeof src);
        parse(src, &in);
        snprintf(msg, sizeof msg, "%s", acode);
        h = hint_for(code, msg, &in);
        if (!h) continue;
        for (i = 0; i < nseen; i++) if (seen[i] == h) dup = 1;
        if (dup) continue;
        if (nseen < 32) seen[nseen++] = h;
        {
            char item[700];
            int k;
            if (lineno > 0) k = snprintf(item, sizeof item, "Hint (line %d): %s\n", lineno, h);
            else k = snprintf(item, sizeof item, "Hint: %s\n", h);
            if (k < 0) continue;
            if (k >= (int)sizeof item) k = (int)sizeof item - 1;
            {
                char *nb = (char *)realloc(out, len + k + 1);
                if (!nb) break;
                out = nb;
                memcpy(out + len, item, k);
                len += k;
                out[len] = 0;
            }
        }
    }
    return out;
}
