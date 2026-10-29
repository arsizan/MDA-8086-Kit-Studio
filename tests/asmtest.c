/* asmtest: assemble each file given (twice, to check repeated use in one process)
   and write NAME.ABS into outdir. Prints messages. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/kitasm.h"
int main(int argc, char **argv)
{
    int i, round, bad = 0;
    if (argc < 4) { printf("usage: asmtest tmpdir outdir file.asm...\n"); return 2; }
    for (round = 0; round < 2; round++)
    for (i = 3; i < argc; i++) {
        KitAsmResult r;
        const char *base = strrchr(argv[i], '/');
        const char *b2 = strrchr(argv[i], '\\');
        char name[256], out[512], *dot;
        if (b2 > base) base = b2;
        base = base ? base + 1 : argv[i];
        snprintf(name, sizeof name, "%s", base);
        if ((dot = strrchr(name, '.'))) *dot = 0;
        kit_assemble(argv[i], argv[1], &r);
        printf("[%d] %-10s ok=%d err=%d warn=%d len=%d %s\n", round, name, r.ok, r.errors, r.warnings, r.len, r.problem);
        if (r.messages) printf("%s", r.messages);
        if (r.ok && round == 1) {
            char *abs = kit_make_abs(r.code, r.len, KIT_ORG);
            FILE *f;
            snprintf(out, sizeof out, "%s/%s.ABS", argv[2], name);
            f = fopen(out, "wb");
            if (f) { fputs(abs, f); fclose(f); }
            free(abs);
        }
        if (!r.ok) bad++;
        kit_asm_free(&r);
    }
    return bad ? 1 : 0;
}
