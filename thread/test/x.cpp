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

int DevNull(void* x, int)
{
    return 0;
}

int (*pDevNull)(void*, int) = &DevNull;

#ifdef ThreadUT

#include "../thread.h"
namespace photon {
int init(uint64_t event_engine, uint64_t io_engine) {
    return vcpu_init();
}

int fini() {
    return vcpu_fini();
}
}

#endif
