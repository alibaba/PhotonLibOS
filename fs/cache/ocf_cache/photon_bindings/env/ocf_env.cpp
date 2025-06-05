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

#include <photon/thread/thread.h>
#include <photon/common/alog.h>

#include <execinfo.h>
#include <sched.h>

extern "C" {
#include "ocf_env.h"
}

/* ALLOCATOR */
struct _env_allocator {
	/*!< Memory pool ID unique name */
	char *name;

	/*!< Size of specific item of memory pool */
	uint32_t item_size;

	/*!< Number of currently allocated items in pool */
	env_atomic count;
};

struct _env_allocator_item {
	uint32_t flags;
	uint32_t cpu;
	char data[];
};

void *env_allocator_new(env_allocator *allocator)
{
	auto item = (_env_allocator_item *)calloc(1, allocator->item_size);

	if (item) {
		item->cpu = 0;
		env_atomic_inc(&allocator->count);
	}

	return &item->data;
}

env_allocator *env_allocator_create(uint32_t size, const char *fmt_name, ...)
{
	char name[OCF_ALLOCATOR_NAME_MAX] = { '\0' };
	int result, error = -1;
	va_list args;

	auto allocator = (env_allocator *) calloc(1, sizeof(env_allocator));
	if (!allocator) {
		error = __LINE__;
		goto err;
	}

	allocator->item_size = size + sizeof(struct _env_allocator_item);

	/* Format allocator name */
	va_start(args, fmt_name);
	result = vsnprintf(name, sizeof(name), fmt_name, args);
	va_end(args);

	if ((result > 0) && (result < (int)sizeof(name))) {
		allocator->name = strdup(name);

		if (!allocator->name) {
			error = __LINE__;
			goto err;
		}
	} else {
		/* Formated string name exceed max allowed size of name */
		error = __LINE__;
		goto err;
	}

	return allocator;

err:
	printf("Cannot create memory allocator, ERROR %d", error);
	env_allocator_destroy(allocator);

	return NULL;
}

void env_allocator_del(env_allocator *allocator, void *obj)
{
	_env_allocator_item *item =
		container_of(obj, _env_allocator_item, data);

	env_atomic_dec(&allocator->count);

	free(item);
}

void env_allocator_destroy(env_allocator *allocator)
{
	if (allocator) {
		if (env_atomic_read(&allocator->count)) {
			printf("Not all objects deallocated\n");
			ENV_WARN(true, OCF_PREFIX_SHORT" Cleanup problem\n");
		}

		free(allocator->name);
		free(allocator);
	}
}

/* DEBUGING */
#define ENV_TRACE_DEPTH	16

void env_stack_trace(void)
{
	void *trace[ENV_TRACE_DEPTH];
	char **messages = NULL;
	int i, size;

	size = backtrace(trace, ENV_TRACE_DEPTH);
	messages = backtrace_symbols(trace, size);
	printf("[stack trace]>>>\n");
	for (i = 0; i < size; ++i)
		printf("%s\n", messages[i]);
	printf("<<<[stack trace]\n");
	free(messages);
}

/* CRC */
uint32_t env_crc32(uint32_t crc, uint8_t const *data, size_t len)
{
	return crc32(crc, data, len);
}

/* EXECUTION CONTEXTS */
#ifdef EXECUTION_CONTEXTS

pthread_mutex_t *exec_context_mutex;

static void __attribute__((constructor)) init_execution_context(void)
{
	unsigned count = env_get_execution_context_count();
	unsigned i;

	ENV_BUG_ON(count == 0);
	exec_context_mutex = (pthread_mutex_t *)malloc(count * sizeof(exec_context_mutex[0]));
	ENV_BUG_ON(exec_context_mutex == NULL);
	for (i = 0; i < count; i++)
		ENV_BUG_ON(pthread_mutex_init(&exec_context_mutex[i], NULL));
}

static void __attribute__((destructor)) deinit_execution_context(void)
{
	unsigned count = env_get_execution_context_count();
	unsigned i;

	ENV_BUG_ON(count == 0);
	ENV_BUG_ON(exec_context_mutex == NULL);

	for (i = 0; i < count; i++)
		ENV_BUG_ON(pthread_mutex_destroy(&exec_context_mutex[i]));
	free(exec_context_mutex);
}

/* get_execuction_context must assure that after the call finishes, the caller
 * will not get preempted from current execution context. For userspace env
 * we simulate this behavior by acquiring per execution context mutex. As a
 * result the caller might actually get preempted, but no other thread will
 * execute in this context by the time the caller puts current execution ctx. */
unsigned env_get_execution_context(void)
{
	unsigned cpu;

	cpu = sched_getcpu();
	cpu = (cpu == -1U) ?  0 : cpu;

	ENV_BUG_ON(pthread_mutex_lock(&exec_context_mutex[cpu]));

	return cpu;
}

void env_put_execution_context(unsigned ctx)
{
	pthread_mutex_unlock(&exec_context_mutex[ctx]);
}

unsigned env_get_execution_context_count(void)
{
	int num = sysconf(_SC_NPROCESSORS_ONLN);

	return (num == -1) ? 0 : num;
}

#endif

void env_rwlock_init(env_rwlock *l) {
    l->lock = new photon::rwlock;
}

void env_rwlock_read_lock(env_rwlock *l) {
    auto lock = (photon::rwlock*) l->lock;
    lock->lock(photon::RLOCK);
}

void env_rwlock_read_unlock(env_rwlock *l) {
    auto lock = (photon::rwlock*) l->lock;
    lock->unlock();
}

void env_rwlock_write_lock(env_rwlock *l) {
    auto lock = (photon::rwlock*) l->lock;
    lock->lock(photon::WLOCK);
}

void env_rwlock_write_unlock(env_rwlock *l) {
    auto lock = (photon::rwlock*) l->lock;
    lock->unlock();
}

void env_rwlock_destroy(env_rwlock *l) {
    auto lock = (photon::rwlock*) l->lock;
    delete lock;
}

void env_msleep(uint64_t n) {
    photon::thread_usleep(n * 1000);
}

int env_mutex_init(env_mutex* mutex) {
    mutex->mutex = new photon::mutex;
    return 0;
}

void env_mutex_lock(env_mutex* mutex) {
    auto m = (photon::mutex*) mutex->mutex;
    m->lock();
}

int env_mutex_trylock(env_mutex* mutex) {
    auto m = (photon::mutex*) mutex->mutex;
    return m->try_lock();
}

void env_mutex_unlock(env_mutex* mutex) {
    auto m = (photon::mutex*) mutex->mutex;
    m->unlock();
}

int env_mutex_destroy(env_mutex* mutex) {
    auto m = (photon::mutex*) mutex->mutex;
    delete m;
    return 0;
}

void env_completion_init(env_completion* completion) {
    auto sem = new photon::semaphore;
    completion->sem = sem;
}

void env_completion_wait(env_completion* completion) {
    auto sem = (photon::semaphore*) completion->sem;
    sem->wait(1);
}

void env_completion_complete(env_completion* completion) {
    auto sem = (photon::semaphore*) completion->sem;
    sem->signal(1);
}

void env_completion_destroy(env_completion* completion) {
    auto sem = (photon::semaphore*) completion->sem;
    delete sem;
}

int env_rmutex_init(env_rmutex* rmutex) {
    rmutex->rmutex = new photon::recursive_mutex;
    return 0;
}

void env_rmutex_lock(env_rmutex* rmutex) {
    auto m = (photon::recursive_mutex*) rmutex->rmutex;
    m->lock();
}

void env_rmutex_unlock(env_rmutex* rmutex) {
    auto m = (photon::recursive_mutex*) rmutex->rmutex;
    m->unlock();
}

int env_rmutex_destroy(env_rmutex* rmutex) {
    auto m = (photon::recursive_mutex*) rmutex->rmutex;
    delete m;
    return 0;
}

int env_spinlock_init(env_spinlock* l) {
    l->lock = new photon::mutex;
    return 0;
}

int env_spinlock_trylock(env_spinlock* l) {
    auto lock = (photon::mutex*) l->lock;
    return lock->try_lock() ? -OCF_ERR_NO_LOCK : 0;
}

void env_spinlock_lock(env_spinlock* l) {
    auto lock = (photon::mutex*) l->lock;
    lock->lock();
}

void env_spinlock_unlock(env_spinlock* l) {
    auto lock = (photon::mutex*) l->lock;
    lock->unlock();
}

void env_spinlock_destroy(env_spinlock* l) {
    auto m = (photon::mutex*) l->lock;
    delete m;
}

int env_rwsem_init(env_rwsem* s) {
    s->lock = new photon::mutex;
    return 0;
}

void env_rwsem_up_read(env_rwsem* s) {
    auto lock = (photon::mutex*) s->lock;
    lock->unlock();
}

void env_rwsem_down_read(env_rwsem* s) {
    auto lock = (photon::mutex*) s->lock;
    lock->lock();
}

int env_rwsem_down_read_trylock(env_rwsem* s) {
    auto lock = (photon::mutex*) s->lock;
    return lock->try_lock() ? -OCF_ERR_NO_LOCK : 0;
    // return 0;
}

void env_rwsem_up_write(env_rwsem* s) {
    auto lock = (photon::mutex*) s->lock;
    lock->unlock();
}

void env_rwsem_down_write(env_rwsem* s) {
    auto lock = (photon::mutex*) s->lock;
    lock->lock();
}

int env_rwsem_down_write_trylock(env_rwsem* s) {
    auto lock = (photon::mutex*) s->lock;
    return lock->try_lock() ? -OCF_ERR_NO_LOCK : 0;
}

int env_rwsem_destroy(env_rwsem* s) {
    auto m = (photon::mutex*) s->lock;
    delete m;
    return 0;
}