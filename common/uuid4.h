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

/**
 * Copyright (c) 2018 rxi
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the MIT license. See LICENSE for details.
 */

#ifndef _UUID4_H
#define _UUID4_H

#define UUID4_VERSION "1.0.0"


enum {
    UUID4_ESUCCESS =  0,
    UUID4_EFAILURE = -1
};

typedef	char uuid4_t[16];
typedef char uuid4_string_t[37];


int uuid4_parse(char *in, uuid4_t uu);
void uuid4_unparse_upper(uuid4_t uu, uuid4_string_t out);
void uuid4_clear(uuid4_t uu);
int uuid4_is_null(uuid4_t uu);
void uuid4_generate(uuid4_t uu);
void uuid4_copy(uuid4_t dst, uuid4_t src);
int uuid4_compare(uuid4_t uu1, uuid4_t uu2);
int invalid_uuid4(char *str_uu);

#endif
