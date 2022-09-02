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

#include "std-compat.h"

#include <photon/common/alog.h>

static inline void throw_system_error(int err_num, const char* msg) {
    LOG_ERROR(msg, ": ", ERRNO(err_num));
    throw std::system_error(::std::error_code(err_num, ::std::generic_category()), msg);
}

namespace photon {
namespace std {

void __throw_system_error(int err_num, const char* msg) {
    throw_system_error(err_num, msg);
}

}
}