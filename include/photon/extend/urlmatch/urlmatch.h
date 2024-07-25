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

#ifndef URLMATCH_H
#define URLMATCH_H

#ifdef __cplusplus
extern "C" {
#endif

// Let's help the compiler
#if __GNUC__ >= 4

	#define PURE_FUNC __attribute__ ((pure))
	#define NORETURN_FUNC __attribute__ ((noreturn))
	#define CONST_FUNC __attribute__ ((const))
	#define WUR_FUNC __attribute__ ((warn_unused_result))
	#define NONNULL(A) __attribute__ ((nonnull (A)))
#else // GNUC

	#define PURE_FUNC
	#define NORETURN_FUNC
	#define CONST_FUNC
	#define WUR_FUNC
	#define NONNULL

#endif // GNUC

// Returns 1 if haystack matches pattern, 0 otherwise.
int url_simplematch(const char pattern[], const char haystack[]) WUR_FUNC PURE_FUNC;

/* These two functions initialize the optimized pattern matcher.
 * _init takes a char array of patterns, one per line.
 * _init_file takes a filename, either a text file containing one pattern per line,
 * or an optimized binary file as saved by _save_optimized.
 *
 * On error they return NULL. */
typedef struct urlctx urlctx;
urlctx *url_init_file(const char file[]) WUR_FUNC;
urlctx *url_init_file2(const int fd) WUR_FUNC;
urlctx *url_init(const char contents[]) WUR_FUNC;

// Save an optimized binary file for faster loading later. Returns 0 on success.
int url_save_optimized(const urlctx *ctx, const char file[]) WUR_FUNC NONNULL(1);
int url_save_optimized2(const urlctx *ctx, const int fd) WUR_FUNC NONNULL(1);

/* Returns 1 if haystack matches the optimized pattern, 0 otherwise.
 *
 * It's safe to call from multiple threads at once, with the same context. */
int url_match(const urlctx *ctx, const char haystack[]) WUR_FUNC PURE_FUNC NONNULL(1);

// Frees this context.
void url_free(urlctx *ctx) NONNULL(1);

/* Auxiliary function for e.g. searching bookmarks
 *
 * Returns the match score, higher the better. -1 is returned on error. */
int ratedsearch(const char needle[], const char haystack[]) WUR_FUNC PURE_FUNC;

#undef PURE_FUNC
#undef NORETURN_FUNC
#undef CONST_FUNC
#undef WUR_FUNC
#undef NONNULL

#ifdef __cplusplus
} // extern C
#endif

#endif
