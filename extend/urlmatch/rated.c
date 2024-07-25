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

#include <ctype.h>

static u32 wordlen(const char *start) {

	const char * const orig = start;

	while (!isspace(*start) && *start) start++;

	return start - orig;
}

static const char *nextword(const char *ptr) {

	while (isspace(*ptr) && *ptr) ptr++;

	return ptr;
}

int ratedsearch(const char needle[], const char haystack[]) {

	// For each source word, if it's present in haystack, increment score.
	// IOW, a simple google-like search.
	const u32 tmplen = 320;
	char tmp[tmplen];

	const char *cur = nextword(needle);
	u32 wlen = wordlen(cur);
	u32 score = 0;

	while (*cur) {
		if (wlen >= tmplen)
			return -1;
		memcpy(tmp, cur, wlen);
		tmp[wlen] = '\0';

		if (strcasestr(haystack, tmp))
			score++;

		cur += wlen;
		cur = nextword(cur);
		wlen = wordlen(cur);
	}

	return score;
}
