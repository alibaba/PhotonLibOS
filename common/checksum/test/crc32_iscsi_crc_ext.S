/**********************************************************************
  Copyright(c) 2020 Arm Corporation All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the
      distribution.
    * Neither the name of Arm Corporation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**********************************************************************/

	.text
	.align		4
	.arch		armv8-a+crc


; #include "crc32_aarch64_common.h"
// #include "aarch64_label.h"
#ifndef __AARCH64_LABEL_H__
#define __AARCH64_LABEL_H__

#ifdef __USER_LABEL_PREFIX__
#define CONCAT1(a, b) CONCAT2(a, b)
#define CONCAT2(a, b) a ## b
#define cdecl(x) CONCAT1 (__USER_LABEL_PREFIX__, x)
#else
#define cdecl(x) x
#endif

#ifdef __APPLE__
#define ASM_DEF_RODATA .section	__TEXT,__const
#else
#define ASM_DEF_RODATA .section .rodata
#endif

#endif


.macro	crc32_hw_common		poly_type

.ifc	\poly_type,crc32
	mvn		wCRC,wCRC
.endif
	cbz		LEN, .zero_length_ret
	tbz		BUF, 0, .align_short
	ldrb		wdata,[BUF],1
	sub		LEN,LEN,1
	crc32_u8	wCRC,wCRC,wdata
.align_short:
	tst		BUF,2
	ccmp		LEN,1,0,ne
	bhi		.align_short_2
	tst		BUF,4
	ccmp		LEN,3,0,ne
	bhi		.align_word

.align_finish:

	cmp		LEN, 63
	bls		.loop_16B
.loop_64B:
	ldp		data0, data1, [BUF],#16
	prfm		pldl2keep,[BUF,2048]
	sub		LEN,LEN,#64
	ldp		data2, data3, [BUF],#16
	prfm		pldl1keep,[BUF,256]
	cmp		LEN,#64
	crc32_u64	wCRC, wCRC, data0
	crc32_u64	wCRC, wCRC, data1
	ldp		data0, data1, [BUF],#16
	crc32_u64	wCRC, wCRC, data2
	crc32_u64	wCRC, wCRC, data3
	ldp		data2, data3, [BUF],#16
	crc32_u64	wCRC, wCRC, data0
	crc32_u64	wCRC, wCRC, data1
	crc32_u64	wCRC, wCRC, data2
	crc32_u64	wCRC, wCRC, data3
	bge		.loop_64B

.loop_16B:
	cmp		LEN, 15
	bls		.less_16B
	ldp		data0, data1, [BUF],#16
	sub		LEN,LEN,#16
	cmp		LEN,15
	crc32_u64	wCRC, wCRC, data0
	crc32_u64	wCRC, wCRC, data1
	bls		.less_16B
	ldp		data0, data1, [BUF],#16
	sub		LEN,LEN,#16
	cmp		LEN,15
	crc32_u64	wCRC, wCRC, data0
	crc32_u64	wCRC, wCRC, data1
	bls		.less_16B
	ldp		data0, data1, [BUF],#16
	sub		LEN,LEN,#16   //MUST less than 16B
	crc32_u64	wCRC, wCRC, data0
	crc32_u64	wCRC, wCRC, data1
.less_16B:
	cmp		LEN, 7
	bls		.less_8B
	ldr		data0, [BUF], 8
	sub		LEN, LEN, #8
	crc32_u64	wCRC, wCRC, data0
.less_8B:
	cmp		LEN, 3
	bls		.less_4B
	ldr		wdata, [BUF], 4
	sub		LEN, LEN, #4
	crc32_u32	wCRC, wCRC, wdata
.less_4B:
	cmp		LEN, 1
	bls		.less_2B
	ldrh		wdata, [BUF], 2
	sub		LEN, LEN, #2
	crc32_u16	wCRC, wCRC, wdata
.less_2B:
	cbz		LEN, .zero_length_ret
	ldrb		wdata, [BUF]
	crc32_u8	wCRC, wCRC, wdata
.zero_length_ret:
.ifc	\poly_type,crc32
	mvn		w0, wCRC
.else
	mov		w0, wCRC
.endif
	ret
.align_short_2:
	ldrh		wdata, [BUF], 2
	sub		LEN, LEN, 2
	tst		BUF, 4
	crc32_u16	wCRC, wCRC, wdata
	ccmp		LEN, 3, 0, ne
	bls		.align_finish
.align_word:
	ldr		wdata, [BUF], 4
	sub		LEN, LEN, #4
	crc32_u32	wCRC, wCRC, wdata
	b .align_finish
.endm


	BUF		.req	x0
	LEN		.req	x1
	wCRC		.req	w2
	data0		.req	x4
	data1		.req	x5
	data2		.req	x6
	data3		.req	x7
	wdata		.req	w3
.macro	crc32_u64	dst,src,data
	crc32cx		\dst,\src,\data
.endm
.macro	crc32_u32	dst,src,data
	crc32cw		\dst,\src,\data
.endm
.macro	crc32_u16	dst,src,data
	crc32ch		\dst,\src,\data
.endm
.macro	crc32_u8	dst,src,data
	crc32cb		\dst,\src,\data
.endm

	/**
	 * uint32_t crc32_iscsi_crc_ext(const unsigned char *BUF,
	 *                  uint64_t LEN,uint32_t wCRC);
	 */
  .global	cdecl(crc32_iscsi_crc_ext)
#ifndef __APPLE__
	.type	crc32_iscsi_crc_ext, %function
#endif
cdecl(crc32_iscsi_crc_ext):
	crc32_hw_common	crc32c
#ifndef __APPLE__
	.size	crc32_iscsi_crc_ext, .-crc32_iscsi_crc_ext
#endif
