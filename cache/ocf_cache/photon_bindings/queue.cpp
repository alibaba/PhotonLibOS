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

#include "queue.h"

#include <photon/thread/thread-pool.h>
#include <photon/thread/workerpool.h>
#include <photon/photon.h>

static void *run(void *args) {
    auto queue = (ocf_queue_t)args;
    ocf_queue_run(queue);
    return nullptr;
}

/* Pooled queue kicker */
class QueueKicker {
public:
    explicit QueueKicker(ocf_queue_t queue) : m_queue(queue) {
    }
    QueueKicker(ocf_queue_t queue, size_t vcpu_num, int ev_engine, int io_engine, int mode) : m_queue(queue) {
        work_pool = new photon::WorkPool(vcpu_num, ev_engine, io_engine, mode);
    }
    ~QueueKicker() {
        delete work_pool;
    }

    inline void kick() {
        if (work_pool) {
            work_pool->async_call(new auto([this](){ run(m_queue); }));
        } else {
            photon::thread_create(run, m_queue);
        }
    }

private:
    /* associated OCF queue */
    ocf_queue_t m_queue;
    /* thread pool */
    photon::WorkPool* work_pool = nullptr;
};

int init_queues(ocf_queue_t mngt_queue, ocf_queue_t io_queue) {
    auto mngt_queue_kicker = new QueueKicker(mngt_queue, 2, 0, 0, 64);
    auto io_queue_kicker = new QueueKicker(io_queue, 4, photon::INIT_EVENT_EPOLL, photon::INIT_IO_LIBCURL, 64);

    ocf_queue_set_priv(mngt_queue, mngt_queue_kicker);
    ocf_queue_set_priv(io_queue, io_queue_kicker);
    return 0;
}

/* Callback for OCF to kick the queue thread */
static void queue_thread_kick(ocf_queue_t q) {
    auto qk = (QueueKicker *)ocf_queue_get_priv(q);
    qk->kick();
}

/* Callback for OCF to stop the queue thread */
static void queue_thread_stop(ocf_queue_t q) {
    auto qk = (QueueKicker *)ocf_queue_get_priv(q);
    delete qk;
}

/* Queue ops */
static const ocf_queue_ops queue_ops = {
    .kick = queue_thread_kick,
    .kick_sync = nullptr,
    .stop = queue_thread_stop,
};

const ocf_queue_ops *get_queue_ops() {
    return &queue_ops;
}
