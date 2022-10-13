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

#include "async_filesystem.h"

#include <cinttypes>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <type_traits>

#include <photon/thread/thread.h>
#include <photon/io/fd-events.h>
#include <photon/common/utility.h>

#include "filesystem.h"
#include "exportfs.h"
#include "../third_party/easy_weak/easy_weak.h"

using namespace std;

// Concepts
// Performer: a facility that performs the intended operation in background, without blocking photon
// ThPerformer, th_performer: perform operations by creating kernel threads (TODO: thread pool)
// AsyncPerformer: performs async operations that are executed in other kernel threads
// UIF: Underlay Interface ---- the IF to be wrapped

namespace photon {
namespace fs
{   // threaded performer, for general functions
    template<typename R>
    struct th_performer
    {
        template<typename Obj, typename Func, typename...ARGS>
        R call(Obj* obj, Func func, ARGS...args)
        {
            R ret;
            auto th = CURRENT;
            auto lambda0 = [=, &ret] ()
            {
                ret = (obj->*func)(args...);
                thread_interrupt(th, EINTR);
            };
            do_call(lambda0);
            return ret;
        }
        template<typename F>
        void do_call(F& f)
        {   // use another lambda to minimize args passing
            photon::thread_usleep_defer(-1UL, [&]{std::thread([&](){ f(); }).detach();});
        }
    };

    template<>  // threaded performer, for void functions
    struct th_performer<void> : public th_performer<int>
    {
        template<typename Obj, typename Func, typename...ARGS>
        void call(Obj* obj, Func func, ARGS...args)
        {
            auto th = CURRENT;
            auto lambda0 = [=]()
            {
                (obj->*func)(args...);
                thread_interrupt(th, EINTR);
            };
            do_call(lambda0);
        }
    };

    // threaded performer       TODO: thread pool
    class ThPerformer
    {
    public:
        template<typename IF, typename Func, typename...ARGS,
            typename R = typename std::result_of<Func(IF*, ARGS...)>::type >
        R perform(IF* _if, Func func, ARGS...args)
        {
            return th_performer<R>().call(_if, func, args...);
        }
    };

    // Async performer
    class AsyncPerformer
    {
    public:
        AsyncPerformer() = default;
        AsyncPerformer(uint64_t timeout) : _timeout(timeout) { }

        uint64_t _timeout;

        template<typename IF, typename Func, typename...ARGS,
            typename R = typename af_traits<Func>::result_type >
        R perform(IF* _if, Func func, ARGS...args)
        {
            using AF = AsyncFuncWrapper<R, IF, ARGS...>;
            return AF(_if, func, _timeout).call(args...);
        }

        template<typename IF, typename Func, typename...ARGS,
            typename R = typename af_traits<Func>::result_type >
        R perform2(IF* _if, Func func, ARGS...args)
        {
            AsyncFuncWrapper_Generic<R> af;
            auto done = [&](AsyncResult<R>* ar)
            {
                return af.put_result(ar->result, ar->error_number), 0;
            };
            return af.call([&](){ return (_if->*func)(args..., done, _timeout);});
        }
    };

    class ExportPerformer
    {
    public:
        ExportPerformer() = default;
        ExportPerformer(uint64_t timeout) : _timeout(timeout) { }

        uint64_t _timeout;

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

        template<typename IF, typename Func, typename...ARGS,
            typename R = typename af_traits<Func>::result_type >
        R perform(IF* _if, Func func, ARGS...args)
        {
            AsyncWaiter<R> w;
            (_if->*func)(args..., w.done(), _timeout);
            return w.wait();
        }
    };

    class EasyPerformer {
    public:
        EasyPerformer() = default;
        EasyPerformer(uint64_t timeout) : _timeout(timeout) { }

        uint64_t _timeout;
        template<typename R>
        struct AsyncWaiter
        {
            easy_comutex_t _mtx;
            std::mutex smtx;
            bool _got_it = false;
            typename AsyncResult<R>::result_type ret;
            AsyncWaiter() { easy_comutex_init(&_mtx); }
            int on_done(AsyncResult<R>* r)
            {
                std::lock_guard<std::mutex> lock(smtx);
                ret = r->result;
                _got_it = true;
                easy_comutex_cond_signal(&_mtx);
                return 0;
            }
            Done<R> done()
            {
                return {this, &AsyncWaiter<R>::on_done};
            }
            R wait()
            {
                std::lock_guard<std::mutex> lock(smtx);
                while(!_got_it) {
                    smtx.unlock();
                    easy_comutex_cond_timedwait(&_mtx, 100);
                    smtx.lock();
                }
                return (R)ret;
            }
        };

        template<typename IF, typename Func, typename...ARGS,
            typename R = typename af_traits<Func>::result_type >
        R perform(IF* _if, Func func, ARGS...args)
        {
            AsyncWaiter<R> w;
            (_if->*func)(args..., w.done(), _timeout);
            return w.wait();
        }
    };

    template<typename UIF>
    class Adaptor
    {
    public:
        UIF* _uif;  // Underlay Interface
        ~Adaptor() { delete _uif; }
    };

    #define PERFORM0(func) \
        _p.perform(_uif, &UIF::func)
    #define PERFORM(func, ...) \
        _p.perform(_uif, &UIF::func, __VA_ARGS__)

    template<typename UIF, typename Performer>
    class FileAdaptor : public Adaptor<UIF>, public IFile
    {
    public:
        using Adaptor<UIF>::_uif;
        IFileSystem* _fs;
        Performer _p;
        template<typename...Ts>
        FileAdaptor(UIF* file, IFileSystem* fs, Ts...xs) :
            Adaptor<UIF>{file}, _fs(fs), _p(xs...) { }
        virtual IFileSystem* filesystem() override
        {
            return _fs;
        }
        virtual Object* get_underlay_object(int i) override
        {
            return _uif;
        }
        virtual int close() override
        {
            return PERFORM0(close);
        }
        virtual ssize_t read(void *buf, size_t count) override
        {
            return PERFORM(read, buf, count);
        }
        virtual ssize_t readv(const struct iovec *iov, int iovcnt) override
        {
            return PERFORM(readv, iov, iovcnt);
        }
        virtual ssize_t readv_mutable(struct iovec *iov, int iovcnt) override
        {
            return PERFORM(readv_mutable, iov, iovcnt);
        }
        virtual ssize_t write(const void *buf, size_t count) override
        {
            return PERFORM(write, buf, count);
        }
        virtual ssize_t writev(const struct iovec *iov, int iovcnt) override
        {
            return PERFORM(writev, iov, iovcnt);
        }
        virtual ssize_t writev_mutable(struct iovec *iov, int iovcnt) override
        {
            return PERFORM(writev_mutable, iov, iovcnt);
        }
        virtual ssize_t pread(void *buf, size_t count, off_t offset) override
        {
            return PERFORM(pread, buf, count, offset);
        }
        virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override
        {
            return PERFORM(pwrite, buf, count, offset);
        }
        virtual ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override
        {
            return PERFORM(preadv, iov, iovcnt, offset);
        }
        virtual ssize_t preadv_mutable(struct iovec *iov, int iovcnt, off_t offset) override
        {
            return PERFORM(preadv_mutable, iov, iovcnt, offset);
        }
        virtual ssize_t preadv2(const struct iovec *iov, int iovcnt, off_t offset, int flags) override
        {
            return PERFORM(preadv2, iov, iovcnt, offset, flags);
        }
        virtual ssize_t preadv2_mutable(struct iovec *iov, int iovcnt, off_t offset, int flags) override
        {
            return PERFORM(preadv2_mutable, iov, iovcnt, offset, flags);
        }
        virtual ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override
        {
            return PERFORM(pwritev, iov, iovcnt, offset);
        }
        virtual ssize_t pwritev_mutable(struct iovec *iov, int iovcnt, off_t offset) override
        {
            return PERFORM(pwritev_mutable, iov, iovcnt, offset);
        }
        virtual ssize_t pwritev2(const struct iovec *iov, int iovcnt, off_t offset, int flags) override
        {
            return PERFORM(pwritev2, iov, iovcnt, offset, flags);
        }
        virtual ssize_t pwritev2_mutable(struct iovec *iov, int iovcnt, off_t offset, int flags) override
        {
            return PERFORM(pwritev2_mutable, iov, iovcnt, offset, flags);
        }
        virtual off_t lseek(off_t offset, int whence) override
        {
            return PERFORM(lseek, offset, whence);
        }
        virtual int fstat(struct stat* buf) override
        {
            return PERFORM(fstat, buf);
        }
        virtual int fsync() override
        {
            return PERFORM0(fsync);
        }
        virtual int fdatasync() override
        {
            return PERFORM0(fdatasync);
        }
        virtual int sync_file_range(off_t offset, off_t nbytes, unsigned int flags) override
        {
            return PERFORM(sync_file_range, offset, nbytes, flags);
        }
        virtual int fchmod(mode_t mode) override
        {
            return PERFORM(fchmod, mode);
        }
        virtual int fchown(uid_t owner, gid_t group) override
        {
            return PERFORM(fchown, owner, group);
        }
        virtual int ftruncate(off_t length) override
        {
            return PERFORM(ftruncate, length);
        }
        virtual int fallocate(int mode, off_t offset, off_t len) override
        {
            return PERFORM(fallocate, mode, offset, len);
        }
        virtual ssize_t do_appendv(const struct iovec *iov, int iovcnt,
                                      off_t*  /*IN*/ offset = nullptr,
                                      off_t* /*OUT*/ position = nullptr) override
        {
            return PERFORM(do_appendv, iov, iovcnt, offset, position);
        }
        virtual int vioctl(int request, va_list args) override
        {
            return _uif->vioctl(request, args);
        }
    };

    template<typename UIF, typename Base>
    class FileXattrAdaptor : public Base, public IFileXAttr
    {
    public:
        UIF* _uif;
        using Base::_p;
        template<typename...Ts>
        FileXattrAdaptor(UIF* xattr, Ts...xs) : Base(xs...), _uif(xattr) { }
        virtual ssize_t fgetxattr(const char *name, void *value, size_t size) override
        {
            return PERFORM(fgetxattr, name, value, size);
        }
        virtual ssize_t flistxattr(char *list, size_t size) override
        {
            return PERFORM(flistxattr, list, size);
        }
        virtual int fsetxattr(const char *name, const void *value, size_t size, int flags) override
        {
            return PERFORM(fsetxattr, name, value, size, flags);
        }
        virtual int fremovexattr(const char *name) override
        {
            return PERFORM(fremovexattr, name);
        }
    };

    template<typename UIF, typename Performer>
    class DIRAdaptor : public Adaptor<UIF>, public DIR
    {
    public:
        using Adaptor<UIF>::_uif;
        Performer _p;
        template<typename...Ts>
        DIRAdaptor(UIF* dir, Ts...xs) : Adaptor<UIF>{dir}, _p(xs...) { }
        virtual Object* get_underlay_object(int i) override
        {
            return _uif;
        }
        virtual int closedir() override
        {
            return PERFORM0(closedir);
        }
        virtual dirent* get() override
        {
            return PERFORM0(get);
        }
        virtual int next() override
        {
            return PERFORM0(next);
        }
        virtual void rewinddir() override
        {
            return PERFORM0(rewinddir);
        }
        virtual void seekdir(long loc) override
        {
            return PERFORM(seekdir, loc);
        }
        virtual long telldir() override
        {
            return PERFORM0(telldir);
        }
    };

    template<typename FS>
    struct fstraits;

    template<>
    struct fstraits<IFileSystem>
    {
        using file_type = IFile;
        using dir_type = DIR;
        using file_xattr_type = IFileXAttr;
        using fs_xattr_type = IFileSystemXAttr;
        using open2_type = IFile* (IFileSystem::*)(const char*, int);
        using open3_type = IFile* (IFileSystem::*)(const char*, int, mode_t);
        constexpr static open2_type open2 = &IFileSystem::open;
        constexpr static open3_type open3 = &IFileSystem::open;
    };
    template<>
    struct fstraits<IFile> : fstraits<IFileSystem> { };

    template<>
    struct fstraits<IAsyncFileSystem>
    {
        using file_type = IAsyncFile;
        using dir_type = AsyncDIR;
        using file_xattr_type = IAsyncFileXAttr;
        using fs_xattr_type = IAsyncFileSystemXAttr;
        using open2_type = AsyncFunc<IAsyncFile*, IAsyncFileSystem, const char*, int>;
        using open3_type = AsyncFunc<IAsyncFile*, IAsyncFileSystem, const char*, int, mode_t>;
        constexpr static open2_type open2 = &IAsyncFileSystem::open;
        constexpr static open3_type open3 = &IAsyncFileSystem::open;
    };

    template<>
    struct fstraits<IAsyncFile> : fstraits<IAsyncFileSystem> { };

    template<typename UIF, typename Performer>
    class FSAdaptor : public Adaptor<UIF>, public IFileSystem
    {
    public:
        using Adaptor<UIF>::_uif;
        Performer _p;
        template<typename...Ts>
        FSAdaptor(UIF* fs, Ts...xs) : Adaptor<UIF>{fs}, _p(xs...) { }
        virtual Object* get_underlay_object(int i) override
        {
            return _uif;
        }
        template<typename T, typename Obj, typename...ARGS>
        T* new_adaptor(Obj* obj, ARGS...args)
        {
            return obj ? new T(obj, args..., _p) : nullptr;
        }
        using performer_type = Performer;
        using ufile_type = typename fstraits<UIF>::file_type;
        using udir_type = typename fstraits<UIF>::dir_type;
        virtual IFile* open(const char *pathname, int flags) override
        {
            auto open2 = fstraits<UIF>::open2;
            auto file = _p.perform(_uif, open2, pathname, flags);
            using A = FileAdaptor<ufile_type, Performer>;
            return new_adaptor<A>(file, this);
        }
        virtual IFile* open(const char *pathname, int flags, mode_t mode) override
        {
            auto open3 = fstraits<UIF>::open3;
            auto file = _p.perform(_uif, open3, pathname, flags, mode);
            using A = FileAdaptor<ufile_type, Performer>;
            return new_adaptor<A>(file, this);
        }
        virtual IFile* creat(const char *pathname, mode_t mode) override
        {
            auto file = PERFORM(creat, pathname, mode);
            using A = FileAdaptor<ufile_type, Performer>;
            return new_adaptor<A>(file, this);
        }
        virtual int mkdir (const char *pathname, mode_t mode) override
        {
            return PERFORM(mkdir, pathname, mode);
        }
        virtual int rmdir(const char *pathname) override
        {
            return PERFORM(rmdir, pathname);
        }
        virtual int symlink (const char *oldname, const char *newname) override
        {
            return PERFORM(symlink, oldname, newname);
        }
        virtual ssize_t readlink(const char *pathname, char *buf, size_t bufsiz) override
        {
            return PERFORM(readlink, pathname, buf, bufsiz);
        }
        virtual int link(const char *oldname, const char *newname) override
        {
            return PERFORM(link, oldname, newname);
        }
        virtual int rename (const char *oldname, const char *newname) override
        {
            return PERFORM(rename, oldname, newname);
        }
        virtual int unlink (const char *pathname) override
        {
            return PERFORM(unlink, pathname);
        }
        virtual int chmod(const char *pathname, mode_t mode) override
        {
            return PERFORM(chmod, pathname, mode);
        }
        virtual int chown(const char *pathname, uid_t owner, gid_t group) override
        {
            return PERFORM(chown, pathname, owner, group);
        }
        virtual int lchown(const char *pathname, uid_t owner, gid_t group) override
        {
            return PERFORM(lchown, pathname, owner, group);
        }
        virtual DIR* opendir(const char *pathname) override
        {
            auto dir = PERFORM(opendir, pathname);
            using A = DIRAdaptor<udir_type, Performer>;
            return new_adaptor<A>(dir);
        }
        virtual int stat(const char *path, struct stat *buf) override
        {
            return PERFORM(stat, path, buf);
        }
        virtual int lstat(const char *path, struct stat *buf) override
        {
            return PERFORM(lstat, path, buf);
        }
        virtual int access(const char *path, int mode) override
        {
            return PERFORM(access, path, mode);
        }
        virtual int truncate(const char *path, off_t length) override
        {
            return PERFORM(truncate, path, length);
        }
        virtual int syncfs() override
        {
            return PERFORM0(syncfs);
        }
        virtual int statfs(const char *path, struct statfs *buf) override
        {
            return PERFORM(statfs, path, buf);
        }
        virtual int statvfs(const char *path, struct statvfs *buf) override
        {
            return PERFORM(statvfs, path, buf);
        }
        virtual int utime(const char *path, const struct utimbuf *file_times) override
        {
            return PERFORM(utime, path, file_times);
        }
        virtual int utimes(const char *path, const struct timeval times[2]) override
        {
            return PERFORM(utimes, path, times);
        }
        virtual int lutimes(const char *path, const struct timeval times[2]) override
        {
            return PERFORM(lutimes, path, times);
        }
        virtual int mknod(const char *path, mode_t mode, dev_t dev) override
        {
            return PERFORM(mknod, path, mode, dev);
        }
    };

    template<typename UIF, typename Performer, typename...Timeout>
    static IFile* _new_file_adaptor(UIF* file, IFileSystem* fs, Timeout...timeout)
    {
        if (!file) return nullptr;
        using XATTR = typename fstraits<UIF>::file_xattr_type;
        auto xattr = dynamic_cast<XATTR*>(file);
        using FA  = FileAdaptor<UIF, Performer>;
        using FXA = FileXattrAdaptor<XATTR, FA>;
        return xattr ? new FXA(xattr, file, fs, timeout...) :
                       new FA(file, fs, timeout...) ;
    }

    template<typename UIF, typename Base>
    class FSXAttrAdaptor : public Base, public IFileSystemXAttr
    {
    public:
        UIF* _uif;
        using Base::_p;
        using BUIF = typename std::remove_pointer<decltype(Base::_uif)>::type;
        using ufile_type = typename fstraits<BUIF>::file_type;
        using ufilex_type = typename fstraits<BUIF>::file_xattr_type;
        using udir_type = typename fstraits<BUIF>::dir_type;
        using Performer = typename Base::performer_type;
        template<typename...Ts>
        FSXAttrAdaptor(UIF* fsxattr, Ts...xs) : Base(xs...), _uif(fsxattr) { }

        IFile* new_file_xattr_adaptor(ufile_type* file)
        {
            return _new_file_adaptor<ufile_type, Performer>(file, this, _p);
        }
        virtual IFile* open(const char *pathname, int flags) override
        {
            auto open2 = fstraits<BUIF>::open2;
            auto file = _p.perform(Base::_uif, open2, pathname, flags);
            return new_file_xattr_adaptor(file);
        }
        virtual IFile* open(const char *pathname, int flags, mode_t mode) override
        {
            auto open3 = fstraits<BUIF>::open3;
            auto file = _p.perform(Base::_uif, open3, pathname, flags, mode);
            return new_file_xattr_adaptor(file);
        }
        virtual IFile* creat(const char *pathname, mode_t mode) override
        {
            auto file = _p.perform(Base::_uif, &BUIF::creat, pathname, mode);
            return new_file_xattr_adaptor(file);
        }
        virtual ssize_t getxattr(const char *path, const char *name, void *value, size_t size) override
        {
            return PERFORM(getxattr, path, name, value, size);
        }
        virtual ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size) override
        {
            return PERFORM(lgetxattr, path, name, value, size);
        }
        virtual ssize_t listxattr(const char *path, char *list, size_t size) override
        {
            return PERFORM(listxattr, path, list, size);
        }
        virtual ssize_t llistxattr(const char *path, char *list, size_t size) override
        {
            return PERFORM(llistxattr, path, list, size);
        }
        virtual int setxattr(const char *path, const char *name, const void *value, size_t size, int flags) override
        {
            return PERFORM(setxattr, path, name, value, size, flags);
        }
        virtual int lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags) override
        {
            return PERFORM(lsetxattr, path, name, value, size, flags);
        }
        virtual int removexattr(const char *path, const char *name) override
        {
            return PERFORM(removexattr, path, name);
        }
        virtual int lremovexattr(const char *path, const char *name) override
        {
            return PERFORM(lremovexattr, path, name);
        }
    };

    IFile* new_async_file_adaptor(IAsyncFile* afile, uint64_t timeout)
    {
        return _new_file_adaptor<IAsyncFile, AsyncPerformer>(afile, nullptr, timeout);
    }
    IFile* new_sync_file_adaptor(IFile* file)
    {
        return _new_file_adaptor<IFile, ThPerformer>(file, nullptr);
    }

    template<typename UIF, typename Performer, typename...Timeout>
    static IFileSystem* _new_fs_adaptor(UIF* fs, Timeout...timeout)
    {
        if (!fs) return nullptr;
        using XATTR = typename fstraits<UIF>::fs_xattr_type;
        auto xattr = dynamic_cast<XATTR*>(fs);
        using FSA  = FSAdaptor<UIF, Performer>;
        using FSXA = FSXAttrAdaptor<XATTR, FSA>;
        return xattr ? new FSXA(xattr, fs, timeout...) :
                       new FSA(fs, timeout...) ;
    }
    IFileSystem* new_async_fs_adaptor(IAsyncFileSystem* afs, uint64_t timeout)
    {
        return _new_fs_adaptor<IAsyncFileSystem, AsyncPerformer>(afs, timeout);
    }
    IFileSystem* new_sync_fs_adaptor(IFileSystem* fs)
    {
        return _new_fs_adaptor<IFileSystem, ThPerformer>(fs);
    }

    template<typename UIF, typename Performer, typename...Timeout>
    static DIR* _new_dir_adaptor(UIF* dir, Timeout...timeout)
    {
        using DA = DIRAdaptor<UIF, Performer>;
        return dir ? new DA(dir, timeout...) : nullptr;
    }
    DIR* new_async_dir_adaptor(AsyncDIR* adir, uint64_t timeout)
    {
        return _new_dir_adaptor<AsyncDIR, AsyncPerformer>(adir, timeout);
    }
    DIR* new_sync_dir_adaptor(DIR* dir)
    {
        return _new_dir_adaptor<DIR, ThPerformer>(dir);
    }

    IFile* export_as_sync_file(IFile* file)
    {
        auto afile = export_as_async_file(file);
        return _new_file_adaptor<IAsyncFile, ExportPerformer>(afile, nullptr, -1);
    }
    IFileSystem* export_as_sync_fs(IFileSystem* fs)
    {
        auto afs = export_as_async_fs(fs);
        return _new_fs_adaptor<IAsyncFileSystem, ExportPerformer>(afs, -1);
    }
    DIR* export_as_sync_dir(DIR* dir)
    {
        auto adir = export_as_async_dir(dir);
        return _new_dir_adaptor<AsyncDIR, ExportPerformer>(adir, -1);
    }

    IFile* export_as_easy_sync_file(IFile* file)
    {
        auto afile = export_as_async_file(file);
        return _new_file_adaptor<IAsyncFile, EasyPerformer>(afile, nullptr, -1);
    }
    IFileSystem* export_as_easy_sync_fs(IFileSystem* fs)
    {
        auto afs = export_as_async_fs(fs);
        return _new_fs_adaptor<IAsyncFileSystem, EasyPerformer>(afs, -1);
    }
    DIR* export_as_easy_sync_dir(DIR* dir)
    {
        auto adir = export_as_async_dir(dir);
        return _new_dir_adaptor<AsyncDIR, EasyPerformer>(adir, -1);
    }
}
}
