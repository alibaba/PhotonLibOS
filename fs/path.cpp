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

#include "path.h"
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <photon/common/utility.h>
#include "string.h"
#include "assert.h"
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/enumerable.h>

using namespace std;

#define ERROR_RETURN(no, ret) { errno = no; return ret; }

#define SEEK2NODE(node, fn, create_path)    \
    Node* node;                             \
    std::string fn;                         \
    {                                       \
        Path p(path);                       \
        auto dir_base = p.dir_base_name();  \
        node = seek2node(dir_base.first, create_path);  \
        if (!node) return -1;               \
        fn = string(dir_base.second);       \
    }

#define SEEK2ENT(node, it)                  \
    Node::iterator it;                      \
    SEEK2NODE(node, __fn__, false)          \
    it = node->find(__fn__);                \
    if (it == node->end())                  \
        ERROR_RETURN(ENOENT, -1);

namespace photon {
namespace fs
{
    void Path::iterator::set(const char* p)
    {
        if (!p || !*p)
            return init_view(nullptr, 0);

        // skip any number of slashes
        while(p < end && *p != '\0' && *p == '/') ++p;
        auto ptr = p;

        // find the next slash
        while(p < end && *p != '\0' && *p != '/') ++p;
        init_view(ptr, p - ptr);
    }

    bool Path::level_valid()
    {
        int level = 0;
        for (auto& name: *this)
        {
            auto size = name.size();
            if (size > 0)
                if (name[0] == '.')
                {
                    if (size == 1)
                    {   // the case for '.'
                        continue;
                    }
                    else if (size == 2)
                    {   // the case for '..'
                        if (name[1] == '.')
                            if (--level < 0)
                                return false;
                    }
                    else ++level;
                }
        }
        return true;
    }

    // 1. no relative path. Use Tree::Node to transform relative path to absolute one.
    // 2. If pathname is not ended with '/', mkdir_recursive will regard it as a file and then create its base,
    //    which differs from Linux mkdir -p
    int mkdir_recursive(const string_view &pathname, IFileSystem* fs, mode_t mode) {
        auto len = pathname.length();
        if (len == 0) return 0;

        char path[PATH_MAX];
        if (pathname[0] != '/') {
            *path = '/';
            memcpy(path + 1, pathname.begin(), len);
            len++;
        } else {
            memcpy(path, pathname.begin(), len);
        }
        path[len] = '\0';

        for (size_t i=1; i<len; i++) {
            if (path[i] == '/') {
                path[i] = 0;
                auto ret = fs->mkdir(path, mode);
                if (ret < 0) {
                    auto e = errno;
                    if (e != EEXIST) {
                        return -1;
                    }
                }
                path[i] = '/';
            }
        }
        return 0;
    }

    namespace Tree
    {
        Node* Node::seek2node(string_view path, bool create_path)
        {
            auto node = this;
            for (auto& p: Path(path))
            {
                if (is_dots(p))
                    ERROR_RETURN(EINVAL, nullptr);

                string name(p);
                auto it = node->find(name);
                if (it == node->end())
                {
                    if (!create_path)
                        ERROR_RETURN(ENOENT, nullptr);

                    auto ret = node->emplace(std::move(name), Value(new Node));
                    assert(ret.second);
                    it = ret.first;
                }
                else if (!it->second.is_node())
                {
                    ERROR_RETURN(EEXIST, nullptr);
                }
                node = it->second.as_node_ptr();
            }
            return node;
        }
        int Node::creat(string_view path, void* v, bool create_path)
        {
            if (path.empty() || path.back() == '/')
                ERROR_RETURN(EINVAL, -1);

            SEEK2NODE(node, fn, create_path);
            auto ret = node->emplace(std::move(fn), Value(v));
            if (!ret.second)
                ERROR_RETURN(EEXIST, -1);

            return 0;
        }
        int Node::read(string_view path, void** v)
        {
            if (path.empty() || path.back() == '/')
                ERROR_RETURN(EINVAL, -1);

            SEEK2ENT(node, it);
            if (it->second.is_node())
                ERROR_RETURN(EISDIR, -1);

            *v = it->second.value;
            return 0;
        }
        int Node::write(string_view path, void* v)
        {
            if (path.empty() || path.back() == '/')
                ERROR_RETURN(EINVAL, -1);

            SEEK2ENT(node, it);
            if (it->second.is_node())
                ERROR_RETURN(EISDIR, -1);

            it->second.value = v;
            return 0;
        }
        int Node::unlink(string_view path)
        {
            if (path.empty() || path.back() == '/')
                ERROR_RETURN(EINVAL, -1);

            SEEK2ENT(node, it);
            if (it->second.is_node())
                ERROR_RETURN(EISDIR, -1);

            node->erase(it);
            return 0;
        }
        int Node::mkdir(string_view path, bool _p)
        {
            if (path.empty())
                ERROR_RETURN(EINVAL, -1);

            SEEK2NODE(node, fn, _p);
            if (node->count(fn) > 0)
                ERROR_RETURN(EEXIST, -1);

            node->emplace(std::move(fn), Value(new Node));
            return 0;
        }
        int Node::rmdir(string_view path)
        {
            if (path.empty())
                ERROR_RETURN(EINVAL, -1);

            SEEK2ENT(node, it);
            if (!it->second.is_node())
                ERROR_RETURN(ENOTDIR, -1);
            if (!it->second.as_node_ptr()->empty())
                ERROR_RETURN(ENOTEMPTY, -1);

            node->erase(it);
            return 0;
        }
        int Node::stat(string_view path)
        {
            if (path.empty())
                ERROR_RETURN(EINVAL, -2);

            SEEK2ENT(node, it);
            return it->second.is_node() ? 2 : 1;
        }
        Node* Node::chdir(string_view path)
        {
            if (path.empty())
                ERROR_RETURN(EINVAL, nullptr);

            return seek2node(path);
        }
    }

    Walker::Walker(IFileSystem* fs, string_view path)
    {
        if (!fs) return;
        m_filesystem = fs;
        path_push_back(path);
        enter_dir();
        if (path.empty() || path.back() != '/') {
          path_push_back("/");
        }
    }
    int Walker::enter_dir()
    {
        auto dir = m_filesystem->opendir(m_path_buffer);
        if (!dir)
            LOG_ERRNO_RETURN(0, -1, "failed to opendir(`)", m_path);
        m_stack.emplace(dir);
        return 0;
    }
    int Walker::is_dir(dirent* entry)
    {
        if (entry->d_type == DT_DIR)
            return 1;
        if (entry->d_type == DT_UNKNOWN)
        {
            struct stat st;
            auto ret = m_filesystem->lstat(m_path_buffer, &st);
            if (ret < 0)
                LOG_ERRNO_RETURN(0, -1, "failed to lstat '`'", m_path);
            return S_ISDIR(st.st_mode);
        }
        return 0;
    }
    void Walker::path_push_back(string_view s)
    {
        auto len0 = m_path.length();
        auto len1 = s.length();
        assert(len0 + len1 < sizeof(m_path_buffer) - 1);
        memcpy(m_path_buffer + len0, s.data(), len1 + 1);
        m_path = string_view(m_path_buffer, len0 + len1);
    }
    void Walker::path_pop_back(size_t len1)
    {
        auto len0 = m_path.length();
        assert(len0 > len1);
        len0 -= len1;
        m_path_buffer[len0] = '\0';
        m_path = string_view(m_path_buffer, len0);
    }
    int Walker::next()
    {
    again:
        if (m_path.empty()) return -1;
        if (m_path.back() != '/')
        {
            auto m = m_path.rfind('/');
            if (m != m_path.npos) {
              auto len0 = m_path.length();
              path_pop_back(len0 - m - 1);
              m_stack.top()->next();
            }
        }

        dirent* entry;
        string_view name;
        while(true)
        {
            entry = m_stack.top()->get();
            if (entry) {
              name = entry->d_name;
              if (!is_dots(name)) break;
              if (m_stack.top()->next() == 1) continue;
            }
            m_stack.pop();
            if (m_stack.empty())
            {
                m_path.remove_prefix(m_path.length());
                return -1; // finished walking
            }
            assert(m_path.back() == '/');
            path_pop_back(1);
            goto again;
        }

        path_push_back(name);
        int ret = is_dir(entry);
        if (ret < 0) {
            return -1;  // fail
        } else if (ret == 1) {
            enter_dir();
            path_push_back("/");
            goto again;
        } /* else if (ret == 0) */
        return 0;   // file
    }
}
}
