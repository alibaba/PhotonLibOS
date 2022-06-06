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
#include <typeinfo>
#include <cxxabi.h>
#include <photon/common/alog.h>

template<typename T>
inline LogBuffer& __printfp__(LogBuffer& log, T func)
{
    size_t size = 0;
    int status = -4; // some arbitrary value to eliminate the compiler warning
    auto name = abi::__cxa_demangle(typeid(func).name(), nullptr, &size, &status);
    log << "function_pointer<" << ALogString(name, size) << "> at " << (void*&)func;
    free(name);
    return log;
}

template<typename Ret, typename...Args>
inline LogBuffer& operator << (LogBuffer& log, Ret (*func)(Args...))
{
    return __printfp__(log, func);
}

template<typename Ret, typename Clazz, typename...Args>
inline LogBuffer& operator << (LogBuffer& log, Ret (Clazz::*func)(Args...))
{
    return __printfp__(log, func);
}

template<typename Ret, typename Clazz, typename...Args>
inline LogBuffer& operator << (LogBuffer& log, Ret (Clazz::*func)(Args...) const)
{
    return __printfp__(log, func);
}

