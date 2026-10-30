CODE SEGMENT
     ASSUME CS:CODE
     ORG 1000H
START:
     MOV AL, 80H
     OUT 1EH, AL          ; 8255 (CS1): ports A, B, C = output
     MOV AL, 0FFH
     OUT 18H, AL          ; port A (red) off
     XOR BX, BX           ; BX = scroll position (0-15)
NEXT:
     MOV DL, 40           ; show each position 40 times
     OR  BX, BX           ; is A in the middle?
     JNZ FRAME
     MOV DL, 200          ; yes: hold it there longer
FRAME:
     MOV SI, BX
     MOV AH, 1            ; AH = column select (column 1)
COL: MOV AL, CS:FONT[SI]
     OUT 1AH, AL          ; port B = row data (0 = LED on)
     MOV AL, AH
     OUT 1CH, AL          ; port C = select this column
     CALL TIMER
     INC SI
     AND SI, 15           ; wrap around after 16 columns
     SHL AH, 1            ; next column
     JNZ COL
     DEC DL
     JNZ FRAME
     DEC BX               ; move the letter one column right
     AND BX, 15
     JMP NEXT
TIMER:
     MOV CX, 300          ; short delay for each column
T1:  LOOP T1
     RET
FONT DB 11111111B         ; letter A, 1 byte = 1 column
     DB 11000000B         ; bit 7 = top row, 0 = LED on
     DB 10110111B
     DB 01110111B
     DB 01110111B
     DB 10110111B
     DB 11000000B
     DB 11111111B
     DB 8 DUP (0FFH)      ; 8 blank columns
CODE ENDS
     END START
