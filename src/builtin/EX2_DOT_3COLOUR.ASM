; Dot matrix: all 64 LEDs on, the rows in three colours
; top to bottom: red, green, yellow, red, green, yellow, red, green
; (yellow = the red and the green LED of a dot on together)
.MODEL SMALL
.CODE
     ORG 1000H            ; the kit starts the program at 0000:1000
     MOV AL, 80H
     OUT 1EH, AL          ; 8255 (CS1): ports A, B, C = output
     MOV AL, 01001001B    ; port A = red rows (bit 7 = top row, 0 = LED on)
     OUT 18H, AL          ;   red on in rows 1, 3, 4, 6, 7
     MOV AL, 10010010B    ; port B = green rows (0 = LED on)
     OUT 1AH, AL          ;   green on in rows 2, 3, 5, 6, 8
FRAME:
     MOV AL, 1            ; column 1 (left)
COL: OUT 1CH, AL          ; port C = switch on this column only
     MOV CX, 300          ; short delay for each column
T1:  LOOP T1
     SHL AL, 1            ; next column
     JNZ COL
     JMP FRAME            ; scan the 8 columns again (fast: all look on)
END
