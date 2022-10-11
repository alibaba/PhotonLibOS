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

// a header file to include std::string_view (of C++17) in c++11 mode,
// in a uniform way, for both gcc and clang

// in C++14, string_view is still in experimental stage
// compilers actually using higher c++14 standard
// can directly using 

// gcc    201402 is c++14 in gcc 5.1.0
//        201300 is c++1y in gcc 4.9.2 since 4.9.2 do not support c++14
// clang  201402 is c++14 in clang 3.5+

// string_view in c++14 is experimental/string_view 
// and become standard in c++17
//
// In gcc, std=c++14/c++1y have no string view support
// such a marco covered whole string_view header
// #if __cplusplus >= 201703L
//
// for C++14 standard, experimental/string_view hase another namespace
// std::experimental::string_view
// which requires __cplusplus >= 201402

#ifdef __APPLE__
    #include <string_view>
#elif __cplusplus >= 201700L
    // C++ 17 supported
    // directly include <string_view>
    #include <string_view>
#elif __cplusplus >= 201400L
    // C++ 14, string_view in experimental
    #include <experimental/string_view>
    // wrapeed into std namespace
    namespace std {
        using string_view = std::experimental::string_view;
    }
#else
    // legacy c++ standard, still may have newer libc++
    // temporarly markup as c++14, use experimental lib
    #pragma push_macro("__cplusplus")
    #undef __cplusplus
    #define __cplusplus 201402L
    #include <experimental/string_view>
    #pragma pop_macro("__cplusplus")
    namespace std
    {
        using string_view = std::experimental::string_view;
    }
#endif
