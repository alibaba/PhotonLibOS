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

#include "reset_handle.h"
#include <photon/common/alog.h>
#include <photon/thread/std-compat.h>
#include <pthread.h>

namespace photon {

static thread_local ResetHandle *list = nullptr;

ResetHandle::ResetHandle() {
    LOG_DEBUG("push ", VALUE(this));
    if (list == nullptr) {
        list = this;
    } else {
        list->insert_tail(this);
    }
}

ResetHandle::~ResetHandle() {
    LOG_DEBUG("erase ", VALUE(this));
    auto nx = this->remove_from_list();
    if (this == list)
        list = nx;
}

void reset_all_handle() {
    if (list == nullptr)
        return;
    LOG_INFO("reset all handle called");
    for (auto handler : *list) {
        LOG_DEBUG("reset ", VALUE(handler));
        handler->reset();
    }
}

} // namespace photon
