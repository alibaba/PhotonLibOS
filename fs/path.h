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
#include <unordered_map>
#include <stack>
#include <memory>
#include <climits>
#include <photon/fs/filesystem.h>
#include <photon/common/string_view.h>

namespace photon {
namespace fs
{
    using string_view = std::string_view;
    // A class to parse a path string (string_view), providing
    // iterator to go through the path's components.
    // Also provides functions to get some specific components.
    class Path
    {
    public:
        template<typename T, typename A>
        Path(const std::basic_string<char,T,A>& path) : m_path(path) { }

        Path(string_view path) : m_path(path) { }

        Path(const char* path) : m_path(path) { }

        struct iterator
        {
            iterator(string_view path)
            {
                end = path.data() + path.size();
                set(path.data());
            }
            iterator() // end()
            {
                end = 0;
                init_view(nullptr, 0);
            }
            iterator& operator++()
            {
                set(m_view.data() + m_view.size());
                return *this;
            }
            const string_view& operator*() const
            {
                return m_view;
            }
            string_view& operator*()
            {
                return m_view;
            }
            string_view following_part() const
            {
                return {m_view.end(), (size_t)(end - m_view.end())};
            }
            const string_view* operator->() const
            {
                return &m_view;
            }
            string_view* operator->()
            {
                return &m_view;
            }
            bool operator==(const iterator& rhs) const
            {
                return m_view == rhs.m_view;
            }
            bool operator!=(const iterator& rhs) const
            {
                return m_view != rhs.m_view;
            }

        protected:
            const char* end;
            string_view m_view;
            void set(const char* p);
            void init_view(const char* ptr, size_t len)
            {
                m_view = {ptr, len};
            }
        };

        iterator begin() const
        {
            return iterator(m_path);
        }
        iterator end() const
        {
            return iterator();
        }
        string_view basename()
        {
            auto end = m_path.find_last_not_of('/');
            if (end == m_path.npos) return string_view();

            auto begin = m_path.find_last_of('/', end);
            if (begin == m_path.npos) begin = 0;
            else begin++;

            return m_path.substr(begin, end - begin + 1);
        }
        std::pair<string_view, string_view> dir_base_name()
        {
            auto bn = basename();
            auto dir = string_view(m_path.data(), bn.data() - m_path.data());
            return {dir, bn};
        }
        string_view dirname()
        {
            return dir_base_name().first;
        }
        Path directory()
        {
            auto pos = m_path.find_last_of('/');
            if (pos == string_view::npos) pos = 0;
            return string_view(m_path.data(), pos);
        }
        bool ends_with_slash()
        {
            return m_path.back() == '/';
        }

        // check valid-ness for '..'
        bool level_valid();

    protected:
        string_view m_path;
    };

    // return 1 for ".", and 2 for "..", and 0 otherwise
    inline int is_dots(const string_view& name)
    {
        if (name[0] == '.')
        {
            if (name.size() == 1) {
                return 1;
            } else if (name.size() == 2 && name[1] == '.') {
                return 2;
            }
        }
        return 0;
    }

    inline bool path_level_valid(const char* path)
    {
        return Path(path).level_valid();
    }

    int mkdir_recursive(const string_view &pathname, IFileSystem* fs, mode_t mode=0755);

    class Walker    // to walk recursively the dir tree
    {
    public:
        Walker(IFileSystem* fs, string_view path);
        string_view path() { return {m_path_buffer, m_path_len}; }
        string_view get() { return path(); }
        bool valid() { return m_path_len; }
        int next();

    protected:
        size_t m_path_len = 0;
        IFileSystem* m_filesystem;
        std::stack<std::unique_ptr<DIR>> m_stack;
        char m_path_buffer[PATH_MAX];
        int is_dir(dirent* entry);
        int enter_dir();
        void path_push_back(string_view s);
        void path_pop_back(size_t len1);
    };

    namespace Tree
    {
        class Node;

        // a class to represent either a user value of void*, or a sub dir (Node*)
        struct Value
        {
            Value() { }
            explicit Value(void* val)           { value = val; }
            explicit Value(Node* node)          { value = (void*)((uint64_t)node | MASK); }
            bool is_node()                      { return (uint64_t)value & MASK; }
            Node* as_node_ptr()                 { return is_node() ? (Node*)as_ptr() : nullptr; }
            void* as_ptr()                      { return (void*)((uint64_t)value & ~MASK); }

            void operator = (Value&& rhs)       { if (this != &rhs) {value = rhs.value; rhs.value = nullptr; } }
            void operator = (Value& rhs)        = delete;
            void operator = (const Value& rhs)  = delete;
            Value(Value&& rhs)                  { value = rhs.value; rhs.value = nullptr; }
            Value(Value& rhs)                   = delete;
            Value(const Value& rhs)             = delete;
            ~Value();

            const static uint64_t MASK = 1UL << 63;
            void* value;
        };

        class Node : public std::unordered_map<std::string, Value>
        {
        public:
            // create a file node on path `path`, and init its value as `v`,
            // optionally creat intermedia dirs
            int creat(string_view path, void* v, bool create_path = false);

            // read a file's content
            int read(string_view path, void** v);

            // update (write) an existing file's content
            int write(string_view path, void* v);

            // remove an existing file
            int unlink(string_view path);

            // create a dir, optionally create intermedia dirs
            int mkdir(string_view path, bool _p = false);

            // remove an existing empty dir
            int rmdir(string_view path);

            // change root dir into a sub dir
            Node* chdir(string_view path);

            // return 1 for file, 2 for dir
            // -1 for non-existance or error
            int stat(string_view path);

            bool is_dir(string_view path)
            {
                return stat(path) == 2;
            }

            bool is_file(string_view path)
            {
                return stat(path) == 1;
            }

        protected:
            Node* seek2node(string_view path, bool create_path = false);
        };

        inline Value::~Value()
        {
            if (is_node())
                delete as_node_ptr();
        }
    } // namespace Tree
} // namespace fs
}
