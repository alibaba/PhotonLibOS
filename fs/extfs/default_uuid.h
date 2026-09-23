/*
Copyright 2023 The Photon Authors

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
#pragma once

// The uuid that make_extfs stamps into the superblock, and the one that
// fsck_extfs restores when it finds an all-zero uuid. Keep it in a single
// place: two copies of the constant is exactly how the old images ended up
// with an unparsable uuid unnoticed.
//
// 299792458 is the speed of light in m/s, a nod to photon; ef5 is the leading
// part of EXT2_SUPER_MAGIC (0xef53)
constexpr char DEFAULT_UUID[] = "bdf7bb2e-c231-43ce-87c2-299792458ef5";
