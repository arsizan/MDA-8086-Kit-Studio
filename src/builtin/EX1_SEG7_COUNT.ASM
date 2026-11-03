; Seven-segment display: counts 0, 1, 2 ... 9 again and again (about 1 s per digit)
.MODEL SMALL
.CODE
     ORG 1000H            ; the kit starts the program at 0000:1000
     MOV AL, 80H
     OUT 1FH, AL          ; 8255 (CS2): ports A, B, C = output
L1:  XOR SI, SI           ; SI = digit 0
L2:  MOV AL, CS:DIGIT[SI] ; segment code of this digit
     OUT 19H, AL          ; port A -> 7-segment (0 = segment on)
     CALL DELAY
     INC SI
     CMP SI, 10
     JB  L2               ; next digit
     JMP L1               ; start again from 0

DELAY:
     MOV DX, 5            ; about 1 second
D1:  MOV CX, 0FFFFH
D2:  LOOP D2
     DEC DX
     JNZ D1
     RET

DIGIT DB 0C0H, 0F9H, 0A4H, 0B0H, 99H   ; 0-4 (bits: dp g f e d c b a)
      DB 92H, 82H, 0F8H, 80H, 90H      ; 5-9
END
