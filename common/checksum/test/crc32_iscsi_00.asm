;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;  Copyright(c) 2011-2015 Intel Corporation All rights reserved.
;
;  Redistribution and use in source and binary forms, with or without
;  modification, are permitted provided that the following conditions
;  are met:
;    * Redistributions of source code must retain the above copyright
;      notice, this list of conditions and the following disclaimer.
;    * Redistributions in binary form must reproduce the above copyright
;      notice, this list of conditions and the following disclaimer in
;      the documentation and/or other materials provided with the
;      distribution.
;    * Neither the name of Intel Corporation nor the names of its
;      contributors may be used to endorse or promote products derived
;      from this software without specific prior written permission.
;
;  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
;  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
;  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
;  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
;  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
;  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
;  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
;  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
;  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
;  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
;  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

; Function to compute iscsi CRC32 with table-based recombination
; crc done "by 3" with block sizes 1920, 960, 480, 240

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

; %include "reg_sizes.asm"
%ifndef _REG_SIZES_ASM_
%define _REG_SIZES_ASM_

%ifndef AS_FEATURE_LEVEL
%define AS_FEATURE_LEVEL 4
%endif

%define EFLAGS_HAS_CPUID        (1<<21)
%define FLAG_CPUID1_ECX_CLMUL   (1<<1)
%define FLAG_CPUID1_EDX_SSE2    (1<<26)
%define FLAG_CPUID1_ECX_SSE3	(1)
%define FLAG_CPUID1_ECX_SSE4_1  (1<<19)
%define FLAG_CPUID1_ECX_SSE4_2  (1<<20)
%define FLAG_CPUID1_ECX_POPCNT  (1<<23)
%define FLAG_CPUID1_ECX_AESNI   (1<<25)
%define FLAG_CPUID1_ECX_OSXSAVE (1<<27)
%define FLAG_CPUID1_ECX_AVX     (1<<28)
%define FLAG_CPUID1_EBX_AVX2    (1<<5)

%define FLAG_CPUID7_EBX_AVX2           (1<<5)
%define FLAG_CPUID7_EBX_AVX512F        (1<<16)
%define FLAG_CPUID7_EBX_AVX512DQ       (1<<17)
%define FLAG_CPUID7_EBX_AVX512IFMA     (1<<21)
%define FLAG_CPUID7_EBX_AVX512PF       (1<<26)
%define FLAG_CPUID7_EBX_AVX512ER       (1<<27)
%define FLAG_CPUID7_EBX_AVX512CD       (1<<28)
%define FLAG_CPUID7_EBX_AVX512BW       (1<<30)
%define FLAG_CPUID7_EBX_AVX512VL       (1<<31)

%define FLAG_CPUID7_ECX_AVX512VBMI     (1<<1)
%define FLAG_CPUID7_ECX_AVX512VBMI2    (1 << 6)
%define FLAG_CPUID7_ECX_GFNI           (1 << 8)
%define FLAG_CPUID7_ECX_VAES           (1 << 9)
%define FLAG_CPUID7_ECX_VPCLMULQDQ     (1 << 10)
%define FLAG_CPUID7_ECX_VNNI           (1 << 11)
%define FLAG_CPUID7_ECX_BITALG         (1 << 12)
%define FLAG_CPUID7_ECX_VPOPCNTDQ      (1 << 14)

%define FLAGS_CPUID7_EBX_AVX512_G1 (FLAG_CPUID7_EBX_AVX512F | FLAG_CPUID7_EBX_AVX512VL | FLAG_CPUID7_EBX_AVX512BW | FLAG_CPUID7_EBX_AVX512CD | FLAG_CPUID7_EBX_AVX512DQ)
%define FLAGS_CPUID7_ECX_AVX512_G2 (FLAG_CPUID7_ECX_AVX512VBMI2 | FLAG_CPUID7_ECX_GFNI | FLAG_CPUID7_ECX_VAES | FLAG_CPUID7_ECX_VPCLMULQDQ | FLAG_CPUID7_ECX_VNNI | FLAG_CPUID7_ECX_BITALG | FLAG_CPUID7_ECX_VPOPCNTDQ)
%define FLAGS_CPUID7_ECX_AVX2_G2 (FLAG_CPUID7_ECX_GFNI | FLAG_CPUID7_ECX_VAES | FLAG_CPUID7_ECX_VPCLMULQDQ)

%define FLAG_XGETBV_EAX_XMM            (1<<1)
%define FLAG_XGETBV_EAX_YMM            (1<<2)
%define FLAG_XGETBV_EAX_XMM_YMM        0x6
%define FLAG_XGETBV_EAX_ZMM_OPM        0xe0

%define FLAG_CPUID1_EAX_AVOTON     0x000406d0
%define FLAG_CPUID1_EAX_STEP_MASK  0xfffffff0

; define d and w variants for registers

%define	raxd	eax
%define raxw	ax
%define raxb	al

%define	rbxd	ebx
%define rbxw	bx
%define rbxb	bl

%define	rcxd	ecx
%define rcxw	cx
%define rcxb	cl

%define	rdxd	edx
%define rdxw	dx
%define rdxb	dl

%define	rsid	esi
%define rsiw	si
%define rsib	sil

%define	rdid	edi
%define rdiw	di
%define rdib	dil

%define	rbpd	ebp
%define rbpw	bp
%define rbpb	bpl

%define ymm0x xmm0
%define ymm1x xmm1
%define ymm2x xmm2
%define ymm3x xmm3
%define ymm4x xmm4
%define ymm5x xmm5
%define ymm6x xmm6
%define ymm7x xmm7
%define ymm8x xmm8
%define ymm9x xmm9
%define ymm10x xmm10
%define ymm11x xmm11
%define ymm12x xmm12
%define ymm13x xmm13
%define ymm14x xmm14
%define ymm15x xmm15

%define zmm0x xmm0
%define zmm1x xmm1
%define zmm2x xmm2
%define zmm3x xmm3
%define zmm4x xmm4
%define zmm5x xmm5
%define zmm6x xmm6
%define zmm7x xmm7
%define zmm8x xmm8
%define zmm9x xmm9
%define zmm10x xmm10
%define zmm11x xmm11
%define zmm12x xmm12
%define zmm13x xmm13
%define zmm14x xmm14
%define zmm15x xmm15
%define zmm16x xmm16
%define zmm17x xmm17
%define zmm18x xmm18
%define zmm19x xmm19
%define zmm20x xmm20
%define zmm21x xmm21
%define zmm22x xmm22
%define zmm23x xmm23
%define zmm24x xmm24
%define zmm25x xmm25
%define zmm26x xmm26
%define zmm27x xmm27
%define zmm28x xmm28
%define zmm29x xmm29
%define zmm30x xmm30
%define zmm31x xmm31

%define zmm0y ymm0
%define zmm1y ymm1
%define zmm2y ymm2
%define zmm3y ymm3
%define zmm4y ymm4
%define zmm5y ymm5
%define zmm6y ymm6
%define zmm7y ymm7
%define zmm8y ymm8
%define zmm9y ymm9
%define zmm10y ymm10
%define zmm11y ymm11
%define zmm12y ymm12
%define zmm13y ymm13
%define zmm14y ymm14
%define zmm15y ymm15
%define zmm16y ymm16
%define zmm17y ymm17
%define zmm18y ymm18
%define zmm19y ymm19
%define zmm20y ymm20
%define zmm21y ymm21
%define zmm22y ymm22
%define zmm23y ymm23
%define zmm24y ymm24
%define zmm25y ymm25
%define zmm26y ymm26
%define zmm27y ymm27
%define zmm28y ymm28
%define zmm29y ymm29
%define zmm30y ymm30
%define zmm31y ymm31

%define DWORD(reg) reg %+ d
%define WORD(reg)  reg %+ w
%define BYTE(reg)  reg %+ b

%define XWORD(reg) reg %+ x

%ifdef INTEL_CET_ENABLED
 %ifdef __NASM_VER__
  %if AS_FEATURE_LEVEL >= 10
   %ifidn __OUTPUT_FORMAT__,elf32
section .note.gnu.property  note  alloc noexec align=4
DD 0x00000004,0x0000000c,0x00000005,0x00554e47
DD 0xc0000002,0x00000004,0x00000003
   %endif
   %ifidn __OUTPUT_FORMAT__,elf64
section .note.gnu.property  note  alloc noexec align=8
DD 0x00000004,0x00000010,0x00000005,0x00554e47
DD 0xc0000002,0x00000004,0x00000003,0x00000000
   %endif
  %endif
 %endif
%endif

%ifidn __OUTPUT_FORMAT__,elf32
section .note.GNU-stack noalloc noexec nowrite progbits
section .text
%endif
%ifidn __OUTPUT_FORMAT__,elf64
 %define __x86_64__
section .note.GNU-stack noalloc noexec nowrite progbits
section .text
%endif
%ifidn __OUTPUT_FORMAT__,win64
 %define __x86_64__
%endif
%ifidn __OUTPUT_FORMAT__,macho64
 %define __x86_64__
%endif

%ifdef __x86_64__
 %define endbranch db 0xf3, 0x0f, 0x1e, 0xfa
%else
 %define endbranch db 0xf3, 0x0f, 0x1e, 0xfb
%endif

%ifdef REL_TEXT
 %define WRT_OPT
%elifidn __OUTPUT_FORMAT__, elf64
 %define WRT_OPT        wrt ..plt
%else
 %define WRT_OPT
%endif

%macro mk_global 1-3
  %ifdef __NASM_VER__
    %ifidn __OUTPUT_FORMAT__, macho64
	global %1
    %elifidn __OUTPUT_FORMAT__, win64
	global %1
    %else
	global %1:%2 %3
    %endif
  %else
	global %1:%2 %3
  %endif
%endmacro


; Fixes for nasm lack of MS proc helpers
%ifdef __NASM_VER__
  %ifidn __OUTPUT_FORMAT__, win64
    %macro alloc_stack 1
	sub	rsp, %1
    %endmacro

    %macro proc_frame 1
	%1:
    %endmacro

    %macro save_xmm128 2
	movdqa	[rsp + %2], %1
    %endmacro

    %macro save_reg 2
	mov	[rsp + %2], %1
    %endmacro

    %macro rex_push_reg	1
	push	%1
    %endmacro

    %macro push_reg 1
	push	%1
    %endmacro

    %define end_prolog
  %endif

  %define endproc_frame
%endif

%ifidn __OUTPUT_FORMAT__, macho64
 %define elf64 macho64
 mac_equ equ 1
%endif
%endif ; ifndef _REG_SIZES_ASM_


default rel
section .text

; crcB3 MACRO to implement crc32 on 3 %%bSize-byte blocks
%macro  crcB3 3
%define %%bSize   %1    ; 1/3 of buffer size
%define %%td2     %2    ; table offset for crc0 (2/3 of buffer)
%define %%td1     %3    ; table offset for crc1 (1/3 of buffer)

%IF %%bSize=640
	sub     len, %%bSize*3
	js      %%crcB3_end           ;; jump to next level if 3*blockSize > len
%ELSE
	cmp     len, %%bSize*3
	jnae    %%crcB3_end           ;; jump to next level if 3*blockSize > len
%ENDIF
	;;;;;; Calculate CRC of 3 blocks of the buffer ;;;;;;
%%crcB3_loop:
					;; rax = crc0 = initial crc
	xor     rbx, rbx                ;; rbx = crc1 = 0;
	xor     r10, r10                ;; r10 = crc2 = 0;

	cmp 	len, %%bSize*3*2
	jbe 	%%non_prefetch

 %assign i 0
 %rep %%bSize/8 - 1
  %if i < %%bSize*3/4
	prefetchnta  [bufptmp+ %%bSize*3 +i*4]
  %endif
	crc32   rax, qword [bufptmp+i + 0*%%bSize]  ;; update crc0
	crc32   rbx, qword [bufptmp+i + 1*%%bSize]  ;; update crc1
	crc32   r10, qword [bufptmp+i + 2*%%bSize]  ;; update crc2
	%assign i (i+8)
 %endrep
 	jmp %%next %+ %1

%%non_prefetch:
 %assign i 0
 %rep %%bSize/8 - 1
	crc32   rax, qword [bufptmp+i + 0*%%bSize]  ;; update crc0
	crc32   rbx, qword [bufptmp+i + 1*%%bSize]  ;; update crc1
	crc32   r10, qword [bufptmp+i + 2*%%bSize]  ;; update crc2
	%assign i (i+8)
 %endrep

%%next %+ %1:
	crc32   rax, qword [bufptmp+i + 0*%%bSize]  ;; update crc0
	crc32   rbx, qword [bufptmp+i + 1*%%bSize]  ;; update crc1
; SKIP  ;crc32  r10, [bufptmp+i + 2*%%bSize]  ;; update crc2

	; merge in crc0
	movzx   bufp_dw, al
	mov     r9d, [crc_init + bufp*4 + %%td2]
	movzx   bufp_dw, ah
	shr     eax, 16
	mov     r11d, [crc_init + bufp*4 + %%td2]
	shl     r11, 8
	xor     r9, r11

	movzx   bufp_dw, al
	mov     r11d, [crc_init + bufp*4 + %%td2]
	movzx   bufp_dw, ah
	shl     r11, 16
	xor     r9, r11
	mov     r11d, [crc_init + bufp*4 + %%td2]
	shl     r11, 24
	xor     r9, r11

	; merge in crc1

	movzx   bufp_dw, bl
	mov     r11d, [crc_init + bufp*4 + %%td1]
	movzx   bufp_dw, bh
	shr     ebx, 16
	xor     r9, r11
	mov     r11d, [crc_init + bufp*4 + %%td1]
	shl     r11, 8
	xor     r9, r11

	movzx   bufp_dw, bl
	mov     r11d, [crc_init + bufp*4 + %%td1]
	movzx   bufp_dw, bh
	shl     r11, 16
	xor     r9, r11
	mov     r11d, [crc_init + bufp*4 + %%td1]
	shl     r11, 24
	xor     r9, r11

	xor     r9, [bufptmp+i + 2*%%bSize]
	crc32   r10, r9
	mov     rax, r10

	add     bufptmp, %%bSize*3      ;; move to next block
	sub     len, %%bSize*3
%IF %%bSize=640
	jns     %%crcB3_loop
%ENDIF
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
%%crcB3_end:
%IF %%bSize=640
	add     len, %%bSize*3
%ENDIF
	je      do_return               ;; return if remaining data is zero
%endmacro

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

;;; ISCSI CRC 32 Implementation with crc32 Instruction

;;; unsigned int crc32_iscsi_00(unsigned char * buffer, int len, unsigned int crc_init);
;;;
;;;        *buf = rcx
;;;         len = rdx
;;;    crc_init = r8
;;;

mk_global  crc32_iscsi_00, function
crc32_iscsi_00:
	endbranch

%ifidn __OUTPUT_FORMAT__, elf64
%define bufp            rdi
%define bufp_dw         edi
%define bufp_w          di
%define bufp_b          dil
%define bufptmp         rcx
%define block_0         rcx
%define block_1         r8
%define block_2         r11
%define len             rsi
%define len_dw          esi
%define len_w           si
%define len_b           sil
%define crc_init        rdx
%define crc_init_dw     edx
%else
%define bufp            rcx
%define bufp_dw         ecx
%define bufp_w          cx
%define bufp_b          cl
%define bufptmp         rdi
%define block_0         rdi
%define block_1         rsi
%define block_2         r11
%define len             rdx
%define len_dw          edx
%define len_w           dx
%define len_b           dl
%define crc_init        r8
%define crc_init_dw     r8d
%endif


	push    rdi
	push    rbx

	mov     rax, crc_init           ;; rax = crc_init;

	cmp     len, 8
	jb      less_than_8

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; 1) ALIGN: ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

	mov     bufptmp, bufp           ;; rdi = *buf
	neg     bufp
	and     bufp, 7                 ;; calculate the unalignment
					;; amount of the address
	je      proc_block              ;; Skip if aligned

	;;;; Calculate CRC of unaligned bytes of the buffer (if any) ;;;;
	mov     rbx, [bufptmp]          ;; load a quadword from the buffer
	add     bufptmp, bufp           ;; align buffer pointer for
					;; quadword processing
	sub     len, bufp               ;; update buffer length
align_loop:
	crc32   eax, bl                 ;;    compute crc32 of 1-byte
	shr     rbx, 8                  ;;    get next byte
	dec     bufp
	jne     align_loop
	;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; 2) BLOCK LEVEL: ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

proc_block:
	cmp     len, 240
	jb      bit8

	lea     crc_init, [mul_table_72]  ;; load table base address

	crcB3   640, 0x1000, 0x0c00     ; 640*3 = 1920 (Tables 1280, 640)
	crcB3   320, 0x0c00, 0x0800     ; 320*3 =  960 (Tables  640, 320)
	crcB3   160, 0x0800, 0x0400     ; 160*3 =  480 (Tables  320, 160)
	crcB3    80, 0x0400, 0x0000     ;  80*3 =  240 (Tables  160,  80)


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;4) LESS THAN 256-bytes REMAIN AT THIS POINT (8-bits of rdx are full)

bit8:
	shl     len_b, 1                ;; shift-out MSB (bit-7)
	jnc     bit7                    ;; jump to bit-6 if bit-7 == 0
 %assign i 0
 %rep 16
	crc32   rax, qword [bufptmp+i]  ;; compute crc32 of 8-byte data
	%assign i (i+8)
 %endrep
	je      do_return               ;; return if remaining data is zero
	add     bufptmp, 128            ;; buf +=64; (next 64 bytes)

bit7:
	shl     len_b, 1                ;; shift-out MSB (bit-7)
	jnc     bit6                    ;; jump to bit-6 if bit-7 == 0
 %assign i 0
 %rep 8
	crc32   rax, qword [bufptmp+i]  ;; compute crc32 of 8-byte data
	%assign i (i+8)
 %endrep
	je      do_return               ;; return if remaining data is zero
	add     bufptmp, 64             ;; buf +=64; (next 64 bytes)
bit6:
	shl     len_b, 1                ;; shift-out MSB (bit-6)
	jnc     bit5                    ;; jump to bit-5 if bit-6 == 0
 %assign i 0
 %rep 4
	crc32   rax, qword [bufptmp+i]  ;;    compute crc32 of 8-byte data
	%assign i (i+8)
 %endrep
	je      do_return               ;; return if remaining data is zero
	add     bufptmp, 32             ;; buf +=32; (next 32 bytes)
bit5:
	shl     len_b, 1                ;; shift-out MSB (bit-5)
	jnc     bit4                    ;; jump to bit-4 if bit-5 == 0
 %assign i 0
 %rep 2
	crc32   rax, qword [bufptmp+i]  ;;    compute crc32 of 8-byte data
	%assign i (i+8)
 %endrep
	je      do_return               ;; return if remaining data is zero
	add     bufptmp, 16             ;; buf +=16; (next 16 bytes)
bit4:
	shl     len_b, 1                ;; shift-out MSB (bit-4)
	jnc     bit3                    ;; jump to bit-3 if bit-4 == 0
	crc32   rax, qword [bufptmp]    ;; compute crc32 of 8-byte data
	je      do_return               ;; return if remaining data is zero
	add     bufptmp, 8              ;; buf +=8; (next 8 bytes)
bit3:
	mov     rbx, qword [bufptmp]    ;; load a 8-bytes from the buffer:
	shl     len_b, 1                ;; shift-out MSB (bit-3)
	jnc     bit2                    ;; jump to bit-2 if bit-3 == 0
	crc32   eax, ebx                ;; compute crc32 of 4-byte data
	je      do_return               ;; return if remaining data is zero
	shr     rbx, 32                 ;; get next 3 bytes
bit2:
	shl     len_b, 1                ;; shift-out MSB (bit-2)
	jnc     bit1                    ;; jump to bit-1 if bit-2 == 0
	crc32   eax, bx                 ;; compute crc32 of 2-byte data
	je      do_return               ;; return if remaining data is zero
	shr     rbx, 16                 ;; next byte
bit1:
	test    len_b,len_b
	je      do_return
	crc32   eax, bl                 ;; compute crc32 of 1-byte data
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

do_return:

	pop     rbx
	pop     rdi
	ret

less_than_8:
	test    len,4
	jz      less_than_4
	crc32   eax, dword[bufp]
	add     bufp,4
less_than_4:
	test    len,2
	jz      less_than_2
	crc32   eax, word[bufp]
	add     bufp,2
less_than_2:
	test    len,1
	jz      do_return
	crc32   rax, byte[bufp]
	pop     rbx
	pop     bufp
	ret

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

;;; global mul_table_72, mul_table_152, mul_table_312, mul_table_632, mul_table_1272

section .data
align   8
mul_table_72:
DD 0x00000000,0x39d3b296,0x73a7652c,0x4a74d7ba
DD 0xe74eca58,0xde9d78ce,0x94e9af74,0xad3a1de2
DD 0xcb71e241,0xf2a250d7,0xb8d6876d,0x810535fb
DD 0x2c3f2819,0x15ec9a8f,0x5f984d35,0x664bffa3
DD 0x930fb273,0xaadc00e5,0xe0a8d75f,0xd97b65c9
DD 0x7441782b,0x4d92cabd,0x07e61d07,0x3e35af91
DD 0x587e5032,0x61ade2a4,0x2bd9351e,0x120a8788
DD 0xbf309a6a,0x86e328fc,0xcc97ff46,0xf5444dd0
DD 0x23f31217,0x1a20a081,0x5054773b,0x6987c5ad
DD 0xc4bdd84f,0xfd6e6ad9,0xb71abd63,0x8ec90ff5
DD 0xe882f056,0xd15142c0,0x9b25957a,0xa2f627ec
DD 0x0fcc3a0e,0x361f8898,0x7c6b5f22,0x45b8edb4
DD 0xb0fca064,0x892f12f2,0xc35bc548,0xfa8877de
DD 0x57b26a3c,0x6e61d8aa,0x24150f10,0x1dc6bd86
DD 0x7b8d4225,0x425ef0b3,0x082a2709,0x31f9959f
DD 0x9cc3887d,0xa5103aeb,0xef64ed51,0xd6b75fc7
DD 0x47e6242e,0x7e3596b8,0x34414102,0x0d92f394
DD 0xa0a8ee76,0x997b5ce0,0xd30f8b5a,0xeadc39cc
DD 0x8c97c66f,0xb54474f9,0xff30a343,0xc6e311d5
DD 0x6bd90c37,0x520abea1,0x187e691b,0x21addb8d
DD 0xd4e9965d,0xed3a24cb,0xa74ef371,0x9e9d41e7
DD 0x33a75c05,0x0a74ee93,0x40003929,0x79d38bbf
DD 0x1f98741c,0x264bc68a,0x6c3f1130,0x55eca3a6
DD 0xf8d6be44,0xc1050cd2,0x8b71db68,0xb2a269fe
DD 0x64153639,0x5dc684af,0x17b25315,0x2e61e183
DD 0x835bfc61,0xba884ef7,0xf0fc994d,0xc92f2bdb
DD 0xaf64d478,0x96b766ee,0xdcc3b154,0xe51003c2
DD 0x482a1e20,0x71f9acb6,0x3b8d7b0c,0x025ec99a
DD 0xf71a844a,0xcec936dc,0x84bde166,0xbd6e53f0
DD 0x10544e12,0x2987fc84,0x63f32b3e,0x5a2099a8
DD 0x3c6b660b,0x05b8d49d,0x4fcc0327,0x761fb1b1
DD 0xdb25ac53,0xe2f61ec5,0xa882c97f,0x91517be9
DD 0x8fcc485c,0xb61ffaca,0xfc6b2d70,0xc5b89fe6
DD 0x68828204,0x51513092,0x1b25e728,0x22f655be
DD 0x44bdaa1d,0x7d6e188b,0x371acf31,0x0ec97da7
DD 0xa3f36045,0x9a20d2d3,0xd0540569,0xe987b7ff
DD 0x1cc3fa2f,0x251048b9,0x6f649f03,0x56b72d95
DD 0xfb8d3077,0xc25e82e1,0x882a555b,0xb1f9e7cd
DD 0xd7b2186e,0xee61aaf8,0xa4157d42,0x9dc6cfd4
DD 0x30fcd236,0x092f60a0,0x435bb71a,0x7a88058c
DD 0xac3f5a4b,0x95ece8dd,0xdf983f67,0xe64b8df1
DD 0x4b719013,0x72a22285,0x38d6f53f,0x010547a9
DD 0x674eb80a,0x5e9d0a9c,0x14e9dd26,0x2d3a6fb0
DD 0x80007252,0xb9d3c0c4,0xf3a7177e,0xca74a5e8
DD 0x3f30e838,0x06e35aae,0x4c978d14,0x75443f82
DD 0xd87e2260,0xe1ad90f6,0xabd9474c,0x920af5da
DD 0xf4410a79,0xcd92b8ef,0x87e66f55,0xbe35ddc3
DD 0x130fc021,0x2adc72b7,0x60a8a50d,0x597b179b
DD 0xc82a6c72,0xf1f9dee4,0xbb8d095e,0x825ebbc8
DD 0x2f64a62a,0x16b714bc,0x5cc3c306,0x65107190
DD 0x035b8e33,0x3a883ca5,0x70fceb1f,0x492f5989
DD 0xe415446b,0xddc6f6fd,0x97b22147,0xae6193d1
DD 0x5b25de01,0x62f66c97,0x2882bb2d,0x115109bb
DD 0xbc6b1459,0x85b8a6cf,0xcfcc7175,0xf61fc3e3
DD 0x90543c40,0xa9878ed6,0xe3f3596c,0xda20ebfa
DD 0x771af618,0x4ec9448e,0x04bd9334,0x3d6e21a2
DD 0xebd97e65,0xd20accf3,0x987e1b49,0xa1ada9df
DD 0x0c97b43d,0x354406ab,0x7f30d111,0x46e36387
DD 0x20a89c24,0x197b2eb2,0x530ff908,0x6adc4b9e
DD 0xc7e6567c,0xfe35e4ea,0xb4413350,0x8d9281c6
DD 0x78d6cc16,0x41057e80,0x0b71a93a,0x32a21bac
DD 0x9f98064e,0xa64bb4d8,0xec3f6362,0xd5ecd1f4
DD 0xb3a72e57,0x8a749cc1,0xc0004b7b,0xf9d3f9ed
DD 0x54e9e40f,0x6d3a5699,0x274e8123,0x1e9d33b5

mul_table_152:
DD 0x00000000,0x878a92a7,0x0af953bf,0x8d73c118
DD 0x15f2a77e,0x927835d9,0x1f0bf4c1,0x98816666
DD 0x2be54efc,0xac6fdc5b,0x211c1d43,0xa6968fe4
DD 0x3e17e982,0xb99d7b25,0x34eeba3d,0xb364289a
DD 0x57ca9df8,0xd0400f5f,0x5d33ce47,0xdab95ce0
DD 0x42383a86,0xc5b2a821,0x48c16939,0xcf4bfb9e
DD 0x7c2fd304,0xfba541a3,0x76d680bb,0xf15c121c
DD 0x69dd747a,0xee57e6dd,0x632427c5,0xe4aeb562
DD 0xaf953bf0,0x281fa957,0xa56c684f,0x22e6fae8
DD 0xba679c8e,0x3ded0e29,0xb09ecf31,0x37145d96
DD 0x8470750c,0x03fae7ab,0x8e8926b3,0x0903b414
DD 0x9182d272,0x160840d5,0x9b7b81cd,0x1cf1136a
DD 0xf85fa608,0x7fd534af,0xf2a6f5b7,0x752c6710
DD 0xedad0176,0x6a2793d1,0xe75452c9,0x60dec06e
DD 0xd3bae8f4,0x54307a53,0xd943bb4b,0x5ec929ec
DD 0xc6484f8a,0x41c2dd2d,0xccb11c35,0x4b3b8e92
DD 0x5ac60111,0xdd4c93b6,0x503f52ae,0xd7b5c009
DD 0x4f34a66f,0xc8be34c8,0x45cdf5d0,0xc2476777
DD 0x71234fed,0xf6a9dd4a,0x7bda1c52,0xfc508ef5
DD 0x64d1e893,0xe35b7a34,0x6e28bb2c,0xe9a2298b
DD 0x0d0c9ce9,0x8a860e4e,0x07f5cf56,0x807f5df1
DD 0x18fe3b97,0x9f74a930,0x12076828,0x958dfa8f
DD 0x26e9d215,0xa16340b2,0x2c1081aa,0xab9a130d
DD 0x331b756b,0xb491e7cc,0x39e226d4,0xbe68b473
DD 0xf5533ae1,0x72d9a846,0xffaa695e,0x7820fbf9
DD 0xe0a19d9f,0x672b0f38,0xea58ce20,0x6dd25c87
DD 0xdeb6741d,0x593ce6ba,0xd44f27a2,0x53c5b505
DD 0xcb44d363,0x4cce41c4,0xc1bd80dc,0x4637127b
DD 0xa299a719,0x251335be,0xa860f4a6,0x2fea6601
DD 0xb76b0067,0x30e192c0,0xbd9253d8,0x3a18c17f
DD 0x897ce9e5,0x0ef67b42,0x8385ba5a,0x040f28fd
DD 0x9c8e4e9b,0x1b04dc3c,0x96771d24,0x11fd8f83
DD 0xb58c0222,0x32069085,0xbf75519d,0x38ffc33a
DD 0xa07ea55c,0x27f437fb,0xaa87f6e3,0x2d0d6444
DD 0x9e694cde,0x19e3de79,0x94901f61,0x131a8dc6
DD 0x8b9beba0,0x0c117907,0x8162b81f,0x06e82ab8
DD 0xe2469fda,0x65cc0d7d,0xe8bfcc65,0x6f355ec2
DD 0xf7b438a4,0x703eaa03,0xfd4d6b1b,0x7ac7f9bc
DD 0xc9a3d126,0x4e294381,0xc35a8299,0x44d0103e
DD 0xdc517658,0x5bdbe4ff,0xd6a825e7,0x5122b740
DD 0x1a1939d2,0x9d93ab75,0x10e06a6d,0x976af8ca
DD 0x0feb9eac,0x88610c0b,0x0512cd13,0x82985fb4
DD 0x31fc772e,0xb676e589,0x3b052491,0xbc8fb636
DD 0x240ed050,0xa38442f7,0x2ef783ef,0xa97d1148
DD 0x4dd3a42a,0xca59368d,0x472af795,0xc0a06532
DD 0x58210354,0xdfab91f3,0x52d850eb,0xd552c24c
DD 0x6636ead6,0xe1bc7871,0x6ccfb969,0xeb452bce
DD 0x73c44da8,0xf44edf0f,0x793d1e17,0xfeb78cb0
DD 0xef4a0333,0x68c09194,0xe5b3508c,0x6239c22b
DD 0xfab8a44d,0x7d3236ea,0xf041f7f2,0x77cb6555
DD 0xc4af4dcf,0x4325df68,0xce561e70,0x49dc8cd7
DD 0xd15deab1,0x56d77816,0xdba4b90e,0x5c2e2ba9
DD 0xb8809ecb,0x3f0a0c6c,0xb279cd74,0x35f35fd3
DD 0xad7239b5,0x2af8ab12,0xa78b6a0a,0x2001f8ad
DD 0x9365d037,0x14ef4290,0x999c8388,0x1e16112f
DD 0x86977749,0x011de5ee,0x8c6e24f6,0x0be4b651
DD 0x40df38c3,0xc755aa64,0x4a266b7c,0xcdacf9db
DD 0x552d9fbd,0xd2a70d1a,0x5fd4cc02,0xd85e5ea5
DD 0x6b3a763f,0xecb0e498,0x61c32580,0xe649b727
DD 0x7ec8d141,0xf94243e6,0x743182fe,0xf3bb1059
DD 0x1715a53b,0x909f379c,0x1decf684,0x9a666423
DD 0x02e70245,0x856d90e2,0x081e51fa,0x8f94c35d
DD 0x3cf0ebc7,0xbb7a7960,0x3609b878,0xb1832adf
DD 0x29024cb9,0xae88de1e,0x23fb1f06,0xa4718da1

mul_table_312:
DD 0x00000000,0xbac2fd7b,0x70698c07,0xcaab717c
DD 0xe0d3180e,0x5a11e575,0x90ba9409,0x2a786972
DD 0xc44a46ed,0x7e88bb96,0xb423caea,0x0ee13791
DD 0x24995ee3,0x9e5ba398,0x54f0d2e4,0xee322f9f
DD 0x8d78fb2b,0x37ba0650,0xfd11772c,0x47d38a57
DD 0x6dabe325,0xd7691e5e,0x1dc26f22,0xa7009259
DD 0x4932bdc6,0xf3f040bd,0x395b31c1,0x8399ccba
DD 0xa9e1a5c8,0x132358b3,0xd98829cf,0x634ad4b4
DD 0x1f1d80a7,0xa5df7ddc,0x6f740ca0,0xd5b6f1db
DD 0xffce98a9,0x450c65d2,0x8fa714ae,0x3565e9d5
DD 0xdb57c64a,0x61953b31,0xab3e4a4d,0x11fcb736
DD 0x3b84de44,0x8146233f,0x4bed5243,0xf12faf38
DD 0x92657b8c,0x28a786f7,0xe20cf78b,0x58ce0af0
DD 0x72b66382,0xc8749ef9,0x02dfef85,0xb81d12fe
DD 0x562f3d61,0xecedc01a,0x2646b166,0x9c844c1d
DD 0xb6fc256f,0x0c3ed814,0xc695a968,0x7c575413
DD 0x3e3b014e,0x84f9fc35,0x4e528d49,0xf4907032
DD 0xdee81940,0x642ae43b,0xae819547,0x1443683c
DD 0xfa7147a3,0x40b3bad8,0x8a18cba4,0x30da36df
DD 0x1aa25fad,0xa060a2d6,0x6acbd3aa,0xd0092ed1
DD 0xb343fa65,0x0981071e,0xc32a7662,0x79e88b19
DD 0x5390e26b,0xe9521f10,0x23f96e6c,0x993b9317
DD 0x7709bc88,0xcdcb41f3,0x0760308f,0xbda2cdf4
DD 0x97daa486,0x2d1859fd,0xe7b32881,0x5d71d5fa
DD 0x212681e9,0x9be47c92,0x514f0dee,0xeb8df095
DD 0xc1f599e7,0x7b37649c,0xb19c15e0,0x0b5ee89b
DD 0xe56cc704,0x5fae3a7f,0x95054b03,0x2fc7b678
DD 0x05bfdf0a,0xbf7d2271,0x75d6530d,0xcf14ae76
DD 0xac5e7ac2,0x169c87b9,0xdc37f6c5,0x66f50bbe
DD 0x4c8d62cc,0xf64f9fb7,0x3ce4eecb,0x862613b0
DD 0x68143c2f,0xd2d6c154,0x187db028,0xa2bf4d53
DD 0x88c72421,0x3205d95a,0xf8aea826,0x426c555d
DD 0x7c76029c,0xc6b4ffe7,0x0c1f8e9b,0xb6dd73e0
DD 0x9ca51a92,0x2667e7e9,0xeccc9695,0x560e6bee
DD 0xb83c4471,0x02feb90a,0xc855c876,0x7297350d
DD 0x58ef5c7f,0xe22da104,0x2886d078,0x92442d03
DD 0xf10ef9b7,0x4bcc04cc,0x816775b0,0x3ba588cb
DD 0x11dde1b9,0xab1f1cc2,0x61b46dbe,0xdb7690c5
DD 0x3544bf5a,0x8f864221,0x452d335d,0xffefce26
DD 0xd597a754,0x6f555a2f,0xa5fe2b53,0x1f3cd628
DD 0x636b823b,0xd9a97f40,0x13020e3c,0xa9c0f347
DD 0x83b89a35,0x397a674e,0xf3d11632,0x4913eb49
DD 0xa721c4d6,0x1de339ad,0xd74848d1,0x6d8ab5aa
DD 0x47f2dcd8,0xfd3021a3,0x379b50df,0x8d59ada4
DD 0xee137910,0x54d1846b,0x9e7af517,0x24b8086c
DD 0x0ec0611e,0xb4029c65,0x7ea9ed19,0xc46b1062
DD 0x2a593ffd,0x909bc286,0x5a30b3fa,0xe0f24e81
DD 0xca8a27f3,0x7048da88,0xbae3abf4,0x0021568f
DD 0x424d03d2,0xf88ffea9,0x32248fd5,0x88e672ae
DD 0xa29e1bdc,0x185ce6a7,0xd2f797db,0x68356aa0
DD 0x8607453f,0x3cc5b844,0xf66ec938,0x4cac3443
DD 0x66d45d31,0xdc16a04a,0x16bdd136,0xac7f2c4d
DD 0xcf35f8f9,0x75f70582,0xbf5c74fe,0x059e8985
DD 0x2fe6e0f7,0x95241d8c,0x5f8f6cf0,0xe54d918b
DD 0x0b7fbe14,0xb1bd436f,0x7b163213,0xc1d4cf68
DD 0xebaca61a,0x516e5b61,0x9bc52a1d,0x2107d766
DD 0x5d508375,0xe7927e0e,0x2d390f72,0x97fbf209
DD 0xbd839b7b,0x07416600,0xcdea177c,0x7728ea07
DD 0x991ac598,0x23d838e3,0xe973499f,0x53b1b4e4
DD 0x79c9dd96,0xc30b20ed,0x09a05191,0xb362acea
DD 0xd028785e,0x6aea8525,0xa041f459,0x1a830922
DD 0x30fb6050,0x8a399d2b,0x4092ec57,0xfa50112c
DD 0x14623eb3,0xaea0c3c8,0x640bb2b4,0xdec94fcf
DD 0xf4b126bd,0x4e73dbc6,0x84d8aaba,0x3e1a57c1

mul_table_632:
DD 0x00000000,0x6b749fb2,0xd6e93f64,0xbd9da0d6
DD 0xa83e0839,0xc34a978b,0x7ed7375d,0x15a3a8ef
DD 0x55906683,0x3ee4f931,0x837959e7,0xe80dc655
DD 0xfdae6eba,0x96daf108,0x2b4751de,0x4033ce6c
DD 0xab20cd06,0xc05452b4,0x7dc9f262,0x16bd6dd0
DD 0x031ec53f,0x686a5a8d,0xd5f7fa5b,0xbe8365e9
DD 0xfeb0ab85,0x95c43437,0x285994e1,0x432d0b53
DD 0x568ea3bc,0x3dfa3c0e,0x80679cd8,0xeb13036a
DD 0x53adecfd,0x38d9734f,0x8544d399,0xee304c2b
DD 0xfb93e4c4,0x90e77b76,0x2d7adba0,0x460e4412
DD 0x063d8a7e,0x6d4915cc,0xd0d4b51a,0xbba02aa8
DD 0xae038247,0xc5771df5,0x78eabd23,0x139e2291
DD 0xf88d21fb,0x93f9be49,0x2e641e9f,0x4510812d
DD 0x50b329c2,0x3bc7b670,0x865a16a6,0xed2e8914
DD 0xad1d4778,0xc669d8ca,0x7bf4781c,0x1080e7ae
DD 0x05234f41,0x6e57d0f3,0xd3ca7025,0xb8beef97
DD 0xa75bd9fa,0xcc2f4648,0x71b2e69e,0x1ac6792c
DD 0x0f65d1c3,0x64114e71,0xd98ceea7,0xb2f87115
DD 0xf2cbbf79,0x99bf20cb,0x2422801d,0x4f561faf
DD 0x5af5b740,0x318128f2,0x8c1c8824,0xe7681796
DD 0x0c7b14fc,0x670f8b4e,0xda922b98,0xb1e6b42a
DD 0xa4451cc5,0xcf318377,0x72ac23a1,0x19d8bc13
DD 0x59eb727f,0x329fedcd,0x8f024d1b,0xe476d2a9
DD 0xf1d57a46,0x9aa1e5f4,0x273c4522,0x4c48da90
DD 0xf4f63507,0x9f82aab5,0x221f0a63,0x496b95d1
DD 0x5cc83d3e,0x37bca28c,0x8a21025a,0xe1559de8
DD 0xa1665384,0xca12cc36,0x778f6ce0,0x1cfbf352
DD 0x09585bbd,0x622cc40f,0xdfb164d9,0xb4c5fb6b
DD 0x5fd6f801,0x34a267b3,0x893fc765,0xe24b58d7
DD 0xf7e8f038,0x9c9c6f8a,0x2101cf5c,0x4a7550ee
DD 0x0a469e82,0x61320130,0xdcafa1e6,0xb7db3e54
DD 0xa27896bb,0xc90c0909,0x7491a9df,0x1fe5366d
DD 0x4b5bc505,0x202f5ab7,0x9db2fa61,0xf6c665d3
DD 0xe365cd3c,0x8811528e,0x358cf258,0x5ef86dea
DD 0x1ecba386,0x75bf3c34,0xc8229ce2,0xa3560350
DD 0xb6f5abbf,0xdd81340d,0x601c94db,0x0b680b69
DD 0xe07b0803,0x8b0f97b1,0x36923767,0x5de6a8d5
DD 0x4845003a,0x23319f88,0x9eac3f5e,0xf5d8a0ec
DD 0xb5eb6e80,0xde9ff132,0x630251e4,0x0876ce56
DD 0x1dd566b9,0x76a1f90b,0xcb3c59dd,0xa048c66f
DD 0x18f629f8,0x7382b64a,0xce1f169c,0xa56b892e
DD 0xb0c821c1,0xdbbcbe73,0x66211ea5,0x0d558117
DD 0x4d664f7b,0x2612d0c9,0x9b8f701f,0xf0fbefad
DD 0xe5584742,0x8e2cd8f0,0x33b17826,0x58c5e794
DD 0xb3d6e4fe,0xd8a27b4c,0x653fdb9a,0x0e4b4428
DD 0x1be8ecc7,0x709c7375,0xcd01d3a3,0xa6754c11
DD 0xe646827d,0x8d321dcf,0x30afbd19,0x5bdb22ab
DD 0x4e788a44,0x250c15f6,0x9891b520,0xf3e52a92
DD 0xec001cff,0x8774834d,0x3ae9239b,0x519dbc29
DD 0x443e14c6,0x2f4a8b74,0x92d72ba2,0xf9a3b410
DD 0xb9907a7c,0xd2e4e5ce,0x6f794518,0x040ddaaa
DD 0x11ae7245,0x7adaedf7,0xc7474d21,0xac33d293
DD 0x4720d1f9,0x2c544e4b,0x91c9ee9d,0xfabd712f
DD 0xef1ed9c0,0x846a4672,0x39f7e6a4,0x52837916
DD 0x12b0b77a,0x79c428c8,0xc459881e,0xaf2d17ac
DD 0xba8ebf43,0xd1fa20f1,0x6c678027,0x07131f95
DD 0xbfadf002,0xd4d96fb0,0x6944cf66,0x023050d4
DD 0x1793f83b,0x7ce76789,0xc17ac75f,0xaa0e58ed
DD 0xea3d9681,0x81490933,0x3cd4a9e5,0x57a03657
DD 0x42039eb8,0x2977010a,0x94eaa1dc,0xff9e3e6e
DD 0x148d3d04,0x7ff9a2b6,0xc2640260,0xa9109dd2
DD 0xbcb3353d,0xd7c7aa8f,0x6a5a0a59,0x012e95eb
DD 0x411d5b87,0x2a69c435,0x97f464e3,0xfc80fb51
DD 0xe92353be,0x8257cc0c,0x3fca6cda,0x54bef368

mul_table_1272:
DD 0x00000000,0xdd66cbbb,0xbf21e187,0x62472a3c
DD 0x7bafb5ff,0xa6c97e44,0xc48e5478,0x19e89fc3
DD 0xf75f6bfe,0x2a39a045,0x487e8a79,0x951841c2
DD 0x8cf0de01,0x519615ba,0x33d13f86,0xeeb7f43d
DD 0xeb52a10d,0x36346ab6,0x5473408a,0x89158b31
DD 0x90fd14f2,0x4d9bdf49,0x2fdcf575,0xf2ba3ece
DD 0x1c0dcaf3,0xc16b0148,0xa32c2b74,0x7e4ae0cf
DD 0x67a27f0c,0xbac4b4b7,0xd8839e8b,0x05e55530
DD 0xd34934eb,0x0e2fff50,0x6c68d56c,0xb10e1ed7
DD 0xa8e68114,0x75804aaf,0x17c76093,0xcaa1ab28
DD 0x24165f15,0xf97094ae,0x9b37be92,0x46517529
DD 0x5fb9eaea,0x82df2151,0xe0980b6d,0x3dfec0d6
DD 0x381b95e6,0xe57d5e5d,0x873a7461,0x5a5cbfda
DD 0x43b42019,0x9ed2eba2,0xfc95c19e,0x21f30a25
DD 0xcf44fe18,0x122235a3,0x70651f9f,0xad03d424
DD 0xb4eb4be7,0x698d805c,0x0bcaaa60,0xd6ac61db
DD 0xa37e1f27,0x7e18d49c,0x1c5ffea0,0xc139351b
DD 0xd8d1aad8,0x05b76163,0x67f04b5f,0xba9680e4
DD 0x542174d9,0x8947bf62,0xeb00955e,0x36665ee5
DD 0x2f8ec126,0xf2e80a9d,0x90af20a1,0x4dc9eb1a
DD 0x482cbe2a,0x954a7591,0xf70d5fad,0x2a6b9416
DD 0x33830bd5,0xeee5c06e,0x8ca2ea52,0x51c421e9
DD 0xbf73d5d4,0x62151e6f,0x00523453,0xdd34ffe8
DD 0xc4dc602b,0x19baab90,0x7bfd81ac,0xa69b4a17
DD 0x70372bcc,0xad51e077,0xcf16ca4b,0x127001f0
DD 0x0b989e33,0xd6fe5588,0xb4b97fb4,0x69dfb40f
DD 0x87684032,0x5a0e8b89,0x3849a1b5,0xe52f6a0e
DD 0xfcc7f5cd,0x21a13e76,0x43e6144a,0x9e80dff1
DD 0x9b658ac1,0x4603417a,0x24446b46,0xf922a0fd
DD 0xe0ca3f3e,0x3dacf485,0x5febdeb9,0x828d1502
DD 0x6c3ae13f,0xb15c2a84,0xd31b00b8,0x0e7dcb03
DD 0x179554c0,0xcaf39f7b,0xa8b4b547,0x75d27efc
DD 0x431048bf,0x9e768304,0xfc31a938,0x21576283
DD 0x38bffd40,0xe5d936fb,0x879e1cc7,0x5af8d77c
DD 0xb44f2341,0x6929e8fa,0x0b6ec2c6,0xd608097d
DD 0xcfe096be,0x12865d05,0x70c17739,0xada7bc82
DD 0xa842e9b2,0x75242209,0x17630835,0xca05c38e
DD 0xd3ed5c4d,0x0e8b97f6,0x6cccbdca,0xb1aa7671
DD 0x5f1d824c,0x827b49f7,0xe03c63cb,0x3d5aa870
DD 0x24b237b3,0xf9d4fc08,0x9b93d634,0x46f51d8f
DD 0x90597c54,0x4d3fb7ef,0x2f789dd3,0xf21e5668
DD 0xebf6c9ab,0x36900210,0x54d7282c,0x89b1e397
DD 0x670617aa,0xba60dc11,0xd827f62d,0x05413d96
DD 0x1ca9a255,0xc1cf69ee,0xa38843d2,0x7eee8869
DD 0x7b0bdd59,0xa66d16e2,0xc42a3cde,0x194cf765
DD 0x00a468a6,0xddc2a31d,0xbf858921,0x62e3429a
DD 0x8c54b6a7,0x51327d1c,0x33755720,0xee139c9b
DD 0xf7fb0358,0x2a9dc8e3,0x48dae2df,0x95bc2964
DD 0xe06e5798,0x3d089c23,0x5f4fb61f,0x82297da4
DD 0x9bc1e267,0x46a729dc,0x24e003e0,0xf986c85b
DD 0x17313c66,0xca57f7dd,0xa810dde1,0x7576165a
DD 0x6c9e8999,0xb1f84222,0xd3bf681e,0x0ed9a3a5
DD 0x0b3cf695,0xd65a3d2e,0xb41d1712,0x697bdca9
DD 0x7093436a,0xadf588d1,0xcfb2a2ed,0x12d46956
DD 0xfc639d6b,0x210556d0,0x43427cec,0x9e24b757
DD 0x87cc2894,0x5aaae32f,0x38edc913,0xe58b02a8
DD 0x33276373,0xee41a8c8,0x8c0682f4,0x5160494f
DD 0x4888d68c,0x95ee1d37,0xf7a9370b,0x2acffcb0
DD 0xc478088d,0x191ec336,0x7b59e90a,0xa63f22b1
DD 0xbfd7bd72,0x62b176c9,0x00f65cf5,0xdd90974e
DD 0xd875c27e,0x051309c5,0x675423f9,0xba32e842
DD 0xa3da7781,0x7ebcbc3a,0x1cfb9606,0xc19d5dbd
DD 0x2f2aa980,0xf24c623b,0x900b4807,0x4d6d83bc
DD 0x54851c7f,0x89e3d7c4,0xeba4fdf8,0x36c23643
