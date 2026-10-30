; ============================================================
; DISPLAY_A.ASM
; Displays the letter 'A' on an 8x8 Dot Matrix (MDA-8086 Kit)
; ============================================================

ORG 1000H

; --- Initialization ---
MOV AL, 10000000B   ; Activate Signal (Set 8255 for I/O mode)
OUT 1EH, AL         ; Write Activate signal to Control Register

MOV AL, 11111111B   ; Off Signal (All LEDs off initially)
OUT 18H, AL         ; Write off signal to Port A

; --- Main Display Loop ---
L1:
    MOV SI, OFFSET FONT ; Point SI to the start of the font data
    MOV AH, 00000001B   ; Start with the first column (bit 0 set)

L2:
    MOV AL, BYTE PTR CS:[SI] ; Load the row data for the current column
    OUT 1AH, AL              ; Send row data to the dot matrix (Port 1A)
    
    MOV AL, AH
    OUT 1CH, AL              ; Select the active column (Port 1C)

    CALL TIMER               ; Small delay for persistence of vision

    INC SI                   ; Move to the next byte of font data
    CLC
    ROL AH, 1                ; Rotate to select the next column
    JNC L2                   ; If not all columns done, repeat
    JMP L1                   ; Restart the entire display loop

INT 3                        ; Halt program

; --- Delay Subroutine ---
TIMER:
    MOV CX, 300
TIMER1:
    NOP
    NOP
    NOP
    NOP
    LOOP TIMER1
    RET

; --- Font Data for 'A' ---
; Each byte represents a row of 8 pixels.
; '1' = LED OFF, '0' = LED ON (Active Low logic)
FONT:
    DB 11111111B
    DB 11001001B
    DB 10110100B
    DB 10110110B
    DB 10110110B
    DB 10110110B
    DB 10000000B
    DB 11111111B
