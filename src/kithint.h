/* kithint.h - plain-language hints for the assembler's most common error messages */
#ifndef KITHINT_H
#define KITHINT_H

/* messages: assembler output ("NAME(line) : Error Axxxx: text" lines)
 * source:   the program text (to look at the line with the error)
 * Returns malloc'd text, one hint per line ("Line 12: ..."), each hint only once;
 * NULL if there is nothing to add. */
char *kit_hints(const char *messages, const char *source);

#endif
