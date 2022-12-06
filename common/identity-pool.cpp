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

#include "identity-pool.h"
#include "../thread/timer.h"
// #include "alog.h"

void* IdentityPoolBase::get()
{
    SCOPED_LOCK(m_mtx);
    void* obj = nullptr;
    assert(m_size <= m_capacity);
    if (m_size > 0) {
        obj = m_items[--m_size];
        if (m_size < min_size_in_interval)
            min_size_in_interval = m_size;
    } else {
        m_mtx.unlock();
        m_ctor(&obj);
        m_mtx.lock();
    }
    ++m_refcnt;
    return obj;
}

void IdentityPoolBase::put(void* obj)
{
    if (!obj) return;
    {
        SCOPED_LOCK(m_mtx);
        if (m_size < m_capacity) {
            m_items[m_size++] = obj;
        } else{
            m_mtx.unlock();
            m_dtor(obj);
            m_mtx.lock();
        }
        --m_refcnt;
    }
    m_cvar.notify_all();
    assert(m_size <= m_capacity);
}

uint64_t IdentityPoolBase::do_scale()
{
    auto des_n = (min_size_in_interval + 1) / 2;
    assert(des_n <= m_size);
    SCOPED_LOCK(m_mtx);
    while (m_size > 0 && des_n-- > 0) {
        auto x = --m_size;
        m_mtx.unlock();
        m_dtor(m_items[x]);
        m_mtx.lock();
    }
    min_size_in_interval = m_size;
    return 0;
}

IdentityPoolBase::~IdentityPoolBase()
{
    if (autoscale)
        disable_autoscale();

    bool logged = false;
    {
        SCOPED_LOCK(m_mtx);
        while (m_refcnt > 0) {
            m_cvar.wait(m_mtx, 10 * 1000 * 1000);
            if (m_refcnt > 0) {
                LOG_WARN("IdentityPool is still in use! Waiting for destruction.");
                logged = true;
            }
        }
    }
    assert(m_size <= m_capacity);
    while (m_size > 0)
        m_dtor(m_items[--m_size]);
    if (logged)
        LOG_WARN("IdentityPool destructed");
}

struct ScalePoolController;
static thread_local ScalePoolController* g_scale_pool_controller;
struct ScalePoolController {
    photon::Timer timer;
    intrusive_list<IdentityPoolBase> entries;
    ScalePoolController(uint64_t interval = 1000UL * 1000)
        : timer(interval, {this, &ScalePoolController::scan_pool_scale}) {}

    ~ScalePoolController() { }
    photon::mutex mutex;

    uint64_t scan_pool_scale() {
        photon::scoped_lock lock(mutex);
        for (auto e : entries) {
            e->do_scale();
        }
        return 0;
    }

    int register_pool(IdentityPoolBase* entry) {
        photon::scoped_lock lock(mutex);
        entries.push_back(entry);
        return 0;
    }

    int unregister_pool(IdentityPoolBase* entry) {
        photon::scoped_lock lock(mutex);
        entries.pop(entry);
        return 0;
    }
};

int IdentityPoolBase::enable_autoscale() {
    if (autoscale) return -1;
    if (g_scale_pool_controller == nullptr) {
        g_scale_pool_controller = new ScalePoolController();
    }
    g_scale_pool_controller->register_pool(this);
    autoscale = true;
    return 0;
}

int IdentityPoolBase::disable_autoscale() {
    if (!autoscale) return -1;
    g_scale_pool_controller->unregister_pool(this);
    autoscale = false;
    if (g_scale_pool_controller->entries.empty()) {
        auto ctl = g_scale_pool_controller;
        g_scale_pool_controller = nullptr;
        delete ctl;
    }
    return 0;
}
