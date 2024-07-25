/*
    liburlmatch - a fast URL matcher
    Copyright (C) 2013 Lauri Kasanen

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, version 3 of the License.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef INTERNAL_H
#define INTERNAL_H

#define _GNU_SOURCE

#include "lrtypes.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

// Let's help the compiler
#if __GNUC__ >= 4

	#define PURE_FUNC __attribute__ ((pure))
	#define NORETURN_FUNC __attribute__ ((noreturn))
	#define CONST_FUNC __attribute__ ((const))
	#define WUR_FUNC __attribute__ ((warn_unused_result))
#else // GNUC

	#define PURE_FUNC
	#define NORETURN_FUNC
	#define CONST_FUNC
	#define WUR_FUNC

#endif // GNUC


#pragma GCC visibility push(hidden)

u32 countwilds(const char str[]) WUR_FUNC PURE_FUNC;
const char *strrstr(const char hay[], const char needle[]) WUR_FUNC PURE_FUNC;
static inline int suffixcmp(const char one[], const char two[]) WUR_FUNC PURE_FUNC;

void *xcalloc(size_t nmemb, size_t size);
void *xmalloc(size_t size);
void die(const char s[]) NORETURN_FUNC;
void swrite(const void *ptr, const size_t size, FILE *stream);
void sread(void *ptr, const size_t size, FILE *stream);
void getsuffix(const char str[], char suf[3]);
static inline int wildprefix(const char str[]) WUR_FUNC PURE_FUNC;


struct urlctx {
	struct prefix *pref;
	u16 count;

	char *storage;
	u32 storagelen;
	u32 used;
};

struct prefix {
	struct suffix *suf;
	u16 count;

	char prefix[6];
	u8 len;
};

struct suffix {
	struct needle *need;
	u16 count;

	char suffix[3];
};

struct needle {
	const char *needle;
	u16 len;
	u16 wilds;
	u16 longest;
	u16 longlen;
};

#define MAGIC "um1"

void printctx(const struct urlctx *);
int ctxcmp(const struct urlctx *, const struct urlctx *);
void *poolalloc(struct urlctx *, u32 bytes) WUR_FUNC;

// Inlines

static inline int wildprefix(const char str[]) {

	u16 len = strlen(str);
	if (len > 5)
		len = 5;

	return memchr(str, '*', len) != NULL;
}

static inline int suffixcmp(const char one[], const char two[]) {

	const u16 len1 = one[1] ? 2 : 1;
	const u16 len2 = two[1] ? 2 : 1;

	if (len1 == len2) {
		if (len1 == 1)
			return one[0] != two[0];
		return one[1] != two[1] || one[0] != two[0];
	}

	if (len1 < len2) {
		// one is a single byte long
		if (one[0] == two[1])
			return 0;
		return 1;
	} else {
		// two is a single byte long
		if (one[1] == two[0])
			return 0;
		return 1;
	}
}

#pragma GCC visibility pop

#endif
