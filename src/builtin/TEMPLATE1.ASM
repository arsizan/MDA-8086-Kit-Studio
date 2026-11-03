; Template 1 - short form: code and tables in .CODE (a table is read with CS:)
.MODEL SMALL
.CODE
     ORG 1000H            ; the kit starts the program at 0000:1000
     MOV AL, 80H
     OUT 1EH, AL          ; 8255 setup: 1EH = dot matrix, 1FH = 7-segment

     ; ---- write your program here ----
     XOR SI, SI
     MOV AL, CS:TABLE[SI] ; reading a table: CS: in front of its name

HERE: JMP HERE            ; a kit program never ends: it loops forever

; ---- tables here, after the code ----
TABLE DB 0FFH, 0FFH
END
