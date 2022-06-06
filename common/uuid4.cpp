/*
Copyright 2022 The Photon Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

/**
 * Copyright (c) 2018 rxi
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the MIT license. See LICENSE for details.
 */

#include <cstdio>
#include <cstdint>
#include <string.h>
#include <sys/time.h>
#include <cstdlib>
#include "uuid4.h"


static uint64_t seed[2]{};


static uint64_t xorshift128plus(uint64_t *s)
{
    /* http://xorshift.di.unimi.it/xorshift128plus.c */
    uint64_t s1 = s[0];
    const uint64_t s0 = s[1];
    s[0] = s0;
    s1 ^= s1 << 23;
    s[1] = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5);
    return s[1] + s0;
}

int uuid4_init(void)
{
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)

    int res;
    FILE *fp = fopen("/dev/urandom", "rb");
    if (!fp)
    {
        return UUID4_EFAILURE;
    }
    res = fread(seed, 1, sizeof(seed), fp);
    fclose(fp);
    if (res != sizeof(seed))
    {
        return UUID4_EFAILURE;
    }
#else
#error "unsupported platform"
#endif
    return UUID4_ESUCCESS;
}

static const char *hex_words = "0123456789ABCDEF";

void uuid4_string_generate(char *dst);

void uuid4_generate(uuid4_t uu)
{
    uuid4_string_t tmp;
    uuid4_string_generate((char *)tmp);
    uuid4_parse((char *)tmp, uu);
}

void uuid4_clear(uuid4_t uu)
{
    memset(uu, 0, 16);
}

int uuid4_is_null(uuid4_t uu)
{
    for (int i = 0; i < 16; i++)
        if (uu[i] != 0)
            return 0;
    return 1;
}

void uuid4_unparse_upper(uuid4_t uu, uuid4_string_t out)
{
// if (uuid4_is_null(uu)) return;
    int p = 0;
    for (int i = 0; i < 16; i++){
        uint8_t x = uu[i];
        char l = hex_words[x & 0xf];
        char u = hex_words[x >> 4];
        out[p++] = u;
        out[p++] = l;
        if (p == 8 || p == 13 || p == 18 || p == 23) out[p++] = '-';
    }
    out[36] = '\0';
    return;
}

uint8_t get_val(const char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;

    return -1;
}

int invalid_uuid4(char *str_uu)
{
    if (strlen(str_uu) != 36) {
        return -1;
    }
    if (str_uu[36] != '\0') return -1;
    for (int i= 0; i < 36; i++) {
        if ((i == 8 || i == 13 || i == 18 || i == 23) && str_uu[i] == '-' )
            continue;
        if (str_uu[i] >= '0' && str_uu[i] <= '9') continue;
        if (str_uu[i] >= 'A' && str_uu[i] <= 'F') continue;
        if (str_uu[i] >= 'a' && str_uu[i] <= 'f') continue;
        return -1;
    }
    return 0;
}

int uuid4_compare(uuid4_t uu1, uuid4_t uu2)
{
    return memcmp(uu1, uu2, 16);
}

void uuid4_copy(uuid4_t dst, uuid4_t src)
{
    memcpy(dst, src, 16);
}

int uuid4_parse(char *in, uuid4_t out)
{
    if (invalid_uuid4(in) != 0) return -1;
    //unsigned char uu[16];
    uuid4_t uu;
    memset(uu, 0, 16);
    int p = 0;
    for (int i = 0; i < 36; i+=2) {
        if (i == 8 || i == 13 || i == 18 || i == 23) i++;
        uint8_t u = get_val(in[i]);
        uint8_t l = get_val(in[i + 1]);
        uu[p++] = (u << 4) + l;
        // printf("[%d %c%c:%u %u %u]\n", i, in[i], in[i+1], u, l, uu[p-1]);
    }
    // printf("\n");
    memcpy(out, uu, 16);

    // printf("done.\n");
    return 0;
}

void uuid4_string_generate(char *dst)
{
    static int init_flg = 0;
    if (init_flg == 0) {
        if (uuid4_init() != 0) printf("uuid init error.\n");
        init_flg = 1;
    }
    static const char *uuid4_template = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
    static const char *chars = "0123456789abcdef";
    union {
        unsigned char b[16];
        uint64_t word[2];
    } s;
    const char *p;
    int i, n;
    /* get random */
    s.word[0] = xorshift128plus(seed);
    s.word[1] = xorshift128plus(seed);
    /* build string */
    p = uuid4_template;
    i = 0;
    while (*p)
    {
        n = s.b[i >> 1];
        n = (i & 1) ? (n >> 4) : (n & 0xf);
        switch (*p)
        {
        case 'x':
            *dst = chars[n];
            i++;
            break;
        case 'y':
            *dst = chars[(n & 0x3) + 8];
            i++;
            break;
        default:
            *dst = *p;
        }
        dst++, p++;
    }
    *dst = '\0';
}

