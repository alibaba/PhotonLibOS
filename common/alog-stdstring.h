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
#include <string>
#include <photon/common/alog.h>
#include <photon/common/string_view.h>

inline LogBuffer& operator << (LogBuffer& log, const std::string& s)
{
    return log << ALogString(s.c_str(), s.length());
}

inline LogBuffer& operator << (LogBuffer& log, const std::string_view& sv)
{
    return log << ALogString(sv.data(), sv.length());
}

class string_key;
inline LogBuffer& operator << (LogBuffer& log, const string_key& sv)
{
    return log << (const std::string_view&)sv;
}

class estring;
inline LogBuffer& operator << (LogBuffer& log, const estring& es)
{
    return log << (const std::string&)es;
}

class estring_view;
inline LogBuffer& operator << (LogBuffer& log, const estring_view& esv)
{
    return log << (const std::string_view&)esv;
}

