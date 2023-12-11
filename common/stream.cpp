#include "stream.h"
#include <stdlib.h>
#include "alog.h"


IStream::ReadAll IStream::readall(size_t max_buf, size_t min_buf) {
    ReadAll buf;
    buf.size = 0;
    ssize_t capacity = min_buf;
    auto ptr = (char*)malloc(capacity);
    if (!ptr)
        LOG_ERROR_RETURN(ENOBUFS, buf, "failed to malloc(`)", capacity);
    buf.ptr.reset(ptr);
    while(true) {
        ssize_t ret = this->read((char*)buf.ptr.get() + buf.size, capacity - buf.size);
        if (ret < 0) {
            buf.size = -buf.size;
            LOG_ERRNO_RETURN(0, buf, "failed to read from stream");
        }
        if (ret == 0) { // EOF
            return buf;
        }
        buf.size += ret;
        assert(buf.size <= capacity);
        if (unlikely(buf.size == capacity)) {
            if (capacity >= max_buf) {
                buf.size = -buf.size;
                LOG_ERROR_RETURN(ENOBUFS, buf, "content size in stream exceeds upper limit ", max_buf);
            }
            auto ptr = realloc(buf.ptr.get(), capacity *= 2);
            if (!ptr) {
                buf.size = -buf.size;
                LOG_ERROR_RETURN(ENOBUFS, buf, "failed to realloc(`)", capacity);
            }
            buf.ptr.reset(ptr);
        }
    }
}

