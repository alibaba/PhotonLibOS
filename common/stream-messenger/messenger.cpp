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

#include "messenger.h"
#include <photon/common/alog.h>

namespace StreamMessenger
{
    // This class turns a stream into a message channel, by adding a `Header` to the
    // message before send it, and extracting the header immediately after recv it.
    // The stream can be any `IStream` object, like TCP socket or UNIX domain socket.
    class Messenger : public IMessageChannel
    {
    public:
        IStream* m_stream;
        Header m_header;       // used for recv, in case of insufficient buffer space
        Messenger(IStream* stream)
        {
            m_stream = stream;
            m_header.magic = 0;
        }
        virtual ssize_t send(iovector& msg) override
        {
            auto size = msg.sum();
            if (size > UINT32_MAX - sizeof(Header))
                LOG_ERRNO_RETURN(EINVAL, -1, "the message is too large to send (` > 4GB)", size);

            Header h;
            h.size = (uint32_t)size;
            msg.push_front({&h, sizeof(h)});
            ssize_t ret = m_stream->writev(msg.iovec(), msg.iovcnt());
            if (ret < (ssize_t)(size + sizeof(h)))
                LOG_ERROR_RETURN(0, -1, "failed to write to underlay stream ", m_stream);

            return (ssize_t)size;
        }
        virtual ssize_t recv(iovector& msg) override
        {
            if (m_header.magic != Header::MAGIC)
            {
                ssize_t ret = m_stream->read(&m_header, sizeof(m_header));
                if (ret < (ssize_t)sizeof(m_header))
                    LOG_ERRNO_RETURN(0, -1, "failed to read the header from stream ", m_stream);
                if (m_header.magic != Header::MAGIC)
                    LOG_ERROR_RETURN(EBADMSG, -1, "invalid header magic in the recvd message");
                if (m_header.version > Header::VERSION)
                    LOG_ERROR_RETURN(EBADMSG, -1 , "invlaid header version in the recvd message");
            }

            size_t size = msg.truncate(m_header.size);
            auto delta = (ssize_t)size - (ssize_t)m_header.size;
            if (delta < 0)
                LOG_ERROR_RETURN(ENOBUFS, delta, "insufficient buffer space (and allocation "
                                 "failed), need ` more bytes", -delta);

            m_header.magic = 0;   // clear the magic state
            ssize_t ret = m_stream->readv(msg.iovec(), msg.iovcnt());
            if (ret < (ssize_t)m_header.size)
                LOG_ERRNO_RETURN(0, -1, "failed to readv the message from stream ", m_stream);

            return m_header.size;
        }
        virtual uint64_t flags() override
        {
            return 0;
        }
        virtual uint64_t flags(uint64_t f) override
        {
            return f;
        }
    };
    IMessageChannel* new_messenger(IStream* stream)
    {
        return new Messenger(stream);
    }
}
