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

#include "thread-pool.h"
#include "thread-key.h"
#include "../common/alog.h"

namespace photon
{
    TPControl* ThreadPoolBase::thread_create_ex(thread_entry start, void* arg, bool joinable)
    {
        auto pCtrl = B::get();
        {
            SCOPED_LOCK(pCtrl->m_mtx);
            pCtrl->joinable = joinable;
            pCtrl->joining = false;
            pCtrl->start = start;
            pCtrl->arg = arg;
        }
        pCtrl->cvar.notify_one();
        return pCtrl;
    }
    void* ThreadPoolBase::stub(void* arg)
    {
        TPControl ctrl;
        auto th = *(thread**)arg;
        *(TPControl**)arg = &ctrl;              // tell ctor where my `ctrl` is
        thread_yield_to(th);
        while(true)
        {
            {
                SCOPED_LOCK(ctrl.m_mtx);
                while (!ctrl.start)                     // wait for `create()` to give me
                    ctrl.cvar.wait(ctrl.m_mtx);           // thread_entry and argument

                if (ctrl.start == &stub)
                    break;
                ((partial_thread*) CURRENT)->tls = nullptr;
            }
            ctrl.start(ctrl.arg);
            deallocate_tls();
            {
                SCOPED_LOCK(ctrl.m_mtx);
                if (ctrl.joining) {
                    assert(ctrl.joinable);
                    ctrl.cvar.notify_all();
                } else if (ctrl.joinable) {
                    ctrl.joining = true;
                    ctrl.cvar.wait(ctrl.m_mtx);
                }
                ctrl.joinable = false;
                ctrl.joining = false;
                ctrl.start = nullptr;
            }
            ctrl.pool->put(&ctrl);
        }
        return nullptr;
    }
    void ThreadPoolBase::join(TPControl* pCtrl)
    {
        SCOPED_LOCK(pCtrl->m_mtx);
        if (!pCtrl->joinable)
            LOG_ERROR_RETURN(EINVAL, , "thread is not joinable");
        if (!pCtrl->start)
            LOG_ERROR_RETURN(EINVAL, , "thread is not running");
        if (pCtrl->start == &stub)
            LOG_ERROR_RETURN(EINVAL, , "thread is dying");

        if (pCtrl->joining) {
            pCtrl->cvar.notify_one();
        } else {
            pCtrl->joining = true;
            pCtrl->cvar.wait(pCtrl->m_mtx);
        }
    }
    int ThreadPoolBase::ctor(ThreadPoolBase* pool, TPControl** out)
    {
        auto pCtrl = (TPControl*)CURRENT;
        auto stack_size = (uint64_t)pool->m_reserved;
        auto th = photon::thread_create(&stub, &pCtrl, stack_size);
        thread_yield_to(th);
        assert(pCtrl);
        *out = pCtrl;
        pCtrl->th = th;
        pCtrl->pool = pool;
        pCtrl->start = nullptr;
        return 0;
    }
    int ThreadPoolBase::dtor(ThreadPoolBase* tpb, TPControl* pCtrl)
    {
        {
            SCOPED_LOCK(pCtrl->m_mtx);
            if (pCtrl->start) {     // it's running
                assert(pCtrl->start != &stub);
                pCtrl->joinable = true;
                pCtrl->m_mtx.unlock();
                tpb->join(pCtrl);
                pCtrl->m_mtx.lock();
            }
            pCtrl->start = &stub;
            pCtrl->cvar.notify_all();
        }
        thread_yield();
        return 0;
    }
}
