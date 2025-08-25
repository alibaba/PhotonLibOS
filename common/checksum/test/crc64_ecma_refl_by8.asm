;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;  Copyright(c) 2011-2016 Intel Corporation All rights reserved.
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

%define FUNCTION_NAME crc64_ecma_refl_by8
%define USE_CONSTS
%macro INCLUDE_CONSTS 0
rk1 :
DQ 0xdabe95afc7875f40
rk2 :
DQ 0xe05dd497ca393ae4
rk3 :
DQ 0xd7d86b2af73de740
rk4 :
DQ 0x8757d71d4fcc1000
rk5 :
DQ 0xdabe95afc7875f40
rk6 :
DQ 0x0000000000000000
rk7 :
DQ 0x9c3e466c172963d5
rk8 :
DQ 0x92d8af2baf0e1e84
rk9 :
DQ 0x947874de595052cb
rk10 :
DQ 0x9e735cb59b4724da
rk11 :
DQ 0xe4ce2cd55fea0037
rk12 :
DQ 0x2fe3fd2920ce82ec
rk13 :
DQ 0xe31d519421a63a5
rk14 :
DQ 0x2e30203212cac325
rk15 :
DQ 0x81f6054a7842df4
rk16 :
DQ 0x6ae3efbb9dd441f3
rk17 :
DQ 0x69a35d91c3730254
rk18 :
DQ 0xb5ea1af9c013aca4
rk19 :
DQ 0x3be653a30fe1af51
rk20 :
DQ 0x60095b008a9efa44
%endm

; %include "crc64_iso_refl_by8.asm"
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

%ifndef FUNCTION_NAME
%define FUNCTION_NAME crc64_iso_refl_by8
%endif

%define	fetch_dist	1024

[bits 64]
default rel

section .text


%ifidn __OUTPUT_FORMAT__, win64
        %xdefine        arg1 rcx
        %xdefine        arg2 rdx
        %xdefine        arg3 r8
%else
        %xdefine        arg1 rdi
        %xdefine        arg2 rsi
        %xdefine        arg3 rdx
%endif

%define TMP 16*0
%ifidn __OUTPUT_FORMAT__, win64
        %define XMM_SAVE 16*2
        %define VARIABLE_OFFSET 16*10+8
%else
        %define VARIABLE_OFFSET 16*2+8
%endif


align 16
mk_global 	FUNCTION_NAME, function
FUNCTION_NAME:
	endbranch
        ; uint64_t c = crc ^ 0xffffffff,ffffffffL;
	not arg1
        sub     rsp, VARIABLE_OFFSET

%ifidn __OUTPUT_FORMAT__, win64
        ; push the xmm registers into the stack to maintain
        movdqa  [rsp + XMM_SAVE + 16*0], xmm6
        movdqa  [rsp + XMM_SAVE + 16*1], xmm7
        movdqa  [rsp + XMM_SAVE + 16*2], xmm8
        movdqa  [rsp + XMM_SAVE + 16*3], xmm9
        movdqa  [rsp + XMM_SAVE + 16*4], xmm10
        movdqa  [rsp + XMM_SAVE + 16*5], xmm11
        movdqa  [rsp + XMM_SAVE + 16*6], xmm12
        movdqa  [rsp + XMM_SAVE + 16*7], xmm13
%endif

        ; check if smaller than 256B
        cmp     arg3, 256

        ; for sizes less than 256, we can't fold 128B at a time...
        jl      _less_than_256


        ; load the initial crc value
        movq    xmm10, arg1      ; initial crc
      ; receive the initial 128B data, xor the initial crc value
        movdqu  xmm0, [arg2+16*0]
        movdqu  xmm1, [arg2+16*1]
        movdqu  xmm2, [arg2+16*2]
        movdqu  xmm3, [arg2+16*3]
        movdqu  xmm4, [arg2+16*4]
        movdqu  xmm5, [arg2+16*5]
        movdqu  xmm6, [arg2+16*6]
        movdqu  xmm7, [arg2+16*7]

        ; XOR the initial_crc value
        pxor    xmm0, xmm10
        movdqa  xmm10, [rk3]    ;xmm10 has rk3 and rk4
                                        ;imm value of pclmulqdq instruction will determine which constant to use
        ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
        ; we subtract 256 instead of 128 to save one instruction from the loop
        sub     arg3, 256

        ; at this section of the code, there is 128*x+y (0<=y<128) bytes of buffer. The _fold_128_B_loop
        ; loop will fold 128B at a time until we have 128+y Bytes of buffer


        ; fold 128B at a time. This section of the code folds 8 xmm registers in parallel
_fold_128_B_loop:

        ; update the buffer pointer
        add     arg2, 128

	prefetchnta [arg2+fetch_dist+0]
        movdqu  xmm9, [arg2+16*0]
        movdqu  xmm12, [arg2+16*1]
        movdqa  xmm8, xmm0
        movdqa  xmm13, xmm1
        pclmulqdq       xmm0, xmm10, 0x10
        pclmulqdq       xmm8, xmm10 , 0x1
        pclmulqdq       xmm1, xmm10, 0x10
        pclmulqdq       xmm13, xmm10 , 0x1
        pxor    xmm0, xmm9
        xorps   xmm0, xmm8
        pxor    xmm1, xmm12
        xorps   xmm1, xmm13

	prefetchnta [arg2+fetch_dist+32]
        movdqu  xmm9, [arg2+16*2]
        movdqu  xmm12, [arg2+16*3]
        movdqa  xmm8, xmm2
        movdqa  xmm13, xmm3
        pclmulqdq       xmm2, xmm10, 0x10
        pclmulqdq       xmm8, xmm10 , 0x1
        pclmulqdq       xmm3, xmm10, 0x10
        pclmulqdq       xmm13, xmm10 , 0x1
        pxor    xmm2, xmm9
        xorps   xmm2, xmm8
        pxor    xmm3, xmm12
        xorps   xmm3, xmm13

	prefetchnta [arg2+fetch_dist+64]
        movdqu  xmm9, [arg2+16*4]
        movdqu  xmm12, [arg2+16*5]
        movdqa  xmm8, xmm4
        movdqa  xmm13, xmm5
        pclmulqdq       xmm4, xmm10, 0x10
        pclmulqdq       xmm8, xmm10 , 0x1
        pclmulqdq       xmm5, xmm10, 0x10
        pclmulqdq       xmm13, xmm10 , 0x1
        pxor    xmm4, xmm9
        xorps   xmm4, xmm8
        pxor    xmm5, xmm12
        xorps   xmm5, xmm13

	prefetchnta [arg2+fetch_dist+96]
        movdqu  xmm9, [arg2+16*6]
        movdqu  xmm12, [arg2+16*7]
        movdqa  xmm8, xmm6
        movdqa  xmm13, xmm7
        pclmulqdq       xmm6, xmm10, 0x10
        pclmulqdq       xmm8, xmm10 , 0x1
        pclmulqdq       xmm7, xmm10, 0x10
        pclmulqdq       xmm13, xmm10 , 0x1
        pxor    xmm6, xmm9
        xorps   xmm6, xmm8
        pxor    xmm7, xmm12
        xorps   xmm7, xmm13

        sub     arg3, 128

        ; check if there is another 128B in the buffer to be able to fold
        jge     _fold_128_B_loop
        ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

        add     arg2, 128
        ; at this point, the buffer pointer is pointing at the last y Bytes of the buffer, where 0 <= y < 128
        ; the 128B of folded data is in 8 of the xmm registers: xmm0, xmm1, xmm2, xmm3, xmm4, xmm5, xmm6, xmm7


        ; fold the 8 xmm registers to 1 xmm register with different constants
	; xmm0 to xmm7
        movdqa  xmm10, [rk9]
        movdqa  xmm8, xmm0
        pclmulqdq       xmm0, xmm10, 0x1
        pclmulqdq       xmm8, xmm10, 0x10
        pxor    xmm7, xmm8
        xorps   xmm7, xmm0
        ;xmm1 to xmm7
        movdqa  xmm10, [rk11]
        movdqa  xmm8, xmm1
        pclmulqdq       xmm1, xmm10, 0x1
        pclmulqdq       xmm8, xmm10, 0x10
        pxor    xmm7, xmm8
        xorps   xmm7, xmm1

        movdqa  xmm10, [rk13]
        movdqa  xmm8, xmm2
        pclmulqdq       xmm2, xmm10, 0x1
        pclmulqdq       xmm8, xmm10, 0x10
        pxor    xmm7, xmm8
        pxor    xmm7, xmm2

        movdqa  xmm10, [rk15]
        movdqa  xmm8, xmm3
        pclmulqdq       xmm3, xmm10, 0x1
        pclmulqdq       xmm8, xmm10, 0x10
        pxor    xmm7, xmm8
        xorps   xmm7, xmm3

        movdqa  xmm10, [rk17]
        movdqa  xmm8, xmm4
        pclmulqdq       xmm4, xmm10, 0x1
        pclmulqdq       xmm8, xmm10, 0x10
        pxor    xmm7, xmm8
        pxor    xmm7, xmm4

        movdqa  xmm10, [rk19]
        movdqa  xmm8, xmm5
        pclmulqdq       xmm5, xmm10, 0x1
        pclmulqdq       xmm8, xmm10, 0x10
        pxor    xmm7, xmm8
        xorps   xmm7, xmm5
	; xmm6 to xmm7
        movdqa  xmm10, [rk1]
        movdqa  xmm8, xmm6
        pclmulqdq       xmm6, xmm10, 0x1
        pclmulqdq       xmm8, xmm10, 0x10
        pxor    xmm7, xmm8
        pxor    xmm7, xmm6


        ; instead of 128, we add 128-16 to the loop counter to save 1 instruction from the loop
        ; instead of a cmp instruction, we use the negative flag with the jl instruction
        add     arg3, 128-16
        jl      _final_reduction_for_128

        ; now we have 16+y bytes left to reduce. 16 Bytes is in register xmm7 and the rest is in memory
        ; we can fold 16 bytes at a time if y>=16
        ; continue folding 16B at a time

_16B_reduction_loop:
        movdqa  xmm8, xmm7
        pclmulqdq       xmm7, xmm10, 0x1
        pclmulqdq       xmm8, xmm10, 0x10
        pxor    xmm7, xmm8
        movdqu  xmm0, [arg2]
        pxor    xmm7, xmm0
        add     arg2, 16
        sub     arg3, 16
        ; instead of a cmp instruction, we utilize the flags with the jge instruction
        ; equivalent of: cmp arg3, 16-16
        ; check if there is any more 16B in the buffer to be able to fold
        jge     _16B_reduction_loop

        ;now we have 16+z bytes left to reduce, where 0<= z < 16.
        ;first, we reduce the data in the xmm7 register


_final_reduction_for_128:
        add arg3, 16
        je _128_done
  ; here we are getting data that is less than 16 bytes.
        ; since we know that there was data before the pointer, we can offset the input pointer before the actual point, to receive exactly 16 bytes.
        ; after that the registers need to be adjusted.
_get_last_two_xmms:


        movdqa xmm2, xmm7
        movdqu xmm1, [arg2 - 16 + arg3]

        ; get rid of the extra data that was loaded before
        ; load the shift constant
        lea     rax, [pshufb_shf_table]
        add     rax, arg3
        movdqu  xmm0, [rax]


        pshufb  xmm7, xmm0
        pxor    xmm0, [mask3]
        pshufb  xmm2, xmm0

        pblendvb        xmm2, xmm1     ;xmm0 is implicit
        ;;;;;;;;;;
        movdqa  xmm8, xmm7
        pclmulqdq       xmm7, xmm10, 0x1

        pclmulqdq       xmm8, xmm10, 0x10
        pxor    xmm7, xmm8
        pxor    xmm7, xmm2

_128_done:
        ; compute crc of a 128-bit value
        movdqa  xmm10, [rk5]
        movdqa  xmm0, xmm7

        ;64b fold
        pclmulqdq       xmm7, xmm10, 0
        psrldq  xmm0, 8
        pxor    xmm7, xmm0

        ;barrett reduction
_barrett:
        movdqa  xmm1, xmm7
        movdqa  xmm10, [rk7]

        pclmulqdq       xmm7, xmm10, 0
        movdqa  xmm2, xmm7
        pclmulqdq       xmm7, xmm10, 0x10
        pslldq  xmm2, 8
        pxor    xmm7, xmm2
        pxor    xmm7, xmm1
        pextrq  rax, xmm7, 1

_cleanup:
        ; return c ^ 0xffffffff, ffffffffL;
        not     rax


%ifidn __OUTPUT_FORMAT__, win64
        movdqa  xmm6, [rsp + XMM_SAVE + 16*0]
        movdqa  xmm7, [rsp + XMM_SAVE + 16*1]
        movdqa  xmm8, [rsp + XMM_SAVE + 16*2]
        movdqa  xmm9, [rsp + XMM_SAVE + 16*3]
        movdqa  xmm10, [rsp + XMM_SAVE + 16*4]
        movdqa  xmm11, [rsp + XMM_SAVE + 16*5]
        movdqa  xmm12, [rsp + XMM_SAVE + 16*6]
        movdqa  xmm13, [rsp + XMM_SAVE + 16*7]
%endif
        add     rsp, VARIABLE_OFFSET
        ret

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

align 16
_less_than_256:

        ; check if there is enough buffer to be able to fold 16B at a time
        cmp     arg3, 32
        jl      _less_than_32

        ; if there is, load the constants
        movdqa  xmm10, [rk1]    ; rk1 and rk2 in xmm10

        movq    xmm0, arg1       ; get the initial crc value
        movdqu  xmm7, [arg2]            ; load the plaintext
        pxor    xmm7, xmm0

        ; update the buffer pointer
        add     arg2, 16

        ; update the counter. subtract 32 instead of 16 to save one instruction from the loop
        sub     arg3, 32

        jmp     _16B_reduction_loop

align 16
_less_than_32:
        ; mov initial crc to the return value. this is necessary for zero-length buffers.
        mov     rax, arg1
        test    arg3, arg3
        je      _cleanup

        movq    xmm0, arg1       ; get the initial crc value

        cmp     arg3, 16
        je      _exact_16_left
        jl      _less_than_16_left

        movdqu  xmm7, [arg2]            ; load the plaintext
        pxor    xmm7, xmm0              ; xor the initial crc value
        add     arg2, 16
        sub     arg3, 16
        movdqa  xmm10, [rk1]    ; rk1 and rk2 in xmm10
        jmp     _get_last_two_xmms


align 16
_less_than_16_left:
        ; use stack space to load data less than 16 bytes, zero-out the 16B in memory first.

        pxor    xmm1, xmm1
        mov     r11, rsp
        movdqa  [r11], xmm1

        ;       backup the counter value
        mov     r9, arg3
        cmp     arg3, 8
        jl      _less_than_8_left

        ; load 8 Bytes
        mov     rax, [arg2]
        mov     [r11], rax
        add     r11, 8
        sub     arg3, 8
        add     arg2, 8
_less_than_8_left:

        cmp     arg3, 4
        jl      _less_than_4_left

        ; load 4 Bytes
        mov     eax, [arg2]
        mov     [r11], eax
        add     r11, 4
        sub     arg3, 4
        add     arg2, 4
_less_than_4_left:

        cmp     arg3, 2
        jl      _less_than_2_left

        ; load 2 Bytes
        mov     ax, [arg2]
        mov     [r11], ax
        add     r11, 2
        sub     arg3, 2
        add     arg2, 2
_less_than_2_left:
        cmp     arg3, 1
        jl      _zero_left

        ; load 1 Byte
        mov     al, [arg2]
        mov     [r11], al

_zero_left:
        movdqa  xmm7, [rsp]
        pxor    xmm7, xmm0      ; xor the initial crc value

        lea rax,[pshufb_shf_table]

	cmp     r9, 8
        jl      _end_1to7

_end_8to15:
        movdqu  xmm0, [rax + r9]
        pshufb  xmm7,xmm0
        jmp     _128_done

_end_1to7:
	; Left shift (8-length) bytes in XMM
        movdqu  xmm0, [rax + r9 + 8]
        pshufb  xmm7,xmm0

        jmp     _barrett

align 16
_exact_16_left:
        movdqu  xmm7, [arg2]
        pxor    xmm7, xmm0      ; xor the initial crc value

        jmp     _128_done

section .data

; precomputed constants
align 16
; rk7 = floor(2^128/Q)
; rk8 = Q
%ifndef USE_CONSTS
rk1:
DQ 0xf500000000000001
rk2:
DQ 0x6b70000000000001
rk3:
DQ 0xb001000000010000
rk4:
DQ 0xf501b0000001b000
rk5:
DQ 0xf500000000000001
rk6:
DQ 0x0000000000000000
rk7:
DQ 0xb000000000000001
rk8:
DQ 0xb000000000000000
rk9:
DQ 0xe014514514501501
rk10:
DQ 0x771db6db6db71c71
rk11:
DQ 0xa101101101110001
rk12:
DQ 0x1ab1ab1ab1aab001
rk13:
DQ 0xf445014445000001
rk14:
DQ 0x6aab71daab700001
rk15:
DQ 0xb100010100000001
rk16:
DQ 0x01b001b1b0000001
rk17:
DQ 0xe145150000000001
rk18:
DQ 0x76db6c7000000001
rk19:
DQ 0xa011000000000001
rk20:
DQ 0x1b1ab00000000001
%else
INCLUDE_CONSTS
%endif

pshufb_shf_table:
; use these values for shift constants for the pshufb instruction
; different alignments result in values as shown:
;       dq 0x8887868584838281, 0x008f8e8d8c8b8a89 ; shl 15 (16-1) / shr1
;       dq 0x8988878685848382, 0x01008f8e8d8c8b8a ; shl 14 (16-3) / shr2
;       dq 0x8a89888786858483, 0x0201008f8e8d8c8b ; shl 13 (16-4) / shr3
;       dq 0x8b8a898887868584, 0x030201008f8e8d8c ; shl 12 (16-4) / shr4
;       dq 0x8c8b8a8988878685, 0x04030201008f8e8d ; shl 11 (16-5) / shr5
;       dq 0x8d8c8b8a89888786, 0x0504030201008f8e ; shl 10 (16-6) / shr6
;       dq 0x8e8d8c8b8a898887, 0x060504030201008f ; shl 9  (16-7) / shr7
;       dq 0x8f8e8d8c8b8a8988, 0x0706050403020100 ; shl 8  (16-8) / shr8
;       dq 0x008f8e8d8c8b8a89, 0x0807060504030201 ; shl 7  (16-9) / shr9
;       dq 0x01008f8e8d8c8b8a, 0x0908070605040302 ; shl 6  (16-10) / shr10
;       dq 0x0201008f8e8d8c8b, 0x0a09080706050403 ; shl 5  (16-11) / shr11
;       dq 0x030201008f8e8d8c, 0x0b0a090807060504 ; shl 4  (16-12) / shr12
;       dq 0x04030201008f8e8d, 0x0c0b0a0908070605 ; shl 3  (16-13) / shr13
;       dq 0x0504030201008f8e, 0x0d0c0b0a09080706 ; shl 2  (16-14) / shr14
;       dq 0x060504030201008f, 0x0e0d0c0b0a090807 ; shl 1  (16-15) / shr15
dq 0x8786858483828100, 0x8f8e8d8c8b8a8988
dq 0x0706050403020100, 0x000e0d0c0b0a0908


mask:
dq     0xFFFFFFFFFFFFFFFF, 0x0000000000000000
mask2:
dq     0xFFFFFFFF00000000, 0xFFFFFFFFFFFFFFFF
mask3:
dq     0x8080808080808080, 0x8080808080808080
