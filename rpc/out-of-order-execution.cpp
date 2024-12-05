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
        condition_variable m_cond_collected;
        mutex m_mutex_w, m_mutex_r;
        uint64_t m_issuing = 0;
        uint64_t m_tag = 0;
        bool m_running = true;

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
            m_issuing ++;
            DEFER(m_issuing --);
            scoped_lock lock(m_mutex_w);
            if (!m_running)
                LOG_ERROR_RETURN(ESHUTDOWN, -1, "engine is been shuting down");
            if (!args.flag_tag_valid)
            {
            again:
                args.tag = ++m_tag; // auto increase if it is not user defined tag
            }
            args.th = CURRENT;
            args.collected = false;
            args.ret = 0;
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

            int ret2 = args.do_issue(&args);
            if (ret2 < 0) {
                m_map.erase(args.tag);
                LOG_ERROR_RETURN(0, -1, "failed to do_issue()");
            }
            return 0;
        }
        int wait_completion(OutOfOrderContext& args) //recieving work
        {
            // lock with param 1 means allow entry without lock
            // when interuptted
            scoped_lock lock(m_mutex_r, 1);

            // when wait_completion returned,
            // always have tag removed from the map
            // notify the waiting function (like shutdown())
            DEFER(m_cond_collected.notify_one());

            auto o_tag = args.tag;
            {
                auto o_it = m_map.find(o_tag);
                if (o_it == m_map.end()) {
                    LOG_ERROR_RETURN(EINVAL, -1, "issue of ` not found", VALUE(args.tag));
                }
                if (o_it->second->th != CURRENT)
                {
                    LOG_ERROR_RETURN(EINVAL, -1, "args tag ` not belong to current thread `", VALUE(args.tag), VALUE(CURRENT));
                }
                if (o_it->second->collected) {
                    // my completion has been done
                    // just collect it, clear the trace,
                    // then return result
                    auto ret = o_it->second->ret;
                    m_map.erase(o_it);
                    return ret;
                }
            }
            //Hold the lock, but not get the result.
            while (true)
            {
                int ret = args.do_completion(&args); //this do_completion may recieve results for other threads.
                // but still works because even if tag of this issue have a unique do_completion
                // which make other threads never could recieve it's result
                // the thread will waiting till it hold the lock and get it by itself
                // Since thread may not know the result of an issue will recieve by which thread
                // User must make sure that the do_completion can atleast recieve the result of it's own issue.
                if (ret < 0) {
                    // set with nullptr means the thread is once issued but failed when wait_completion
                    m_map.erase(o_tag);
                    LOG_ERROR_RETURN(0, -1, "failed to do_completion()");
                }

                if (o_tag == args.tag) {
                    m_map.erase(o_tag);
                    break;   // it's my result, let's break, and collect it
                }

                auto it = m_map.find(args.tag);

                if (it == m_map.end()) {
                    // response tag never issued
                    m_map.erase(o_tag);
                    LOG_ERROR_RETURN(ENOENT, -2, "response's tag ` not found, response should be dropped", args.tag);
                }

                auto targ = it->second;
                auto th = targ->th;

                if (!th)
                    // issued but requesting thread just failed in completion when waiting
                    LOG_ERROR_RETURN(ENOENT, -2, "response recvd, but requesting thread is NULL!");

                it->second->ret = targ->do_collect(targ);
                it->second->collected = true;
                thread_interrupt(th);    // other threads' response, resume him
            }
            // only break can bring out the while-loop
            // means my result has been completed,
            // ready to collect
            DEFER(thread_yield_to(nullptr));
            return args.do_collect(&args);
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
