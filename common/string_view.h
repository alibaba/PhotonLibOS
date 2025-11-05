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

#pragma once

// Unified inclusion of std::string_view across GCC (libstdc++) and
// Clang 11/16 (libc++), without any polyfill.
//
// Strategy:
// - libc++: use <string_view> in all modes (it is available/back-deployed).
// - libstdc++: use <string_view> in C++17+, else try <experimental/string_view>.
// - If nothing is available, produce a compile-time error.
//
// Correct __cplusplus values:
//   C++11 = 201103L, C++14 = 201402L, C++17 = 201703L.

#if defined(_LIBCPP_VERSION)
// libc++ (Clang 11/16, Apple Clang): back-deployed <string_view>
  #include <string_view>

#elif __cplusplus >= 201703L
// C++17 or newer (libstdc++ provides <string_view>)
  #include <string_view>

#elif defined(__has_include)
// Pre-C++17: prefer experimental for libstdc++
  #if __has_include(<experimental/string_view>)
    #include <experimental/string_view>
    namespace std {
      using string_view     = std::experimental::string_view;
      using wstring_view    = std::experimental::wstring_view;
      using u16string_view  = std::experimental::u16string_view;
      using u32string_view  = std::experimental::u32string_view;
    }
  #elif __has_include(<string_view>)
    // As a last try, if <string_view> exists pre-C++17 (some backports)
    #include <string_view>
  #else
    #error "No string_view support found. Install libstdc++ in gcc or libc++ in clang."
  #endif

#else
  // Very old compilers without __has_include can't be probed safely.
  #error "Compiler lacks __has_include; cannot detect string_view. Update compiler."
#endif

