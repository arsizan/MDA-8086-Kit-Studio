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
     DEC BX               ; move the text one column right
     AND BX, 31
     JMP NEXT
TIMER:
     MOV CX, 300          ; short delay for each column
T1:  LOOP T1
     RET
TEXT DB 5 DUP (0FFH)                      ; gap between words
     DB 0CEH, 0B6H, 0B6H, 0B6H, 0B9H, 0FFH ; S
     DB 0BEH, 80H, 0BEH, 0FFH              ; I
     DB 0BCH, 0BAH, 0B6H, 0AEH, 9EH, 0FFH  ; Z
     DB 0C0H, 0B7H, 0B7H, 0B7H, 0C0H, 0FFH ; A
     DB 80H, 0EFH, 0F7H, 0FBH, 80H         ; N
CODE ENDS
     END START
