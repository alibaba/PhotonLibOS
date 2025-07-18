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

#include "rpc.h"
#include "out-of-order-execution.h"
#include <unordered_map>
#include <netinet/tcp.h>
#include <photon/thread/thread11.h>
#include <photon/thread/thread-pool.h>
#include <photon/common/intrusive_list.h>
#include <photon/common/utility.h>
#include <photon/common/alog.h>
#include <photon/common/timeout.h>
#include <photon/common/expirecontainer.h>
#include <photon/net/socket.h>
#include <photon/net/security-context/tls-stream.h>

using namespace std;

namespace photon {
namespace rpc {

    class StubImpl : public Stub
    {
    public:
        Header m_header;
        IStream* m_stream;
        OutOfOrder_Execution_Engine* m_engine = new_ooo_execution_engine();
        bool m_ownership;
        photon::rwlock m_rwlock;
        StubImpl(IStream* s, bool ownership = false) :
            m_stream(s), m_ownership(ownership) { }
        virtual ~StubImpl() override
        {
            delete_ooo_execution_engine(m_engine);
            if (m_ownership) delete m_stream;
        }

        IStream* get_stream() override {
            return m_stream;
        }
        int set_stream(IStream* stream) override {
            scoped_rwlock wl(m_rwlock, photon::WLOCK);
            if (m_ownership)
                delete m_stream;
            m_stream = stream;
            return 0;
        }
        int get_queue_count() override {
            return ooo_get_queue_count(m_engine);
        }
        int do_send(OutOfOrderContext* args_)
        {
            auto args = (OooArgs*)args_;
            if (args->timeout.expiration() < photon::now) {
                LOG_ERROR_RETURN(ETIMEDOUT, -1, "Request timedout before send");
            }
            auto size = args->request->sum();
            if (size > UINT32_MAX)
                LOG_ERROR_RETURN(EINVAL, -1, "request size(`) toooo big!", size);

            Header header;
            header.function = args->function;
            header.size = (uint32_t)size;
            header.tag = args->tag;

            auto iov = args->request;
            auto ret = iov->push_front({&header, sizeof(header)});
            if (ret != sizeof(header)) return -1;
            m_stream->timeout(args->timeout.timeout());
            DEFER(m_stream->timeout(-1));
            ret = args->RET = m_stream->writev(iov->iovec(), iov->iovcnt());
            if (ret != header.size + sizeof(header)) {
                ERRNO err;
                m_stream->shutdown(ShutdownHow::ReadWrite);
                LOG_ERROR_RETURN(ECONNRESET, -1, "Failed to write header ", err);
            }
            return 0;
        }
        int do_recv_header(OutOfOrderContext* args_)
        {
            auto args = (OooArgs*)args_;
            m_header.magic = 0;
            if (args->timeout.expiration() < photon::now) {
                // m_stream->shutdown(ShutdownHow::ReadWrite);
                LOG_ERROR_RETURN(ETIMEDOUT, -1, "Timeout before read header ");
            }
            m_stream->timeout(args->timeout.timeout());
            DEFER(m_stream->timeout(-1));
            auto ret = args->RET = m_stream->read(&m_header, sizeof(m_header));
            args->tag = m_header.tag;
            if (ret != sizeof(m_header)) {
                ERRNO err;
                m_stream->shutdown(ShutdownHow::ReadWrite);
                LOG_ERROR_RETURN(ECONNRESET, -1, "Failed to read header ", err);
            }
            if ((m_header.magic != rpc::Header::MAGIC) || (m_header.version != rpc::Header::VERSION)) {
                // this cannot be a RPC header
                // client is not RPC Client or data has been corrupt
                m_stream->shutdown(ShutdownHow::ReadWrite);
                LOG_ERROR_RETURN(ECONNRESET, -1, "Header check failed");
            }
            return 0; // return 0 means it has been disconnected
        }
        int do_recv_body(OutOfOrderContext* args_)
        {
            auto args = (OooArgs*)args_;
            args->response->truncate(m_header.size);
            auto iov = args->response;
            if (iov->iovcnt() == 0) {
                iov->malloc(m_header.size);
            }
            m_stream->timeout(args->timeout.timeout());
            DEFER(m_stream->timeout(-1));
            auto ret = m_stream->readv((const iovec*)iov->iovec(), iov->iovcnt());
            // return 0 means it has been disconnected
            // should take as fault
            if (ret != m_header.size) {
                ERRNO err;
                m_stream->shutdown(ShutdownHow::ReadWrite);
                LOG_ERROR_RETURN(ECONNRESET, -1, "Failed to read body ", VALUE(ret), VALUE(m_header.size), err);
            }
            return ret;
        }
        struct OooArgs : public OutOfOrderContext
        {
            union
            {
                ssize_t RET;
                FunctionID function;
            };
            iovector *request, *response;
            OooArgs(StubImpl* stub, FunctionID function, iovector* req, iovector* resp, Timeout timeout_)
            {
                request = req;
                response = resp;
                engine = stub->m_engine;
                this->function = function;
                do_issue.bind(stub, &StubImpl::do_send);
                do_completion.bind(stub, &StubImpl::do_recv_header);
                do_collect.bind(stub, &StubImpl::do_recv_body);
                timeout = timeout_;
            }
        };

        int do_call(FunctionID function, iovector* request, iovector* response, Timeout tmo) override {
            scoped_rwlock rl(m_rwlock, photon::RLOCK);
            if (tmo.expiration() < photon::now) {
                LOG_ERROR_RETURN(ETIMEDOUT, -1, "Timed out before rpc start", VALUE(tmo.timeout()));
            }
            int ret = 0;
            OooArgs args(this, function, request, response, tmo.timeout());
            ret = ooo_issue_operation(args);
            if (ret < 0) {
                if (errno != ECONNRESET)
                    errno = EFAULT;
                LOG_ERRNO_RETURN(0, -1, "failed to send request");
            }
            ret = ooo_wait_completion(args);
            if (ret < 0) {
                if (errno != ECONNRESET)
                    errno = EFAULT;
                LOG_ERRNO_RETURN(0, -1, "failed to receive response ");
            } else if (ret > (int) response->sum()) {
                LOG_ERROR_RETURN(0, -1, "RPC: response iov buffer is too small");
            }

            ooo_result_collected(args);
            return ret;
        }
    };
    Stub* new_rpc_stub(IStream* stream, bool ownership)
    {
        if (!stream) return nullptr;
        return new StubImpl(stream, ownership);
    }

    class SkeletonImpl final: public Skeleton
    {
    public:
        unordered_map<uint64_t, Function> m_map;
        virtual int add_function(FunctionID func_id, Function func) override
        {
            auto ret = m_map.insert({func_id, func});
            return ret.second ? 0 : -1;
        }
        virtual int remove_function(FunctionID func_id) override
        {
            auto ret = m_map.erase(func_id);
            return (int)ret - 1;
        }
        Notifier stream_accept_notify, stream_close_notify;
        virtual int set_accept_notify(Notifier notifier) override {
            stream_accept_notify = notifier;
            return 0;
        }
        virtual int set_close_notify(Notifier notifier) override {
            stream_close_notify = notifier;
            return 0;
        }
        IOAlloc m_allocator;
        virtual void set_allocator(IOAlloc allocator) override
        {
            m_allocator = allocator;
        }
        struct Context
        {
            Header header;
            Function func;
            IOVector request;
            IStream* stream;
            SkeletonImpl* sk;
            bool got_it;
            int* stream_serv_count;
            photon::condition_variable *stream_cv;
            photon::mutex* w_lock;

            Context(SkeletonImpl* sk, IStream* s) :
                request(sk->m_allocator), stream(s), sk(sk) { }

            Context(Context&& rhs) : request(std::move(rhs.request))
            {
#define COPY(x) x = rhs.x
                COPY(header.size);
                COPY(header.function);
                COPY(header.tag);
                COPY(func);
                COPY(stream);
                COPY(sk);
                COPY(stream_serv_count);
                COPY(stream_cv);
                COPY(w_lock);
#undef COPY
            }

            int read_request()
            {
                ssize_t ret = stream->read(&header, sizeof(header));
                ERRNO err;
                if (ret == 0) {
                    // means socket already shutted or disconnected
                    // do not needs more logs
                    return -1;
                }
                if (ret != sizeof(header)) {
                    stream->shutdown(ShutdownHow::ReadWrite);
                    LOG_ERROR_RETURN(err.no, -1, "Failed to read rpc header ", stream, VALUE(ret), err);
                    return -1;
                }

                if (header.magic != Header::MAGIC)
                    LOG_ERROR_RETURN(err.no, -1, "header magic doesn't match ", stream);

                if (header.version != Header::VERSION)
                    LOG_ERROR_RETURN(err.no, -1, "protocol version doesn't match ", stream);

                auto it = sk->m_map.find(header.function);
                if (it == sk->m_map.end())
                    LOG_ERROR_RETURN(ENOSYS, -1, "unable to find function service for ID ", header.function.function);

                func = it->second;
                ret = request.push_back(header.size);
                if (ret != header.size) {
                    LOG_ERRNO_RETURN(ENOMEM, -1, "Failed to allocate iov");
                }
                ret = stream->readv(request.iovec(), request.iovcnt());
                ERRNO errbody;
                if (ret != header.size) {
                    stream->shutdown(ShutdownHow::ReadWrite);
                    LOG_ERROR_RETURN(errbody.no, -1, "failed to read rpc request body from stream ", stream, VALUE(ret), errbody);
                }
                return 0;
            }
            int serve_request()
            {
                sk->m_serving_count++;
                ResponseSender sender(this, &Context::response_sender);
                int ret = func(&request, sender, stream);
                sk->m_serving_count--;
                sk->m_cond_served.notify_all();
                return ret;
            }
            int response_sender(iovector* resp)
            {
                assert(w_lock);
                Header h;
                h.size = (uint32_t)resp->sum();
                h.function = header.function;
                h.tag = header.tag;
                h.reserved = 0;
                resp->push_front(&h, sizeof(h));
                if (stream == nullptr)
                    LOG_ERRNO_RETURN(0, -1, "socket closed ");

                w_lock->lock();
                ssize_t ret = stream->writev(resp->iovec(), resp->iovcnt());
                w_lock->unlock();

                if (ret < (ssize_t)(sizeof(h) + h.size)) {
                    stream->shutdown(ShutdownHow::ReadWrite);
                    LOG_ERRNO_RETURN(0, -1, "failed to send rpc response to stream ", stream);
                }
                return 0;
            }
        };
        condition_variable m_cond_served;
        struct ThreadLink : public intrusive_list_node<ThreadLink>
        {
            photon::thread* thread = photon::CURRENT;
        };
        intrusive_list<ThreadLink> m_list;  // Stores the thread ID of every stream
        uint64_t m_serving_count = 0;
        bool m_running = true;
        photon::ThreadPoolBase *m_thread_pool;
        virtual int serve(IStream* stream) override
        {
            if (unlikely(!m_running))
                return -1;

#pragma GCC diagnostic push
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wunknown-warning-option"
#endif
#if __GNUC__ >= 12
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif
            ThreadLink node;
            m_list.push_back(&node);
#pragma GCC diagnostic pop
            DEFER(m_list.erase(&node));
            // stream serve refcount
            int stream_serv_count = 0;
            photon::mutex w_lock;
            photon::condition_variable stream_cv;
            // once serve exit, stream will destruct
            // make sure all requests relies on this stream are finished
            DEFER({
                while (stream_serv_count > 0) stream_cv.wait_no_lock();
            });
            if (stream_accept_notify) stream_accept_notify(stream);
            DEFER(if (stream_close_notify) stream_close_notify(stream));

            while(likely(m_running)) {
                Context context(this, stream);
                context.stream_serv_count = &stream_serv_count;
                context.stream_cv = &stream_cv;
                context.w_lock = &w_lock;
                int ret = context.read_request();
                if (ret < 0) {
                    // should only shutdown read, for other threads
                    // might still writing
                    ERRNO e;
                    stream->shutdown(ShutdownHow::ReadWrite);
                    if (e.no == ECANCELED || e.no == EAGAIN || e.no == EINTR || e.no == ENXIO) {
                        return -1;
                    } else {
                        LOG_ERROR_RETURN(0, -1, "Read request failed `, `", VALUE(ret), e);
                    }
                }

                context.got_it = false;
                m_thread_pool->thread_create(&async_serve, &context);
                stream_serv_count ++;
                while(!context.got_it)
                    thread_yield();
            }
            return 0;
        }
        static void* async_serve(void* args_)
        {
            bool &got_it = ((Context*)args_)->got_it;
            Context context(std::move(*(Context*)args_));
            got_it = true;
            thread_yield();
            context.serve_request();
            // serve done, here reduce refcount
            (*context.stream_serv_count) --;
            context.stream_cv->notify_all();
            return nullptr;
        }
        virtual int shutdown(bool no_more_requests) override {
            m_running = !no_more_requests;
            while (m_list) {
                auto th = m_list.front()->thread;
                thread_enable_join(th);
                if (no_more_requests) {
                    thread_interrupt(th);
                }
                // Wait all streams destructed. Their attached RPC requests are finished as well.
                thread_join((join_handle*) th);
            }
            return 0;
        }
        int shutdown_no_wait() override {
            m_running = false;
            for (auto* each: m_list) {
                thread_interrupt(each->thread);
            }
            return 0;
        }
        virtual ~SkeletonImpl() {
            shutdown(true);
            photon::delete_thread_pool(m_thread_pool);
        }
        explicit SkeletonImpl(uint32_t pool_size = 128) :
              m_thread_pool(photon::new_thread_pool(pool_size)) {
            m_thread_pool->enable_autoscale();
        }
    };
    Skeleton* new_skeleton(uint32_t pool_size)
    {
        return new SkeletonImpl(pool_size);
    }

    class StubPoolImpl : public StubPool {
    public:
        explicit StubPoolImpl(uint64_t expiration, uint64_t timeout,
                              std::shared_ptr<net::ISocketClient> socket_client = nullptr)
            : m_socket_client(std::move(socket_client)) {
            tls_ctx = net::new_tls_context(nullptr, nullptr, nullptr);
            if (!m_socket_client) {
                m_socket_client.reset(net::new_tcp_socket_client());
            }
            m_socket_client->timeout(timeout);
            m_pool = new ObjectCache<net::EndPoint, rpc::Stub*>(expiration);
        }

        ~StubPoolImpl() {
            delete m_pool;
            delete tls_ctx;
        }

        Stub* get_stub(const net::EndPoint& endpoint, bool tls) override {
            auto stub_ctor = [&]() -> rpc::Stub* {
                auto socket = get_socket(endpoint, tls);
                if (socket == nullptr) {
                    return nullptr;
                }
                return rpc::new_rpc_stub(socket, true);
            };
            return m_pool->acquire(endpoint, stub_ctor);
        }

        int put_stub(const net::EndPoint& endpoint, bool immediately) override {
            m_pool->release(endpoint, immediately);
            return 0;
        }

        Stub* acquire(const net::EndPoint& endpoint) override {
            auto ctor = [&]() { return nullptr; };
            return m_pool->acquire(endpoint, ctor);
        }

        uint64_t get_timeout() const override {
            return m_socket_client->timeout();
        }

    protected:
        net::ISocketStream* get_socket(const net::EndPoint& ep, bool tls) const {
            auto sock = m_socket_client->connect(ep);
            if (!sock)
                LOG_ERRNO_RETURN(0, nullptr, "failed to connect to ", ep);
            LOG_DEBUG("connected to ", ep);
            sock->timeout(-1UL);
            if (tls) {
                sock = net::new_tls_stream(tls_ctx, sock, net::SecurityRole::Client, true);
            }
            return sock;
        }

        ObjectCache<net::EndPoint, rpc::Stub*>* m_pool;
        std::shared_ptr<net::ISocketClient> m_socket_client;
        net::TLSContext* tls_ctx = nullptr;
    };

    // dummy pool, for unix domain socket connection to only one point only
    // so no-mather what connection in, gets domain socket
    class UDSStubPoolImpl : public StubPoolImpl {
    public:
        explicit UDSStubPoolImpl(const char* path, uint64_t expiration,
                                 uint64_t timeout)
            : StubPoolImpl(expiration, timeout),
              m_path(path), m_client(net::new_uds_client()) {
                  m_client->timeout(timeout);
              }

        ~UDSStubPoolImpl() {
            delete m_client;
        }

        Stub* get_stub(const net::EndPoint& endpoint, bool) override {
            return m_pool->acquire(endpoint, [&]() -> Stub* {
                auto sock = m_client->connect(m_path.c_str());
                if (!sock) {
                    LOG_ERRNO_RETURN(0, nullptr,
                                     "Connect to unix domain socket failed");
                }
                // stub socket always set timeout for single action
                sock->timeout(-1UL);
                return new_rpc_stub(sock, true);
            });
        }

    protected:
        std::string m_path;
        net::ISocketClient * m_client;
    };

    StubPool* new_stub_pool(uint64_t expiration, uint64_t timeout,
                            std::shared_ptr<net::ISocketClient> socket_client) {
        return new StubPoolImpl(expiration, timeout, std::move(socket_client));
    }

    StubPool* new_uds_stub_pool(const char* path, uint64_t expiration,
                                uint64_t timeout) {
        return new UDSStubPoolImpl(path, expiration, timeout);
    }
    }  // namespace rpc
}
