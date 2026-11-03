; Template 2 - book style: tables in .DATA, DS set at the start (no CS: needed)
.MODEL SMALL
.DATA
; ---- tables here ----
TABLE DB 0FFH, 0FFH
.CODE
     ORG 1000H            ; the kit starts the program at 0000:1000
MAIN PROC
     MOV AX, @DATA
     MOV DS, AX           ; DS -> .DATA
     MOV AL, 80H
     OUT 1EH, AL          ; 8255 setup: 1EH = dot matrix, 1FH = 7-segment

     ; ---- write your program here ----
     XOR SI, SI
     MOV AL, TABLE[SI]    ; tables in .DATA are read without CS:

HERE: JMP HERE            ; a kit program never ends: it loops forever
MAIN ENDP
END
