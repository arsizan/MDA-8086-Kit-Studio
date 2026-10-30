.MODEL SMALL
.STACK 100H

.DATA
    ; 'A'                   8x8                (Hex code)
    ; 0 =   , 1 =    (          Active High)
    PATTERN DB 18H    ; Row 1: 00011000
            DB 24H    ; Row 2: 00100100
            DB 42H    ; Row 3: 01000010
            DB 7EH    ; Row 4: 01111110 (            )
            DB 42H    ; Row 5: 01000010
            DB 42H    ; Row 6: 01000010
            DB 42H    ; Row 7: 01000010
            DB 00H    ; Row 8: 00000000 (     )

    ;     Row                                (Row 1      8)
    ROWS    DB 01H, 02H, 04H, 08H, 10H, 20H, 40H, 80H

    ; !!!         !!!
    ; MDA-8086       8255 IC                                                         
    ;                 00H     02H            
    PORT_ROW EQU 00H  ; Row                    
    PORT_COL EQU 02H  ;     /                  

.CODE
MAIN PROC
    MOV AX, @DATA
    MOV DS, AX

    ; 8255 PPI-                                                (                       )

INFINITE_LOOP:
    LEA SI, PATTERN     ; SI                            
    LEA DI, ROWS        ; DI             Row          
    MOV CX, 8           ;     Row             

SCAN_LOOP:
    ;  .        Row            
    MOV AL, [DI]
    OUT PORT_ROW, AL    
    
    ;  .          Row              
    MOV AL, [SI]
    OUT PORT_COL, AL    
    
    ;  .                                     Delay 
    CALL DELAY_PROC     
    
    ;  . Ghosting (              )                        
    MOV AL, 00H
    OUT PORT_COL, AL    

    INC SI              ;          
    INC DI              ;      Row
    LOOP SCAN_LOOP      ;            
    
    JMP INFINITE_LOOP   ;                                

MAIN ENDP

; --- Delay Procedure ---
DELAY_PROC PROC
    PUSH CX
    MOV CX, 01FFH       ;                                                 /          
DELAY_L:
    NOP
    LOOP DELAY_L
    POP CX
    RET
DELAY_PROC ENDP

END MAIN


