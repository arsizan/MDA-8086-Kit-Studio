CODE SEGMENT
     ASSUME CS:CODE
     ORG 1000H
START:
     MOV AL, 80H
     OUT 1EH, AL          ; 8255 (CS1): ports A, B, C = output
     MOV AL, 0FFH
     OUT 18H, AL          ; port A (red) off
     XOR BX, BX           ; BX = scroll position (0-31)
NEXT:
     MOV DL, 20           ; show each position 20 times
FRAME:
     MOV SI, BX
     MOV AH, 1            ; AH = column select (left column)
COL: MOV AL, CS:TEXT[SI]
     OUT 1AH, AL          ; port B = row data (0 = LED on)
     MOV AL, AH
     OUT 1CH, AL          ; port C = select this column
     CALL TIMER
     INC SI
     AND SI, 31           ; wrap around after 32 columns
     SHL AH, 1            ; next column to the right
     JNZ COL
     DEC DL
     JNZ FRAME
     INC BX               ; move the text one column left
     AND BX, 31
     JMP NEXT
TIMER:
     MOV CX, 300          ; short delay for each column
T1:  LOOP T1
     RET
TEXT DB 15 DUP (0FFH)                     ; gap between words
     DB 0FDH, 0FEH, 0BEH, 81H, 0BFH, 0FFH  ; J
     DB 0C1H, 0BEH, 0BEH, 0BEH, 0C1H, 0FFH ; O
     DB 9FH, 0EFH, 0F0H, 0EFH, 9FH         ; Y
CODE ENDS
     END START
