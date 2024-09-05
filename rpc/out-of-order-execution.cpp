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

#include "out-of-order-execution.h"
#include <unordered_map>
#include <photon/thread/thread.h>
#include <photon/common/utility.h>
#include <photon/common/alog.h>
using namespace std;

namespace photon {
namespace rpc {

    class OooEngine
    {
    public:
        unordered_map<uint64_t, OutOfOrderContext*> m_map;
        condition_variable m_cond_collected, m_wait;
        mutex m_mutex_w, m_mutex_r, m_mutex_map;
        uint64_t m_issuing = 0;
        uint64_t m_tag = 0;
        bool m_running = true;

        // rlock used as both reader lock and wait notifier.
        // add yield in lock will break the assuption that threads
        // not holding lock should kept in sleep.
        // so do not yield, just put into sleep when needed
        // make sure it able to wake by interrupts
        OooEngine(): m_mutex_r(0) {}

        ~OooEngine() {
            shutdown();
        }
        int shutdown() {
            if (!m_running) return -1;
            m_running = false;
            // let all issuing processes done
            while (m_issuing)
                photon::thread_yield_to(nullptr);
            // let all issued tasks complete
            while (get_queue_count() > 0)
                m_cond_collected.wait_no_lock();
            return 0;
        }
        int get_queue_count() {
            return m_map.size();
        }
        int issue_operation(OutOfOrderContext& args) //firing issue
        {
            SCOPED_LOCK(m_mutex_w);
            m_issuing ++;
            DEFER(m_issuing --);
            if (!m_running)
                LOG_ERROR_RETURN(ESHUTDOWN, -1, "engine is been shuting down");
            if (!args.flag_tag_valid)
            {
            again:
                args.tag = ++m_tag; // auto increase if it is not user defined tag
            }
            args.th = CURRENT;
            {
                SCOPED_LOCK(args.phaselock);
                args.phase = OooPhase::BEFORE_ISSUE;
            }
            args.ret = 0;
            {
                SCOPED_LOCK(m_mutex_map);
                auto ret = m_map.insert({args.tag, &args}); //the return value is {iter, bool}
                if (!ret.second) // means insert failed because of key already exists
                {
                    auto tag = args.tag;
                    auto th = (ret.first == m_map.end()) ? nullptr : ret.first->second->th;
                    LOG_ERROR("failed to insert record into unordered hash map",
                            VALUE(tag), VALUE(CURRENT), VALUE(th));
                    if (args.flag_tag_valid) // user set tag, need to tell user it is a failure
                        LOG_ERROR_RETURN(EINVAL, -1, "the tag in argument is NOT valid");
                    goto again;
                }
            }

            int ret2 = args.do_issue(&args);
            if (ret2 < 0) {
                SCOPED_LOCK(m_mutex_map);
                m_map.erase(args.tag);
                m_cond_collected.notify_one();
                LOG_ERROR_RETURN(0, -1, "failed to do_issue()");
            }
            {
                SCOPED_LOCK(args.phaselock);
                args.phase = OooPhase::ISSUED;
            }
            return 0;
        }

        static void wait_check(void* args) {
            OutOfOrderContext& ctx = *(OutOfOrderContext*)args;
            ctx.phase = OooPhase::WAITING;
            ctx.phaselock.unlock();
        };
        int wait_completion(OutOfOrderContext& args) //recieving work
        {
            {
                // check if context issued
                SCOPED_LOCK(m_mutex_map);
                if (m_map.find(args.tag) == m_map.end()) {
                    LOG_ERROR_RETURN(EINVAL, -1,
                                        "context not found in map");
                }
            }
            DEFER(m_wait.notify_one());
            {
                SCOPED_LOCK(args.phaselock);
                if (args.phase == OooPhase::BEFORE_ISSUE)
                    LOG_ERROR_RETURN(EINVAL, -1, "context not issued");
                if (args.phase == OooPhase::WAITING)
                    LOG_ERROR_RETURN(EINVAL, -1, "context already in waiting");
                for (bool hold_lock = false; !hold_lock;) {
                    switch (args.phase) {                        
                        case OooPhase::COLLECTED:
                            // result alread collected before wait
                            if (args.th != CURRENT)
                                LOG_ERROR_RETURN(EINVAL, -1, "context is not issued by current thread");
                            return args.ret;
                        case OooPhase::ISSUED:
                            args.th = photon::CURRENT;
                            args.phase = OooPhase::WAITING;
                        case OooPhase::WAITING:
                            {
                                if (m_mutex_r.try_lock() == 0) {
                                    hold_lock = true;
                                    break;
                                }
                                auto ret = m_wait.wait(args.phaselock, args.timeout);
                                // Check if collected
                                if (args.phase == OooPhase::COLLECTED &&
                                    args.th == CURRENT) {
                                    return args.ret;
                                }
                                if (ret == -1) {
                                    // or just timed out
                                    {
                                        SCOPED_LOCK(m_mutex_map);
                                        m_map.erase(args.tag);
                                        m_cond_collected.notify_one();
                                    }
                                    LOG_ERROR_RETURN(ETIMEDOUT, -1, "waiting for completion timeout");
                                }
                                break;
                            }
                        default:
                            LOG_ERROR_RETURN(EINVAL, -1, "unexpected phase");
                    }
                }
            }

            // Holding mutex_r
            // My origin tag is o_tag
            auto o_tag = args.tag;
            DEFER(m_mutex_r.unlock());
            for (;;) {
                int ret = args.do_completion(&args);
                //this do_completion may recieve results for other threads.
                // but still works because even if tag of this issue have a unique do_completion
                // which make other threads never could recieve it's result
                // the thread will waiting till it hold the lock and get it by itself
                // Since thread may not know the result of an issue will recieve by which thread
                // User must make sure that the do_completion can atleast recieve the result of it's own issue.
                OutOfOrderContext* targ = nullptr;
                unordered_map<uint64_t, OutOfOrderContext*>::iterator it;
                {
                    SCOPED_LOCK(m_mutex_map);
                    DEFER(m_cond_collected.notify_one());
                    if (ret < 0) {
                        // set with nullptr means the thread is once issued but failed when wait_completion
                        m_map.erase(o_tag);
                        LOG_ERROR_RETURN(0, -1, "failed to do_completion()");
                    }

                    it = m_map.find(args.tag);

                    if (it == m_map.end()) {
                        // response tag never issued
                        m_map.erase(o_tag);
                        LOG_ERROR_RETURN(ENOENT, -2, "response's tag ` not found, response should be dropped", args.tag);
                    }
                    targ = it->second;
                    m_map.erase(it);
                }

                // collect with mutex_r
                targ->ret = targ->do_collect(targ);

                {
                    photon::thread *th;
                    {
                        SCOPED_LOCK(targ->phaselock);
                        th = targ->th;
                        targ->phase = OooPhase::COLLECTED;
                    }
                    if (o_tag == args.tag) {
                        if (th != CURRENT) {
                            LOG_ERROR_RETURN(EINVAL, -1, "args tag ` not belong to current thread `", VALUE(args.tag), VALUE(CURRENT));
                        }
                        return args.ret;  // it's my result, let's break, and
                                          // collect it
                    }
                    if (!th)
                        // issued but requesting thread just failed in completion when waiting
                        LOG_ERROR_RETURN(ENOENT, -2, "response recvd, but requesting thread is NULL!");
                    thread_interrupt(th, EINTR);    // other threads' response, resume him
                }
            }
        }
        int issue_wait(OutOfOrderContext& args)
        {
            int ret = issue_operation(args);
            if (ret < 0) return ret;
            ret = wait_completion(args);
            return ret;
        }
        void result_collected()
        {
        }
    };

    OutOfOrder_Execution_Engine* new_ooo_execution_engine()
    {
        return (OutOfOrder_Execution_Engine*)(new OooEngine);
    }
    void delete_ooo_execution_engine(OutOfOrder_Execution_Engine* engine)
    {
        delete (OooEngine*) engine;
    }
    int ooo_issue_operation(OutOfOrderContext& args)
    {
        auto e = (OooEngine*)args.engine;
        return e->issue_operation(args);
    }
    int ooo_wait_completion(OutOfOrderContext& args)
    {
        auto e = (OooEngine*)args.engine;
        return e->wait_completion(args);
    }
    int ooo_issue_wait(OutOfOrderContext& args)
    {
        auto e = (OooEngine*)args.engine;
        return e->issue_wait(args);
    }
    void ooo_result_collected(OutOfOrderContext& args)
    {
        auto e = (OooEngine*)args.engine;
        e->result_collected();
    }
    int ooo_get_queue_count(OutOfOrder_Execution_Engine* engine)
    {
        auto e = (OooEngine*) engine;
        return e->get_queue_count();
    }
}
}
