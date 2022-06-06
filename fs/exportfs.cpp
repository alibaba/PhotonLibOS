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

#include "exportfs.h"
#include <inttypes.h>
#include <functional>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <boost/lockfree/spsc_queue.hpp>
#include <sched.h>
#include "filesystem.h"
#include "async_filesystem.h"
#include <photon/thread/thread.h>
#include <photon/thread/thread-pool.h>
#include <photon/io/fd-events.h>
#include <photon/common/event-loop.h>
#include <photon/common/alog.h>

namespace photon {
namespace fs
{
    static EventLoop* evloop = nullptr;
    typedef boost::lockfree::spsc_queue<std::function<void()>, boost::lockfree::capacity<65536> > spsc;

    class ExportBase
    {
    public:
        static std::mutex mutex;
        using F0 = std::function<void()>;
        static spsc op_queue;
        static int ref;
        static condition_variable cond;
        static ThreadPoolBase* pool;
        static void perform(uint64_t /*timeout*/, const F0& act)
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                while (!op_queue.push(act)) sched_yield();
            }
            thread_interrupt(evloop->loop_thread(), EINPROGRESS);
        }
        static int wait4events(void*, EventLoop*)
        {
            if (op_queue.read_available()) return 1;
            auto th = photon::CURRENT;
            int ret = photon::thread_usleep_defer(1000UL * 1000, [&] {
                if (op_queue.read_available()) photon::thread_interrupt(th, EINPROGRESS);
            });
            if (ret < 0) {
                if (errno == EINPROGRESS) {
                    return 1;
                }
                return -1;
            }
            return 0;
        }
        static int on_events(void*, EventLoop*)
        {
            //TODO: thread pooling
            ++ref;
            if (pool != nullptr)
                pool->thread_create(&do_opq, nullptr);
            else
                photon::thread_create(&do_opq, nullptr);
            photon::thread_yield_to(nullptr); // let `th` to run and pop an op
            return 0;
        }
        static void* do_opq(void*)
        {
            DEFER({if (--ref == 0) cond.notify_all();});
            if (op_queue.read_available() == 0) return nullptr;
            F0 func;
            op_queue.pop(func);
            func();
            return nullptr;
        }
        template<typename R, typename RT = typename AsyncResult<R>::result_type>
        void do_callback(uint32_t id, RT result, Done<R> done)
        {
            AsyncResult<R> ar;
            ar.result = result;
            ar.object = this;
            ar.operation = id;
            if (op_failed(ar.result))
            {
                ERRNO e(ar.error_number = errno);
                LOG_DEBUG("failed to perform an action (ID = `) ", id, e);
            }

            int ret = done(&ar);
            if (ret < 0)
                LOG_ERROR("event notified, but got an error ", VALUE(ret), ERRNO());
        }
        bool op_failed(ssize_t r)
        {
            return r < 0;
        }
        bool op_failed(void* ptr)
        {
            return ptr == nullptr;
        }
        template <typename T>
        static void safe_delete(T *&ptr) {
            if (CURRENT) {
                // in photon environment
                delete ptr;
            } else {
                auto n_ptr = ptr;
                perform(-1UL, [n_ptr] { delete n_ptr; });
            }
            ptr = nullptr;
        }
    };

    __attribute__((visibility("hidden"))) std::mutex ExportBase::mutex;
    __attribute__((visibility("hidden"))) spsc ExportBase::op_queue;
    __attribute__((visibility("hidden"))) int ExportBase::ref = 1;
    __attribute__((visibility("hidden"))) condition_variable ExportBase::cond;
    __attribute__((visibility("hidden"))) ThreadPoolBase* ExportBase::pool = nullptr;

    #define PERFORM(ID, expr)       \
        perform(timeout, [=]() {    \
            do_callback(ID, expr, done); });

    class ExportAsAsyncFile : public ExportBase, public IAsyncFile
    {
    public:
        IFile* m_file;
        IAsyncFileSystem* m_fs;
        ExportAsAsyncFile(IFile* file, IAsyncFileSystem* fs)
        {
            m_file = file;
            m_fs = fs;
        }
        virtual ~ExportAsAsyncFile()
        {
            safe_delete(m_file);
        }
        virtual IAsyncFileSystem* filesystem() override
        {
            return m_fs;
        }
        OVERRIDE_ASYNC(ssize_t, read, void *buf, size_t count)
        {
            PERFORM(OPID_READ, m_file->read(buf, count));
        }
        OVERRIDE_ASYNC(ssize_t, readv, const struct iovec *iov, int iovcnt)
        {
            PERFORM(OPID_READV, m_file->readv(iov, iovcnt));
        }
        OVERRIDE_ASYNC(ssize_t, write, const void *buf, size_t count)
        {
            PERFORM(OPID_WRITE, m_file->write(buf, count));
        }
        OVERRIDE_ASYNC(ssize_t, writev, const struct iovec *iov, int iovcnt)
        {
            PERFORM(OPID_WRITEV, m_file->writev(iov, iovcnt));
        }
        OVERRIDE_ASYNC(ssize_t, pread, void *buf, size_t count, off_t offset)
        {
            PERFORM(OPID_PREAD, m_file->pread(buf, count, offset));
        }
        OVERRIDE_ASYNC(ssize_t, preadv, const struct iovec *iov, int iovcnt, off_t offset)
        {
            PERFORM(OPID_PREADV, m_file->preadv(iov, iovcnt, offset));
        }
        OVERRIDE_ASYNC(ssize_t, preadv2, const struct iovec *iov, int iovcnt, off_t offset, int flags)
        {
            PERFORM(OPID_PREADV2, m_file->preadv2(iov, iovcnt, offset, flags));
        }
        OVERRIDE_ASYNC(ssize_t, pwrite, const void *buf, size_t count, off_t offset)
        {
            PERFORM(OPID_PWRITE, m_file->pwrite(buf, count, offset));
        }
        OVERRIDE_ASYNC(ssize_t, pwritev, const struct iovec *iov, int iovcnt, off_t offset)
        {
            PERFORM(OPID_PWRITEV, m_file->pwritev(iov, iovcnt, offset));
        }
        OVERRIDE_ASYNC(ssize_t, pwritev2, const struct iovec *iov, int iovcnt, off_t offset, int flags)
        {
            PERFORM(OPID_PWRITEV2, m_file->pwritev2(iov, iovcnt, offset, flags));
        }
        OVERRIDE_ASYNC(off_t, lseek, off_t offset, int whence)
        {
            PERFORM(OPID_LSEEK, m_file->lseek(offset, whence));
        }
        OVERRIDE_ASYNC0(int, fsync)
        {
            PERFORM(OPID_FSYNC, m_file->fsync());
        }
        OVERRIDE_ASYNC0(int, fdatasync)
        {
            PERFORM(OPID_FDATASYNC, m_file->fdatasync());
        }
        OVERRIDE_ASYNC0(int, close)
        {
            PERFORM(OPID_CLOSE, m_file->close());
        }
        OVERRIDE_ASYNC(int, fchmod, mode_t mode)
        {
            PERFORM(OPID_FCHMOD, m_file->fchmod(mode));
        }
        OVERRIDE_ASYNC(int, fchown, uid_t owner, gid_t group)
        {
            PERFORM(OPID_FCHOWN, m_file->fchown(owner, group));
        }
        OVERRIDE_ASYNC(int, fstat, struct stat *buf)
        {
            PERFORM(OPID_FSTAT, m_file->fstat(buf));
        }
        OVERRIDE_ASYNC(int, ftruncate, off_t length)
        {
            PERFORM(OPID_FTRUNCATE, m_file->ftruncate(length));
        }
        OVERRIDE_ASYNC(ssize_t, do_appendv, const struct iovec* iov, int iovcnt, off_t* offset, off_t* position)
        {
            PERFORM(OPID_APPENDV, m_file->do_appendv(iov, iovcnt, offset, position));
        }
        template<typename R>
        struct AsyncWaiter
        {
            std::mutex _mtx;
            std::unique_lock<std::mutex> _lock;
            std::condition_variable _cond;
            bool _got_it = false;
            typename AsyncResult<R>::result_type ret;
            int err = 0;

            AsyncWaiter() : _lock(_mtx) { }
            int on_done(AsyncResult<R>* r)
            {
                std::lock_guard<std::mutex> lock(_mtx);
                ret = r->result;
                err = r->error_number;
                _got_it = true;
                _cond.notify_all();
                return 0;
            }
            Done<R> done()
            {
                return {this, &AsyncWaiter<R>::on_done};
            }
            R wait()
            {
                while(!_got_it)
                    _cond.wait(_lock, [this]{return _got_it;});
                if (err) errno = err;
                return (R)ret;
            }
        };
        virtual int vioctl(int request, va_list args) override
        {
            AsyncWaiter<int> w;
            auto done = w.done();
            uint64_t timeout = -1;
            PERFORM(OPID_VIOCTL, m_file->vioctl(request, args));
            return w.wait();
        }
    };
    class ExportAsAsyncDIR : public ExportBase, public AsyncDIR
    {
    public:
        DIR* m_dirp;
        ExportAsAsyncDIR(DIR* dirp) : m_dirp(dirp) { }
        virtual ~ExportAsAsyncDIR() {
            safe_delete(m_dirp);
        }
        OVERRIDE_ASYNC0(int, closedir)
        {
            PERFORM(OPID_CLOSEDIR, m_dirp->closedir());
        }
        OVERRIDE_ASYNC0(dirent*, get)
        {
            PERFORM(OPID_GETDIR, m_dirp->get());
        }
        OVERRIDE_ASYNC0(int, next)
        {
            PERFORM(OPID_NEXTDIR, m_dirp->next());
        }
        int _rewinddir()
        {
            m_dirp->rewinddir();
            return 0;
        }
        OVERRIDE_ASYNC0(void, rewinddir)
        {
            PERFORM(OPID_REWINDDIR, _rewinddir());
        }
        int _seekdir(long loc)
        {
            m_dirp->seekdir(loc);
            return 0;
        }
        OVERRIDE_ASYNC(void, seekdir, long loc)
        {
            PERFORM(OPID_SEEKDIR, _seekdir(loc));
        }
        OVERRIDE_ASYNC0(long, telldir)
        {
            PERFORM(OPID_TELLDIR, m_dirp->telldir());
        }
    };
    class ExportAsAsyncFS : public ExportBase, public IAsyncFileSystem
    {
    public:
        IFileSystem* m_fs;
        ExportAsAsyncFS(IFileSystem* fs) : m_fs(fs)
        {
        }

        virtual ~ExportAsAsyncFS() {
            safe_delete(m_fs);
        }
        IAsyncFile* wrap(IFile* file)
        {
            return !file ? nullptr :
            new ExportAsAsyncFile(file, this);
        }
        OVERRIDE_ASYNC(IAsyncFile*, open, const char *pathname, int flags)
        {
            PERFORM(OPID_OPEN, wrap(m_fs->open(pathname, flags)));
        }
        OVERRIDE_ASYNC(IAsyncFile*, open, const char *pathname, int flags, mode_t mode)
        {
            PERFORM(OPID_OPEN, wrap(m_fs->open(pathname, flags, mode)));
        }
        OVERRIDE_ASYNC(IAsyncFile*, creat, const char *pathname, mode_t mode)
        {
            PERFORM(OPID_CREATE, wrap(m_fs->creat(pathname, mode)));
        }
        OVERRIDE_ASYNC(int, mkdir, const char *pathname, mode_t mode)
        {
            PERFORM(OPID_MKDIR, m_fs->mkdir(pathname, mode));
        }
        OVERRIDE_ASYNC(int, rmdir, const char *pathname)
        {
            PERFORM(OPID_RMDIR, m_fs->rmdir(pathname));
        }
        OVERRIDE_ASYNC(int, symlink, const char *oldname, const char *newname)
        {
            PERFORM(OPID_SYMLINK, m_fs->symlink(oldname, newname));
        }
        OVERRIDE_ASYNC(ssize_t, readlink, const char *path, char *buf, size_t bufsiz)
        {
            PERFORM(OPID_READLINK, m_fs->readlink(path, buf, bufsiz));
        }
        OVERRIDE_ASYNC(int, link, const char *oldname, const char *newname)
        {
            PERFORM(OPID_LINK, m_fs->link(oldname, newname));
        }
        OVERRIDE_ASYNC(int, rename, const char *oldname, const char *newname)
        {
            PERFORM(OPID_RENAME, m_fs->rename(oldname, newname));
        }
        OVERRIDE_ASYNC(int, unlink, const char *filename)
        {
            PERFORM(OPID_UNLINK, m_fs->unlink(filename));
        }
        OVERRIDE_ASYNC(int, chmod, const char *pathname, mode_t mode)
        {
            PERFORM(OPID_CHMOD, m_fs->chmod(pathname, mode));
        }
        OVERRIDE_ASYNC(int, chown, const char *pathname, uid_t owner, gid_t group)
        {
            PERFORM(OPID_CHOWN, m_fs->chown(pathname, owner, group));
        }
        OVERRIDE_ASYNC(int, lchown, const char *pathname, uid_t owner, gid_t group)
        {
            PERFORM(OPID_LCHOWN, m_fs->lchown(pathname, owner, group));
        }
        OVERRIDE_ASYNC(int, statfs, const char *path, struct statfs *buf)
        {
            PERFORM(OPID_STATFS, m_fs->statfs(path, buf));
        }
        OVERRIDE_ASYNC(int, statvfs, const char *path, struct statvfs *buf)
        {
            PERFORM(OPID_STATVFS, m_fs->statvfs(path, buf));
        }
        OVERRIDE_ASYNC(int, stat, const char *path, struct stat *buf)
        {
            PERFORM(OPID_STAT, m_fs->stat(path, buf));
        }
        OVERRIDE_ASYNC(int, lstat, const char *path, struct stat *buf)
        {
            PERFORM(OPID_LSTAT, m_fs->lstat(path, buf));
        }
        OVERRIDE_ASYNC(int, access, const char *pathname, int mode)
        {
            PERFORM(OPID_ACCESS, m_fs->access(pathname, mode));
        }
        OVERRIDE_ASYNC(int, truncate, const char *path, off_t length)
        {
            PERFORM(OPID_TRUNCATE, m_fs->truncate(path, length));
        }
        OVERRIDE_ASYNC0(int, syncfs)
        {
            PERFORM(OPID_SYNCFS, m_fs->syncfs());
        }
        AsyncDIR* wrap(DIR* dirp)
        {
            return !dirp ? nullptr :
            new ExportAsAsyncDIR(dirp);
        }
        OVERRIDE_ASYNC(AsyncDIR*, opendir, const char *name)
        {
            PERFORM(OPID_OPENDIR, wrap(m_fs->opendir(name)));
        }
    };
    int exportfs_init(uint32_t thread_pool_capacity)
    {
        if (photon::CURRENT == nullptr) {
            LOG_ERROR_RETURN(ENOSYS, -1, "photon not initialized");
        }
        if (evloop)
            LOG_ERROR_RETURN(EALREADY, -1, "already inited");

        evloop = new_event_loop({nullptr, &ExportBase::wait4events},
                                {nullptr, &ExportBase::on_events});
        if (!evloop)
            // currently it will never trigger this branch
            // cause the new_event_loop just return `new EventLoopImpl(wait, on_event)`
            // when something wrong happend in the constructor
            // an Exception will be thrown.
            LOG_ERROR_RETURN(EFAULT, -1, "failed to create event loop");

        ExportBase::ref = 1;
        if (thread_pool_capacity != 0) ExportBase::pool = new_thread_pool(thread_pool_capacity);
        evloop->async_run();
        return 0;
    }
    int exportfs_fini()
    {
        if (photon::CURRENT == nullptr) {
            LOG_ERROR_RETURN(ENOSYS, -1, "photon not initialized");
        }
        if (!evloop)
            LOG_ERROR_RETURN(ENOSYS, -1, "not inited yet");

        evloop->stop();
        --ExportBase::ref;
        while (ExportBase::ref != 0)
        {
            ExportBase::cond.wait_no_lock();
        }
        safe_delete(evloop);
        evloop = nullptr;
        if (ExportBase::pool != nullptr) delete_thread_pool(ExportBase::pool);
        ExportBase::pool = nullptr;
        ExportBase::op_queue.reset();
        return 0;
    }
    IAsyncFile* export_as_async_file(IFile* file)
    {
        return new ExportAsAsyncFile(file, nullptr);
    }
    IAsyncFileSystem* export_as_async_fs(IFileSystem* fs)
    {
        return new ExportAsAsyncFS(fs);
    }
    AsyncDIR* export_as_async_dir(DIR* dir)
    {
        return new ExportAsAsyncDIR(dir);
    }
}
}
