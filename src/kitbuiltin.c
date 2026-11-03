/* kitbuiltin.c - made by tools/gen_builtin.py from the .ASM files in src/builtin - do not edit by hand */
#include <stddef.h>
#include "kitbuiltin.h"

/* EX1_SEG7_COUNT.ASM */
static const char text0[] =
    "; Seven-segment display: counts 0, 1, 2 ... 9 again and again (about 1 s per digit)\n"
    ".MODEL SMALL\n"
    ".CODE\n"
    "     ORG 1000H            ; the kit starts the program at 0000:1000\n"
    "     MOV AL, 80H\n"
    "     OUT 1FH, AL          ; 8255 (CS2): ports A, B, C = output\n"
    "L1:  XOR SI, SI           ; SI = digit 0\n"
    "L2:  MOV AL, CS:DIGIT[SI] ; segment code of this digit\n"
    "     OUT 19H, AL          ; port A -> 7-segment (0 = segment on)\n"
    "     CALL DELAY\n"
    "     INC SI\n"
    "     CMP SI, 10\n"
    "     JB  L2               ; next digit\n"
    "     JMP L1               ; start again from 0\n"
    "\n"
    "DELAY:\n"
    "     MOV DX, 5            ; about 1 second\n"
    "D1:  MOV CX, 0FFFFH\n"
    "D2:  LOOP D2\n"
    "     DEC DX\n"
    "     JNZ D1\n"
    "     RET\n"
    "\n"
    "DIGIT DB 0C0H, 0F9H, 0A4H, 0B0H, 99H   ; 0-4 (bits: dp g f e d c b a)\n"
    "      DB 92H, 82H, 0F8H, 80H, 90H      ; 5-9\n"
    "END\n";

/* EX2_DOT_3COLOUR.ASM */
static const char text1[] =
    "; Dot matrix: all 64 LEDs on, the rows in three colours\n"
    "; top to bottom: red, green, yellow, red, green, yellow, red, green\n"
    "; (yellow = the red and the green LED of a dot on together)\n"
    ".MODEL SMALL\n"
    ".CODE\n"
    "     ORG 1000H            ; the kit starts the program at 0000:1000\n"
    "     MOV AL, 80H\n"
    "     OUT 1EH, AL          ; 8255 (CS1): ports A, B, C = output\n"
    "     MOV AL, 01001001B    ; port A = red rows (bit 7 = top row, 0 = LED on)\n"
    "     OUT 18H, AL          ;   red on in rows 1, 3, 4, 6, 7\n"
    "     MOV AL, 10010010B    ; port B = green rows (0 = LED on)\n"
    "     OUT 1AH, AL          ;   green on in rows 2, 3, 5, 6, 8\n"
    "FRAME:\n"
    "     MOV AL, 1            ; column 1 (left)\n"
    "COL: OUT 1CH, AL          ; port C = switch on this column only\n"
    "     MOV CX, 300          ; short delay for each column\n"
    "T1:  LOOP T1\n"
    "     SHL AL, 1            ; next column\n"
    "     JNZ COL\n"
    "     JMP FRAME            ; scan the 8 columns again (fast: all look on)\n"
    "END\n";

/* EX3_DOT_A.ASM */
static const char text2[] =
    "; Dot matrix: letter A in green (the 8 columns are lit one after another, fast)\n"
    ".MODEL SMALL\n"
    ".CODE\n"
    "     ORG 1000H            ; the kit starts the program at 0000:1000\n"
    "     MOV AL, 80H\n"
    "     OUT 1EH, AL          ; 8255 (CS1): ports A, B, C = output\n"
    "     MOV AL, 0FFH\n"
    "     OUT 18H, AL          ; port A (red) off\n"
    "FRAME:\n"
    "     XOR SI, SI           ; start from column 1\n"
    "     MOV AH, 1            ; AH = column select\n"
    "COL: MOV AL, CS:FONT[SI]\n"
    "     OUT 1AH, AL          ; port B = green rows of this column (0 = LED on)\n"
    "     MOV AL, AH\n"
    "     OUT 1CH, AL          ; port C = select this column\n"
    "     MOV CX, 300          ; short delay for each column\n"
    "T1:  LOOP T1\n"
    "     INC SI\n"
    "     SHL AH, 1            ; next column\n"
    "     JNZ COL\n"
    "     JMP FRAME            ; scan all 8 columns again\n"
    "FONT DB 0FFH, 0C0H, 0B7H, 77H, 77H, 0B7H, 0C0H, 0FFH   ; letter A, 1 byte = 1 column\n"
    "END\n";

/* EX4_DOT_A_SCROLL.ASM */
static const char text3[] =
    "; Dot matrix: letter A scrolls to the right (it stops for a moment in the middle)\n"
    ".MODEL SMALL\n"
    ".CODE\n"
    "     ORG 1000H            ; the kit starts the program at 0000:1000\n"
    "     MOV AL, 80H\n"
    "     OUT 1EH, AL          ; 8255 (CS1): ports A, B, C = output\n"
    "     MOV AL, 0FFH\n"
    "     OUT 18H, AL          ; port A (red) off\n"
    "     XOR BX, BX           ; BX = scroll position (0-15)\n"
    "NEXT:\n"
    "     MOV DL, 40           ; show each position 40 times\n"
    "     OR  BX, BX           ; is A in the middle?\n"
    "     JNZ FRAME\n"
    "     MOV DL, 200          ; yes: hold it there longer\n"
    "FRAME:\n"
    "     MOV SI, BX\n"
    "     MOV AH, 1            ; AH = column select (column 1)\n"
    "COL: MOV AL, CS:FONT[SI]\n"
    "     OUT 1AH, AL          ; port B = row data (0 = LED on)\n"
    "     MOV AL, AH\n"
    "     OUT 1CH, AL          ; port C = select this column\n"
    "     CALL TIMER\n"
    "     INC SI\n"
    "     AND SI, 15           ; wrap around after 16 columns\n"
    "     SHL AH, 1            ; next column\n"
    "     JNZ COL\n"
    "     DEC DL\n"
    "     JNZ FRAME\n"
    "     DEC BX               ; move the letter one column right\n"
    "     AND BX, 15\n"
    "     JMP NEXT\n"
    "TIMER:\n"
    "     MOV CX, 300          ; short delay for each column\n"
    "T1:  LOOP T1\n"
    "     RET\n"
    "FONT DB 11111111B         ; letter A, 1 byte = 1 column\n"
    "     DB 11000000B         ; bit 7 = top row, 0 = LED on\n"
    "     DB 10110111B\n"
    "     DB 01110111B\n"
    "     DB 01110111B\n"
    "     DB 10110111B\n"
    "     DB 11000000B\n"
    "     DB 11111111B\n"
    "     DB 8 DUP (0FFH)      ; 8 blank columns\n"
    "END\n";

/* TEMPLATE1.ASM */
static const char text4[] =
    "; Template 1 - short form: code and tables in .CODE (a table is read with CS:)\n"
    ".MODEL SMALL\n"
    ".CODE\n"
    "     ORG 1000H            ; the kit starts the program at 0000:1000\n"
    "     MOV AL, 80H\n"
    "     OUT 1EH, AL          ; 8255 setup: 1EH = dot matrix, 1FH = 7-segment\n"
    "\n"
    "     ; ---- write your program here ----\n"
    "     XOR SI, SI\n"
    "     MOV AL, CS:TABLE[SI] ; reading a table: CS: in front of its name\n"
    "\n"
    "HERE: JMP HERE            ; a kit program never ends: it loops forever\n"
    "\n"
    "; ---- tables here, after the code ----\n"
    "TABLE DB 0FFH, 0FFH\n"
    "END\n";

/* TEMPLATE2.ASM */
static const char text5[] =
    "; Template 2 - book style: tables in .DATA, DS set at the start (no CS: needed)\n"
    ".MODEL SMALL\n"
    ".DATA\n"
    "; ---- tables here ----\n"
    "TABLE DB 0FFH, 0FFH\n"
    ".CODE\n"
    "     ORG 1000H            ; the kit starts the program at 0000:1000\n"
    "MAIN PROC\n"
    "     MOV AX, @DATA\n"
    "     MOV DS, AX           ; DS -> .DATA\n"
    "     MOV AL, 80H\n"
    "     OUT 1EH, AL          ; 8255 setup: 1EH = dot matrix, 1FH = 7-segment\n"
    "\n"
    "     ; ---- write your program here ----\n"
    "     XOR SI, SI\n"
    "     MOV AL, TABLE[SI]    ; tables in .DATA are read without CS:\n"
    "\n"
    "HERE: JMP HERE            ; a kit program never ends: it loops forever\n"
    "MAIN ENDP\n"
    "END\n";

const KitBuiltin kit_examples[] = {
    { "1. Seven-segment: count 0-9", "SEG7_COUNT.ASM", text0 },
    { "2. Dot matrix: three-colour rows (all 8x8)", "DOT_3COLOUR.ASM", text1 },
    { "3. Dot matrix: letter A", "DOT_A.ASM", text2 },
    { "4. Dot matrix: letter A scrolling", "DOT_A_SCROLL.ASM", text3 },
};
const int kit_nexamples = 4;

const KitBuiltin kit_templates[] = {
    { "Template 1 - Short form (.CODE only, tables with CS:)", NULL, text4 },
    { "Template 2 - Book style (.DATA, MOV DS,AX)", NULL, text5 },
};
const int kit_ntemplates = 2;
