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
#include "photon/extend/urlmatch/urlmatch.h"
#include <zlib.h>

int url_save_optimized(const urlctx *ctx, const char file[]) {

	const int fd = open(file, O_WRONLY | O_CREAT, 0644);
	if (fd < 0)
		return 1;

	return url_save_optimized2(ctx, fd);
}

int url_save_optimized2(const urlctx *ctx, const int fd) {

	char *buf;
	size_t len;

	FILE *f = open_memstream(&buf, &len);
	if (!f) return 1;

	swrite(&ctx->count, 2, f);
	swrite(&ctx->storagelen, 4, f);

	u32 p, s, n;
	for (p = 0; p < ctx->count; p++) {
		const struct prefix * const curpref = &ctx->pref[p];
		swrite(&curpref->count, 2, f);
		swrite(curpref->prefix, 5, f);
		swrite(&curpref->len, 1, f);

		for (s = 0; s < curpref->count; s++) {
			const struct suffix * const cursuf = &curpref->suf[s];
			swrite(&cursuf->count, 2, f);
			swrite(cursuf->suffix, 2, f);

			for (n = 0; n < cursuf->count; n++) {
				const struct needle * const curneed = &cursuf->need[n];
				swrite(&curneed->len, 2, f);
				swrite(&curneed->wilds, 2, f);
				swrite(&curneed->longest, 2, f);
				swrite(&curneed->longlen, 2, f);
				swrite(curneed->needle, curneed->len + 1, f);
			}
		}
	}

	fclose(f);

	// Cool, a buffer. Let's compress it.
	u64 bound = compressBound(len);
	u8 *dest = xcalloc(bound, 1);
	if (compress2(dest, &bound, (u8 *) buf, len, 9) != Z_OK) return 2;

	free(buf);

	f = fdopen(fd, "w");
	if (!f) return 1;

	swrite(MAGIC, 3, f);
	swrite(&len, sizeof(size_t), f);
	swrite(dest, bound, f);

	free(dest);
	fclose(f);
	return 0;
}

static int finalcheck(const char find[], const u32 len,
			const char hay[], const u32 haylen) {

	// This is the core of the simple check

	u32 i, h = 0;

	for (i = 0; i < len; i++) {
		if (find[i] != '*') {
			if (find[i] != hay[h])
				return 0;
			h++;
		} else {
			// If multiple wildcards in a row, skip to the last
			while (find[i+1] == '*') i++;

			if (i == len - 1)
				return 1;

			// Wildcard, not last
			const char * const ender = strchrnul(&find[i + 1], '*');
			const u32 dist = ender - &find[i + 1];

			char piece[dist + 1];
			memcpy(piece, &find[i + 1], dist);
			piece[dist] = '\0';

			const char * const lastmatch = strrstr(&hay[h], piece);
			if (!lastmatch)
				return 0;

			// Is backtracking required?
			const char * const firstmatch = strstr(&hay[h], piece);

			// The dist check is to make sure this is not a suffix search
			if (firstmatch != lastmatch && dist != len - i - 1) {
				const u32 move = firstmatch - &hay[h];
				h += move;
			} else {
				const u32 move = lastmatch - &hay[h];
				h += move;
			}
		}
	}

	// We ran out of needle but not hay
	if (h != haylen) return 0;

	return 1;

}

static void getsuffixlen(const char str[], char suf[3], const u32 len) {

	if (len == 1) {
		suf[0] = str[0];
		suf[1] = '\0';
		return;
	}

	suf[0] = str[len - 2];
	suf[1] = str[len - 1];
	suf[2] = '\0';
}

int url_match(const urlctx * const ctx, const char haystack[]) {

	const u32 len = strlen(haystack);
	char suf[3], pref[6];

	if (len < 1) return 0;
	getsuffixlen(haystack, suf, len);

	strncpy(pref, haystack, 5);
	pref[5] = '\0';

	u32 p, s;

	// Find all applicable prefixes
	const u32 pmax = ctx->count;
	for (p = 0; p < pmax; p++) {
		const struct prefix * const curpref = &ctx->pref[p];

		// Does this prefix match?
		if (curpref->prefix[0] != '*') {
			int ret = strncmp(pref, curpref->prefix, curpref->len);
			if (ret > 0)
				continue;
			if (ret < 0)
				break;
		}

		const u32 smax = curpref->count;
		for (s = 0; s < smax; s++) {
			const struct suffix * const cursuf = &curpref->suf[s];

			// Does this suffix match?
			if (cursuf->suffix[0] != '*' &&
				suffixcmp(suf, cursuf->suffix))
				continue;

			// OK, we have to test all needles in this suffix.
			u32 n;
			const u32 nmax = cursuf->count;
			for (n = 0; n < nmax; n++) {
				const struct needle * const curneed = &cursuf->need[n];

				// First: no wildcards
				if (!curneed->wilds) {
					// Do the lengths match?
					if (len != curneed->len)
						continue;
					if (!strcmp(haystack, curneed->needle))
						return 1;
				} else {
					// Is the longest streak in it?
					if (curneed->longlen) {
						if (curneed->longlen >= 4) {
							if (!memmem(haystack, len,
								curneed->needle + curneed->longest,
								curneed->longlen))
								continue;
						} else {
							if (!memchr(haystack,
								curneed->needle[curneed->longest],
								len))
								continue;
						}
					}

					// The prefix and suffix match, and it contains
					// the longest streak. Do the actual comparison.
					if (finalcheck(curneed->needle, curneed->len,
						haystack, len))
						return 1;
				}
			}
		}
	}

	return 0;
}

void url_free(urlctx *ctx) {

	free(ctx->storage);
	free(ctx);
}
