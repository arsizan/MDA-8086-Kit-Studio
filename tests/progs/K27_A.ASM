CODE SEGMENT
     ASSUME CS:CODE
     ORG 1000H
START:
     MOV AL, 80H
     OUT 1EH, AL          ; 8255 (CS1): ports A, B, C = output
     MOV AL, 0FFH
     OUT 18H, AL          ; port A (red) off
FRAME:
     XOR SI, SI           ; start from column 1
     MOV AH, 1            ; AH = column select
COL: MOV AL, CS:FONT[SI]
     OUT 1AH, AL          ; port B = row data (0 = LED on)
     MOV AL, AH
     OUT 1CH, AL          ; port C = select this column
     MOV CX, 300          ; short delay for each column
T1:  LOOP T1
     INC SI
     SHL AH, 1            ; next column
     JNZ COL
     JMP FRAME            ; scan all 8 columns again
FONT DB 0FFH, 0C0H, 0B7H, 77H, 77H, 0B7H, 0C0H, 0FFH   ; letter A
CODE ENDS
     END START
