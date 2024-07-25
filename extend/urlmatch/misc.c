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

#include "photon/extend/urlmatch/internal.h"

u32 countwilds(const char str[]) {

	u32 sum = 0;

	const char *ptr = str;
	for (; *ptr; ptr++) {
		if (*ptr == '*') sum++;
	}

	return sum;
}

const char *strrstr(const char hay[], const char needle[]) {

	const char *next;
	next = strstr(hay, needle);
	if (!next) return NULL;

	while (1) {
		const char *prev = next;
		next = strstr(next + 1, needle);

		if (!next) return prev;
	}
}

void *xcalloc(size_t nmemb, size_t size) {

	void *tmp = calloc(nmemb, size);
	if (!tmp) die("Out of memory");

	return tmp;
}

void *xmalloc(size_t size) {

	void *tmp = malloc(size);
	if (!tmp) die("Out of memory");

	return tmp;
}

void die(const char s[]) {

	fprintf(stderr, "%s\n", s);
	exit(1);
}

void swrite(const void * const ptr, const size_t size, FILE * const stream) {

	const size_t ret = fwrite(ptr, size, 1, stream);

	if (ret != 1) die("Failed writing");
}

void sread(void * const ptr, const size_t size, FILE * const stream) {

	const size_t ret = fread(ptr, size, 1, stream);

	if (ret != 1) die("Failed reading");
}

void getsuffix(const char str[], char suf[3]) {

	const u32 len = strlen(str);
	if (len == 0)
		return;

	if (len == 1) {
		suf[0] = str[0];
		suf[1] = '\0';
		return;
	}

	suf[0] = str[len - 2];
	suf[1] = str[len - 1];
	suf[2] = '\0';

	if (suf[0] == '*' && suf[1] != '*') {
		suf[0] = suf[1];
		suf[1] = '\0';
	} else if (suf[0] == '*' || suf[1] == '*') {
		suf[0] = '*';
		suf[1] = '\0';
	}
}

void printctx(const struct urlctx * const ctx) {

	u16 p, s, n;
	u16 pmax, smax, nmax;

	pmax = ctx->count;

	printf("URL context has %u prefixes\n", pmax);

	for (p = 0; p < pmax; p++) {
		const struct prefix * const curpref = &ctx->pref[p];

		smax = curpref->count;
		printf("\tPrefix %u '%s' has %u suffixes\n", p, curpref->prefix, smax);

		for (s = 0; s < smax; s++) {
			const struct suffix * const cursuf = &curpref->suf[s];

			nmax = cursuf->count;
			printf("\t\tSuffix %u '%s' has %u needles\n", s, cursuf->suffix,
					nmax);

			for (n = 0; n < cursuf->count; n++) {
				const struct needle * const curneed = &cursuf->need[n];

				printf("\t\t\tNeedle %u: %s\n", n, curneed->needle);
			}
		}
	}
}

int ctxcmp(const struct urlctx * const a, const struct urlctx * const b) {

	u16 p, s, n;
	u16 pmax, smax, nmax;

	pmax = a->count;


#define cmperr(ack) do { fprintf(stderr, ack "\n"); return 1; } while (0)

	if (a->count != b->count) cmperr("prefix count");

	for (p = 0; p < pmax; p++) {
		const struct prefix * const curpref = &a->pref[p];
		const struct prefix * const curbpref = &b->pref[p];

		smax = curpref->count;
		if (curpref->count != curbpref->count) cmperr("suffix count");
		if (strcmp(curpref->prefix, curbpref->prefix)) cmperr("prefix");
		if (curpref->len != curbpref->len) cmperr("prefix length");

		for (s = 0; s < smax; s++) {
			const struct suffix * const cursuf = &curpref->suf[s];
			const struct suffix * const curbsuf = &curbpref->suf[s];

			nmax = cursuf->count;
			if (cursuf->count != curbsuf->count) cmperr("needle count");
			if (strcmp(cursuf->suffix, curbsuf->suffix)) cmperr("suffix");

			for (n = 0; n < nmax; n++) {
				const struct needle * const curneed = &cursuf->need[n];
				const struct needle * const curbneed = &curbsuf->need[n];

				if (curneed->len != curbneed->len)
					cmperr("needle len");
				if (curneed->wilds != curbneed->wilds)
					cmperr("needle wilds");
				if (curneed->longest != curbneed->longest)
					cmperr("needle longest");
				if (curneed->longlen != curbneed->longlen)
					cmperr("needle longlen");
				if (strcmp(curneed->needle, curbneed->needle))
					cmperr("needle");
			}
		}
	}

#undef cmperr

	return 0;
}

void *poolalloc(struct urlctx * const ctx, u32 bytes) {

	/* Everything we return is 64-bit aligned.

	   This is guaranteed by relying on our base
	   pointer being ok, and only giving out
	   multiples of 8. */

	while (bytes % 8 != 0)
		bytes++;

	if (ctx->used + bytes > ctx->storagelen)
		die("Storage OOM");

	const u32 cur = ctx->used;
	ctx->used += bytes;

	return ctx->storage + cur;
}
