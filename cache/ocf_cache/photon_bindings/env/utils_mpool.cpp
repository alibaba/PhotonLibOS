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

#ifdef __cplusplus
extern "C" {
#include "ocf_env.h"
#include "utils_mpool.h"
}
#else
#include "ocf_env.h"
#include "utils_mpool.h"
#endif

struct env_mpool {
	int mpool_max;
		/*!< Max mpool allocation order */

	env_allocator *allocator[env_mpool_max];
		/*!< OS handle to memory pool */

	uint32_t hdr_size;
		/*!< Data header size (constant allocation part) */

	uint32_t elem_size;
		/*!< Per element size increment (variable allocation part) */

	bool fallback;
		/*!< Should mpool fallback to vmalloc */

	int flags;
		/*!< Allocation flags */
};

struct env_mpool *env_mpool_create(uint32_t hdr_size, uint32_t elem_size,
		int flags, int mpool_max, bool fallback,
		const uint32_t limits[env_mpool_max],
		const char *name_perfix,
		bool zero)
{
	uint32_t i;
	char name[MPOOL_ALLOCATOR_NAME_MAX] = { '\0' };
	int result;
	struct env_mpool *mpool;
	size_t size;

	mpool = (env_mpool *)env_zalloc(sizeof(*mpool), ENV_MEM_NORMAL);
	if (!mpool)
		return NULL;

	mpool->flags = flags;
	mpool->fallback = fallback;
	mpool->mpool_max = mpool_max;
	mpool->hdr_size = hdr_size;
	mpool->elem_size = elem_size;

	for (i = 0; i < (uint32_t) min(env_mpool_max, mpool_max + 1); i++) {
		result = snprintf(name, sizeof(name), "%s_%u", name_perfix,
				(1 << i));
		if (result < 0 || result >= (int) sizeof(name))
			goto err;

		size = hdr_size + (elem_size * (1 << i));

		mpool->allocator[i] = env_allocator_create_extended(
				size, name, limits ? limits[i] : -1,
				zero);

		if (!mpool->allocator[i])
			goto err;
	}

	return mpool;

err:
	env_mpool_destroy(mpool);
	return NULL;
}

void env_mpool_destroy(struct env_mpool *mallocator)
{
	if (mallocator) {
		uint32_t i;

		for (i = 0; i < env_mpool_max; i++)
			if (mallocator->allocator[i])
				env_allocator_destroy(mallocator->allocator[i]);

		env_free(mallocator);
	}
}

static env_allocator *env_mpool_get_allocator(
	struct env_mpool *mallocator, uint32_t count)
{
	unsigned int idx;

	if (unlikely(count == 0))
		return nullptr;

	idx = 31 - __builtin_clz(count);

	if (__builtin_ffs(count) <= idx)
		idx++;

	if (idx >= env_mpool_max || idx > (unsigned int) mallocator->mpool_max)
		return NULL;

	return mallocator->allocator[idx];
}

void *env_mpool_new_f(struct env_mpool *mpool, uint32_t count, int flags)
{
	void *items = NULL;
	env_allocator *allocator;
	size_t size = mpool->hdr_size + (mpool->elem_size * count);

	allocator = env_mpool_get_allocator(mpool, count);

	if (allocator) {
		items = env_allocator_new(allocator);
	} else if(mpool->fallback) {
		items = env_zalloc(size, 0);
	}

#ifdef ZERO_OR_NULL_PTR
	if (ZERO_OR_NULL_PTR(items))
		return NULL;
#endif

	return items;
}

void *env_mpool_new(struct env_mpool *mpool, uint32_t count)
{
	return env_mpool_new_f(mpool, count, mpool->flags);
}

bool env_mpool_del(struct env_mpool *mpool,
		void *items, uint32_t count)
{
	env_allocator *allocator;

	allocator = env_mpool_get_allocator(mpool, count);

	if (allocator)
		env_allocator_del(allocator, items);
	else if (mpool->fallback)
		env_free(items);
	else
		return false;

	return true;
}
