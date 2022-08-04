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
#include <cinttypes>

template<typename PF, typename T>
struct pmf_map
{
    PF f;
    T* obj; // may be adjusted for virtual function call
};

template<typename PF, typename T, typename MF>
inline auto __get_mfa__(T* obj, MF f)
    -> pmf_map<PF, T>
{
    struct PMF  // Pointer to Member Function
    {
        union { uint64_t func_ptr, offset; };
        uint64_t this_adjustment;
#ifdef __x86_64__
        bool is_virtual() const
        {
            return offset & 1;
        }
        uint64_t get_virtual_function_address(void*& obj) const
        {
            (char*&)obj += this_adjustment;
            auto vtbl_addr = *(uint64_t*)obj;
            return *(uint64_t*)(vtbl_addr + offset - 1);
        }
#elif defined(__aarch64__)
        bool is_virtual() const
        {
            return this_adjustment & 1;
        }
        uint64_t get_virtual_function_address(void*& obj) const
        {
            (char*&)obj += (this_adjustment / 2);
            auto vtbl_addr = *(uint64_t*)obj;
            return *(uint64_t*)(vtbl_addr + offset);
        }
#endif
        uint64_t get_function_address(void*& obj) const
        {
            return is_virtual() ? get_virtual_function_address(obj) : func_ptr;
        }
    };

    auto pmf = (PMF&)f;
    auto addr = pmf.get_function_address((void*&)obj);
    return pmf_map<PF, T>{(PF)addr, obj};
}

template<typename T, typename R, typename...ARGS>
inline auto get_member_function_address(T* obj, R (T::*f)(ARGS...))
    -> pmf_map<R(*)(T*, ARGS...), T>
{
    typedef R(*PF)(T*, ARGS...);
    return __get_mfa__<PF>(obj, f);
}

template<typename T, typename R, typename...ARGS>
inline auto get_member_function_address(const T* obj, R (T::*f)(ARGS...) const)
    -> pmf_map<R(*)(const T*, ARGS...), const T>
{
    typedef R(*PF)(const T*, ARGS...);
    return __get_mfa__<PF>(obj, f);
}

