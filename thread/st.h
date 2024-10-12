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

#ifndef __STATE_THREADS__
#define __STATE_THREADS__
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

// This file provides an emulation layer for state threads (https://state-threads.sourceforge.net)
// #include <photon/thread/thread.h>
// #include <photon/net/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *  st_thread_t;
typedef void *  st_cond_t;
typedef void *  st_mutex_t;
typedef void *  st_netfd_t;
typedef unsigned long long st_utime_t;
// typedef void (*st_switch_cb_t)(void);



#define ST_EVENTSYS_DEFAULT     0 // epoll in Linux, kqueue in MacOSX
#define ST_EVENTSYS_SELECT      1 // epoll in Linux, kqueue in MacOSX
#define ST_EVENTSYS_POLL        2 // epoll in Linux, kqueue in MacOSX
#define ST_EVENTSYS_ALT         3 // epoll in Linux, kqueue in MacOSX
#define ST_EVENTSYS_IOURING     4 // io_uring in Linux, kqueue in MacOSX

int st_set_eventsys(int eventsys);
int st_get_eventsys(void);
int st_init(void);
int st_getfdlimit(void);
const char *st_get_eventsys_name(void);
// st_switch_cb_t st_set_switch_in_cb(st_switch_cb_t cb);
// st_switch_cb_t st_set_switch_out_cb(st_switch_cb_t cb);

st_thread_t st_thread_create(void *(*start)(void *arg), void *arg,
                             int joinable, int stack_size);
void st_thread_exit(void *retval);
int st_thread_join(st_thread_t thread, void **retvalp);
st_thread_t st_thread_self(void);
void st_thread_interrupt(st_thread_t thread);
int st_sleep(int secs);
int st_usleep(st_utime_t usecs);
int st_randomize_stacks(int on);
int st_key_create(int *keyp, void (*destructor)(void *));
int st_key_getlimit(void);
int st_thread_setspecific(int key, void *value);
void *st_thread_getspecific(int key);

// Synchronization
st_cond_t st_cond_new(void);
int st_cond_destroy(st_cond_t cvar);
int st_cond_wait(st_cond_t cvar);
int st_cond_timedwait(st_cond_t cvar, st_utime_t timeout);
int st_cond_signal(st_cond_t cvar);
int st_cond_broadcast(st_cond_t cvar);

st_mutex_t st_mutex_new(void);
int st_mutex_destroy(st_mutex_t lock);
int st_mutex_lock(st_mutex_t lock);
int st_mutex_trylock(st_mutex_t lock);
int st_mutex_unlock(st_mutex_t lock);

time_t st_time(void);
st_utime_t st_utime(void);
int st_set_utime_function(st_utime_t (*func)(void));
int st_timecache_set(int on);


// I/O Functions
st_netfd_t st_netfd_open(int osfd);
st_netfd_t st_netfd_open_socket(int osfd);
void st_netfd_free(st_netfd_t fd);
int st_netfd_close(st_netfd_t fd);
int st_netfd_fileno(st_netfd_t fd);
void st_netfd_setspecific(st_netfd_t fd, void *value,
                          void (*destructor)(void *));
void *st_netfd_getspecific(st_netfd_t fd);
int st_netfd_serialize_accept(st_netfd_t fd);
int st_netfd_poll(st_netfd_t fd, int how, st_utime_t timeout);
st_netfd_t st_accept(st_netfd_t fd, struct sockaddr *addr, int *addrlen,
                     st_utime_t timeout);
int st_connect(st_netfd_t fd, const struct sockaddr *addr, int addrlen,
               st_utime_t timeout);
ssize_t st_read(st_netfd_t fd, void *buf, size_t nbyte, st_utime_t timeout);
ssize_t st_read_fully(st_netfd_t fd, void *buf, size_t nbyte,
                      st_utime_t timeout);
int st_read_resid(st_netfd_t fd, void *buf, size_t *resid,
		  st_utime_t timeout);
ssize_t st_readv(st_netfd_t fd, const struct iovec *iov, int iov_size,
		 st_utime_t timeout);
int st_readv_resid(st_netfd_t fd, struct iovec **iov, int *iov_size,
		   st_utime_t timeout);
ssize_t st_write(st_netfd_t fd, const void *buf, size_t nbyte,
                 st_utime_t timeout);
int st_write_resid(st_netfd_t fd, const void *buf, size_t *resid,
                   st_utime_t timeout);
ssize_t st_writev(st_netfd_t fd, const struct iovec *iov, int iov_size,
                  st_utime_t timeout);
ssize_t st_writev(st_netfd_t fd, const struct iovec *iov, int iov_size,
                  st_utime_t timeout);
int st_writev_resid(st_netfd_t fd, struct iovec **iov, int *iov_size,
		    st_utime_t timeout);
int st_recvfrom(st_netfd_t fd, void *buf, int len, struct sockaddr *from,
                int *fromlen, st_utime_t timeout);
int st_sendto(st_netfd_t fd, const void *msg, int len, struct sockaddr *to,
              int tolen, st_utime_t timeout);
int st_recvmsg(st_netfd_t fd, struct msghdr *msg, int flags,
               st_utime_t timeout);
int st_sendmsg(st_netfd_t fd, const struct msghdr *msg, int flags,
               st_utime_t timeout);
st_netfd_t st_open(const char *path, int oflags, mode_t mode);
int st_poll(struct pollfd *pds, int npds, st_utime_t timeout);




#ifdef __cplusplus
}
#endif


#endif