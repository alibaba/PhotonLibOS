/* crc64.c -- compute CRC-64
 * Copyright (C) 2013 Mark Adler
 * Version 1.4  16 Dec 2013  Mark Adler
 */

/*
  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the author be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Mark Adler
  madler@alumni.caltech.edu
 */

/* Compute CRC-64 in the manner of xz, using the ECMA-182 polynomial,
   bit-reversed, with one's complement pre and post processing.  Provide a
   means to combine separately computed CRC-64's. */

/* Version history:
   1.0  13 Dec 2013  First version
   1.1  13 Dec 2013  Fix comments in test code
   1.2  14 Dec 2013  Determine endianess at run time
   1.3  15 Dec 2013  Add eight-byte processing for big endian as well
                     Make use of the pthread library optional
   1.4  16 Dec 2013  Make once variable volatile for limited thread protection
 */

#include <stdio.h>
#include <inttypes.h>
#include <assert.h>

/* The include of pthread.h below can be commented out in order to not use the
   pthread library for table initialization.  In that case, the initialization
   will not be thread-safe.  That's fine, so long as it can be assured that
   there is only one thread using crc64(). */
#include <pthread.h>            /* link with -lpthread */

/* 64-bit CRC polynomial with these coefficients, but reversed:
    64, 62, 57, 55, 54, 53, 52, 47, 46, 45, 40, 39, 38, 37, 35, 33, 32,
    31, 29, 27, 24, 23, 22, 21, 19, 17, 13, 12, 10, 9, 7, 4, 1, 0 */
#define POLY UINT64_C(0xc96c5795d7870f42)

/* Tables for CRC calculation -- filled in by initialization functions that are
   called once.  These could be replaced by constant tables generated in the
   same way.  There are two tables, one for each endianess.  Since these are
   static, i.e. local, one should be compiled out of existence if the compiler
   can evaluate the endianess check in crc64() at compile time. */
#include <memory>
using Table = std::tuple<uint64_t[8][256]>;
static std::unique_ptr<Table> crc64_table;
#define crc64_little_table std::get<0>(*crc64_table)
#define crc64_big_table std::get<0>(*crc64_table)

/* Fill in the CRC-64 constants table. */
static void crc64_init(uint64_t [][256])
{
    unsigned n, k;
    uint64_t crc;

    crc64_table.reset(new Table);
    auto& table = std::get<0>(*crc64_table);
    /* generate CRC-64's for all single byte sequences */
    for (n = 0; n < 256; n++) {
        crc = n;
        for (k = 0; k < 8; k++)
            crc = crc & 1 ? POLY ^ (crc >> 1) : crc >> 1;
        table[0][n] = crc;
    }

    /* generate CRC-64's for those followed by 1 to 7 zeros */
    for (n = 0; n < 256; n++) {
        crc = table[0][n];
        for (k = 1; k < 8; k++) {
            crc = table[0][crc & 0xff] ^ (crc >> 8);
            table[k][n] = crc;
        }
    }
}

/* This function is called once to initialize the CRC-64 table for use on a
   little-endian architecture. */
static void crc64_little_init(void)
{
    crc64_init(crc64_little_table);
}

/* Reverse the bytes in a 64-bit word. */
static inline uint64_t rev8(uint64_t a)
{
    uint64_t m;

    m = UINT64_C(0xff00ff00ff00ff);
    a = ((a >> 8) & m) | (a & m) << 8;
    m = UINT64_C(0xffff0000ffff);
    a = ((a >> 16) & m) | (a & m) << 16;
    return a >> 32 | a << 32;
}

/* This function is called once to initialize the CRC-64 table for use on a
   big-endian architecture. */
static void crc64_big_init(void)
{
    unsigned k, n;

    crc64_init(crc64_big_table);
    for (k = 0; k < 8; k++)
        for (n = 0; n < 256; n++)
            crc64_big_table[k][n] = rev8(crc64_big_table[k][n]);
}

/* Run the init() function exactly once.  If pthread.h is not included, then
   this macro will use a simple static state variable for the purpose, which is
   not thread-safe.  The init function must be of the type void init(void). */
#ifdef PTHREAD_ONCE_INIT
#  define ONCE(init) \
    do { \
        static pthread_once_t once = PTHREAD_ONCE_INIT; \
        pthread_once(&once, init); \
    } while (0)
#else
#  define ONCE(init) \
    do { \
        static volatile int once = 1; \
        if (once) { \
            if (once++ == 1) { \
                init(); \
                once = 0; \
            } \
            else \
                while (once) \
                    ; \
        } \
    } while (0)
#endif

/* Calculate a CRC-64 eight bytes at a time on a little-endian architecture. */
static inline uint64_t crc64_little(uint64_t crc, const void *buf, size_t len)
{
    auto next = (unsigned char *)buf;

    ONCE(crc64_little_init);
    crc = ~crc;
    while (len && ((uintptr_t)next & 7) != 0) {
        crc = crc64_little_table[0][(crc ^ *next++) & 0xff] ^ (crc >> 8);
        len--;
    }
    while (len >= 8) {
        crc ^= *(uint64_t *)next;
        crc = crc64_little_table[7][crc & 0xff] ^
              crc64_little_table[6][(crc >> 8) & 0xff] ^
              crc64_little_table[5][(crc >> 16) & 0xff] ^
              crc64_little_table[4][(crc >> 24) & 0xff] ^
              crc64_little_table[3][(crc >> 32) & 0xff] ^
              crc64_little_table[2][(crc >> 40) & 0xff] ^
              crc64_little_table[1][(crc >> 48) & 0xff] ^
              crc64_little_table[0][crc >> 56];
        next += 8;
        len -= 8;
    }
    while (len) {
        crc = crc64_little_table[0][(crc ^ *next++) & 0xff] ^ (crc >> 8);
        len--;
    }
    return ~crc;
}

/* Calculate a CRC-64 eight bytes at a time on a big-endian architecture. */
static inline uint64_t crc64_big(uint64_t crc, const void *buf, size_t len)
{
    auto next = (unsigned char *)buf;

    ONCE(crc64_big_init);
    crc = ~rev8(crc);
    while (len && ((uintptr_t)next & 7) != 0) {
        crc = crc64_big_table[0][(crc >> 56) ^ *next++] ^ (crc << 8);
        len--;
    }
    while (len >= 8) {
        crc ^= *(uint64_t *)next;
        crc = crc64_big_table[0][crc & 0xff] ^
              crc64_big_table[1][(crc >> 8) & 0xff] ^
              crc64_big_table[2][(crc >> 16) & 0xff] ^
              crc64_big_table[3][(crc >> 24) & 0xff] ^
              crc64_big_table[4][(crc >> 32) & 0xff] ^
              crc64_big_table[5][(crc >> 40) & 0xff] ^
              crc64_big_table[6][(crc >> 48) & 0xff] ^
              crc64_big_table[7][crc >> 56];
        next += 8;
        len -= 8;
    }
    while (len) {
        crc = crc64_big_table[0][(crc >> 56) ^ *next++] ^ (crc << 8);
        len--;
    }
    return ~rev8(crc);
}

/* Return the CRC-64 of buf[0..len-1] with initial crc, processing eight bytes
   at a time.  This selects one of two routines depending on the endianess of
   the architecture.  A good optimizing compiler will determine the endianess
   at compile time if it can, and get rid of the unused code and table.  If the
   endianess can be changed at run time, then this code will handle that as
   well, initializing and using two tables, if called upon to do so. */
uint64_t crc64ecma_sw(const uint8_t *buf, size_t len, uint64_t crc) {
    uint64_t n = 1;
    return *(char *)&n ? crc64_little(crc, buf, len) :
                         crc64_big(crc, buf, len);
}

extern "C" uint64_t crc64_ecma_refl_pmull__(uint64_t seed, const uint8_t *buf, uint64_t len);

uint64_t crc64ecma_hw(const uint8_t *buf, size_t len, uint64_t crc) {
#ifdef __aarch64__
    return crc64_ecma_refl_pmull__(crc, buf, len);
#else
    // todo: use Intel ISA-L
    return crc64ecma_sw(buf, len, crc);
#endif
}
