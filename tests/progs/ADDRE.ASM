;*****************************************
;     MDA-Win8086 EXPERIMENT PROGRAM    *
;     FILENAME  : ADDRE.ASM
;     PROCESSOR : I8086
;*****************************************
CODE	SEGMENT
	ASSUME	CS:CODE,DS:CODE,ES:CODE,SS:CODE

	ORG    1000H
	;
        MOV     AX,0
        MOV     DS,AX
        MOV     ES,AX
        ;
        MOV     BX,2000H
        MOV     AX,[BX]
        MOV     CH,[BX+2]
        MOV     CL,[BX+3]
        ;
        MOV     BP,BX
        MOV     DX,DS:[BP+4]
        ;
        MOV     SI,ES:[BP+6]
        MOV     DI,[BP+8]
	;
	MOV	BX,[200AH]
        INT     3
	;
CODE	ENDS
	END
	

