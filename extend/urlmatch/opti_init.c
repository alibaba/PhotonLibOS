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

static urlctx *initbin(FILE * const f, const u32 inlen) {

	size_t len;
	sread(&len, sizeof(size_t), f);

	u8 * const src = xcalloc(inlen, 1);
	u8 *buf = xcalloc(len, 1);
	u8 * const origbuf = buf;

	sread(src, inlen, f);
	if (uncompress(buf, &len, src, inlen) != Z_OK) return NULL;
	free(src);

	// Cool, unpacked. Read it.
	urlctx * const out = xcalloc(sizeof(urlctx), 1);

	memcpy(&out->count, buf, 2);
	buf += 2;

	memcpy(&out->storagelen, buf, 4);
	buf += 4;

	out->storage = xcalloc(out->storagelen, 1);
	out->pref = poolalloc(out, sizeof(struct prefix) * out->count);
	u32 p, s, n;

	for (p = 0; p < out->count; p++) {
		struct prefix * const curpref = &out->pref[p];

		memcpy(&curpref->count, buf, 2);
		buf += 2;
		memcpy(curpref->prefix, buf, 5);
		buf += 5;
		curpref->len = *buf;
		buf++;

		curpref->suf = poolalloc(out, sizeof(struct suffix) * curpref->count);

		for (s = 0; s < curpref->count; s++) {
			struct suffix * const cursuf = &curpref->suf[s];

			memcpy(&cursuf->count, buf, 2);
			buf += 2;
			memcpy(cursuf->suffix, buf, 2);
			buf += 2;

			cursuf->need = poolalloc(out, sizeof(struct needle) * cursuf->count);

			for (n = 0; n < cursuf->count; n++) {
				struct needle * const curneed = &cursuf->need[n];

				memcpy(&curneed->len, buf, 2);
				buf += 2;
				memcpy(&curneed->wilds, buf, 2);
				buf += 2;
				memcpy(&curneed->longest, buf, 2);
				buf += 2;
				memcpy(&curneed->longlen, buf, 2);
				buf += 2;

				curneed->needle = poolalloc(out, curneed->len + 1);

				memcpy((char *) curneed->needle, buf, curneed->len + 1);
				buf += curneed->len + 1;
			}
		}
	}

	free(origbuf);
	return out;
}

urlctx *url_init_file(const char file[]) {

	const int fd = open(file, O_RDONLY);
	if (fd < 0)
		return NULL;

	return url_init_file2(fd);
}

urlctx *url_init_file2(const int fd) {

	FILE * const f = fdopen(fd, "r");
	if (!f) return NULL;

	fseek(f, 0, SEEK_END);
	const long len = ftell(f);
	rewind(f);

	char buf[4] = { 0 };
	fread(buf, 3, 1, f);

	urlctx *out = NULL;

	// Binary format
	if (!strcmp(buf, MAGIC)) {
		out = initbin(f, len - 3 - sizeof(size_t));
	} else { // Text format
		rewind(f);

		char *tmp = xcalloc(len, 1);
		if (fread(tmp, len, 1, f) != 1) die("Failed reading");
		out = url_init(tmp);
		free(tmp);
	}

	fclose(f);
	return out;
}

static int wildpfxcmp(const char a[], const char b[]) {

	const int awild = wildprefix(a);
	const int bwild = wildprefix(b);

	if (!awild && !bwild) {
		return strncmp(a, b, 5);
	} else if (awild && !bwild) {
		return strncmp("*", b, 5);
	} else if (!awild && bwild) {
		return strncmp(a, "*", 5);
	}

	return 0;
}

static void preparepfx(char str[]) {

	if (!wildprefix(str))
		return;

	memset(str + 1, '\0', 4);
	str[0] = '*';
}

static int cstrcmp(const void * const p1, const void * const p2) {

	const char * const a = * (char * const *) p1;
	const char * const b = * (char * const *) p2;

	int ret = wildpfxcmp(a, b);
	if (ret) return ret;

	// Secondary sort by the suffix
	char sufa[3] = { 0 };
	char sufb[3] = { 0 };

	getsuffix(a, sufa);
	getsuffix(b, sufb);

	return strcmp(sufa, sufb);
}

static void calclongest(const char needle[], const u16 len, const u16 wilds,
			u16 * const longest, u16 * const longlen) {

	// Easy path
	if (wilds == 1) {
		const char *ptr = strchr(needle, '*');
		const u16 pos = ptr - needle;
		const u16 half = len / 2;

		if (pos < half) {
			*longlen = len - pos - 1;
			*longest = pos + 1;
		} else {
			*longlen = pos;
			*longest = 0;
		}
	} else {
		u16 max = 0;
		u16 maxlen = 0;

		const char *ptr = needle;
		while (*ptr) {
			const char * const next = strchrnul(ptr, '*');
			const u16 thislen = next - ptr;

			if (maxlen < thislen) {
				maxlen = thislen;
				max = ptr - needle;
			}

			if (!*next) break;
			ptr = next + 1;
		}

		*longest = max;
		*longlen = maxlen;
	}
}

static void addneedle(urlctx * const ctx, struct needle * const to, const char from[]) {

	const u32 len = strlen(from);
	to->needle = poolalloc(ctx, len + 1);
	memcpy((char *) to->needle, from, len + 1);

	to->len = len;
	to->wilds = countwilds(from);

	if (to->wilds)
		calclongest(from, to->len, to->wilds, &to->longest, &to->longlen);
}

urlctx *url_init(const char contents[]) {

	u32 lines = 1;
	const char *ptr = contents;
	const u32 contentlen = strlen(contents);
	const char * const endbyte = ptr + contentlen;
	for (; *ptr; ptr++) {
		if (*ptr == '\n') lines++;
	}

	char **outlines = xcalloc(lines, sizeof(char *));
	const u32 origlines = lines;

	// Copy each pattern line to its own space, and optimize on the way
	ptr = contents;
	u32 i = 0, j;
	while (1) {
		const char * const end = strchrnul(ptr, '\n');
		const u32 len = end - ptr;

		if (len < 1) {
			ptr = end + 1;
			if (ptr >= endbyte) { i--; break; }
			continue;
		}

		char tmp[len + 1];
		tmp[len] = '\0';
		memcpy(tmp, ptr, len);

		outlines[i] = xcalloc(len + 1, 1);

		u32 p, o;
		outlines[i][0] = tmp[0];
		for (p = 1, o = 1; p < len; p++) {
			if (tmp[p - 1] == '*' && tmp[p] == '*') {
				continue;
			}
			outlines[i][o] = tmp[p];

			o++;
		}

		if (!*end) break;
		ptr = end + 1;
		i++;
	}

	lines = i + 1;
	qsort(outlines, lines, sizeof(char *), cstrcmp);

	urlctx * const out = xcalloc(sizeof(urlctx), 1);
	// The theoretical maximum amount needed
	out->storagelen = contentlen + 1 +
				lines * (sizeof(struct suffix) +
						sizeof(struct needle) +
						sizeof(struct prefix) + 8);
	out->storage = xcalloc(out->storagelen, 1);

	// How many prefixes do we have?
	u32 prefixes = 1;
	for (i = 1; i < lines; i++) {
		if (strncmp(outlines[i - 1], outlines[i], 5) &&
			(!wildprefix(outlines[i - 1]) || !wildprefix(outlines[i])))
			prefixes++;
	}

	out->count = prefixes;
	out->pref = poolalloc(out, sizeof(struct prefix) * prefixes);

	// Add each prefix
	prefixes = 1;
	strncpy(out->pref[0].prefix, outlines[0], 5);
	preparepfx(out->pref[0].prefix);
	out->pref[0].len = strlen(out->pref[0].prefix);
	for (i = 1; i < lines; i++) {
		if (wildpfxcmp(outlines[i - 1], outlines[i])) {
			strncpy(out->pref[prefixes].prefix, outlines[i], 5);

			preparepfx(out->pref[prefixes].prefix);
			out->pref[prefixes].len = strlen(out->pref[prefixes].prefix);

			prefixes++;
		}
	}

	// For each prefix, how many suffixes are there?
	for (i = 0; i < out->count; i++) {

		struct prefix * const curpref = &out->pref[i];

		u32 suffixes = 0;
		char prevsuf[3] = { 0 };
		for (j = 0; j < lines; j++) {
			const int ret = wildpfxcmp(curpref->prefix, outlines[j]);

			if (ret > 0) continue;
			if (ret < 0) break;

			char suf[3];
			getsuffix(outlines[j], suf);
			if (strcmp(prevsuf, suf)) suffixes++;
			memcpy(prevsuf, suf, 3);
		}

		curpref->suf = poolalloc(out, sizeof(struct suffix) * suffixes);
		curpref->count = suffixes;

		// For each suffix, how many needles do we have?
		suffixes = 0;
		prevsuf[0] = prevsuf[1] = 0;
		for (j = 0; j < lines; j++) {
			const int ret = wildpfxcmp(curpref->prefix, outlines[j]);

			if (ret > 0) continue;
			if (ret < 0) break;

			char suf[3];
			getsuffix(outlines[j], suf);
			if (strcmp(prevsuf, suf)) {
				curpref->suf[suffixes].count = 1;
				memcpy(curpref->suf[suffixes].suffix, suf, 3);

				suffixes++;
			} else {
				curpref->suf[suffixes - 1].count++;
			}
			memcpy(prevsuf, suf, 3);
		}

		// Allocate the needle counts
		for (j = 0; j < curpref->count; j++) {
			curpref->suf[j].need = poolalloc(out, sizeof(struct needle) *
							curpref->suf[j].count);
		}

		// For each suffix, save the needles
		suffixes = 0;
		prevsuf[0] = prevsuf[1] = 0;
		for (j = 0; j < lines; j++) {
			const int ret = wildpfxcmp(curpref->prefix, outlines[j]);

			if (ret > 0) continue;
			if (ret < 0) break;

			char suf[3];
			getsuffix(outlines[j], suf);
			if (strcmp(prevsuf, suf)) {
				struct suffix * const cursuf = &curpref->suf[suffixes];

				cursuf->count = 1;
				memcpy(cursuf->suffix, suf, 3);
				addneedle(out, &cursuf->need[0], outlines[j]);
				suffixes++;
			} else {
				struct suffix * const cursuf = &curpref->suf[suffixes - 1];

				addneedle(out, &cursuf->need[cursuf->count],
						outlines[j]);
				cursuf->count++;
			}
			memcpy(prevsuf, suf, 3);
		}
	}

	for (i = 0; i < origlines; i++) free(outlines[i]);
	free(outlines);

	// Refresh storage size, so that binary save + load doesn't waste space
	out->storagelen = out->used;

	return out;
}
