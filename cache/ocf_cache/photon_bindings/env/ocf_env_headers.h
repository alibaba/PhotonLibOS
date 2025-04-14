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

#ifndef __OCF_ENV_HEADERS_H__
#define __OCF_ENV_HEADERS_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* TODO: Move prefix printing to context logger. */
#define OCF_LOGO "OCF"
#define OCF_PREFIX_SHORT "[" OCF_LOGO "] "
#define OCF_PREFIX_LONG "Open CAS Framework"

#define OCF_VERSION_MAIN 20
#define OCF_VERSION_MAJOR 3
#define OCF_VERSION_MINOR 0

#endif /* __OCF_ENV_HEADERS_H__ */
