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

#include "iostream.h"
#include <photon/common/alog.h>

namespace photon {
namespace net {

#define _EOF traits_type::eof()
#define NOT_EOF(x) traits_type::not_eof((x))
#define TO_INT(x) traits_type::to_int_type((x))
#define EQ_INT(a, b) traits_type::eq_int_type((a), (b))

class streambuf : public std::streambuf {
    ISocketStream* _sock;
    char _ibuf[1024];
    char _obuf[1024];
    bool _owner;

public:
    explicit streambuf(ISocketStream* sock, bool owner_ship) :
        _sock(sock), _owner(owner_ship) { }

    ~streambuf() {
        overflow(_EOF);
        if (_owner) delete _sock;
    }

    int underflow() override { // read
        assert(!gptr() || gptr() >= egptr());
        auto ret = _sock->recv(_ibuf, sizeof(_ibuf));
        if (ret < 0)
            LOG_ERRNO_RETURN(0, _EOF, "failed to read socket");
        if (ret == 0)
            LOG_ERRNO_RETURN(0, _EOF, "failed to read socket: closed");

        setg(_ibuf, _ibuf, _ibuf + ret);
        return TO_INT(*gptr());
    }

    int_type overflow(int_type value) override  { // write
        auto count = pptr() - pbase();
        if (count) {
            auto ret = _sock->write(_obuf, count);
            if (ret < 0)
                LOG_ERRNO_RETURN(0, _EOF, "failed to write to socket ");
            if (ret < count)
                LOG_ERROR_RETURN(0, _EOF, "failed to write to closed socket");
        }

        setp(_obuf, _obuf + sizeof(_obuf));
        if (!EQ_INT(value, _EOF)) sputc(value);
        return NOT_EOF(value);
    };

    int sync() override {
        auto ret = overflow(_EOF);
        return EQ_INT(ret, _EOF) ? -1 : 0;
    }
};

class iostream : public streambuf, public std::iostream {
public:
    iostream(ISocketStream* sock, bool owner_ship) :
        streambuf(sock, owner_ship), std::iostream(this) { }
};

std::iostream* new_iostream(ISocketStream* sock, bool owner_ship) {
    return new iostream(sock, owner_ship);
}


}
}
