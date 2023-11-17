#include "simple_dom.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/common/stream.h>
#include <photon/fs/localfs.h>

namespace photon {
namespace SimpleDOM {

static Document parse_json(char* text, size_t size, int flags) {
/*
    using namespace rapidjson;
    auto flags = kParseInsituFlag   | kParseNumbersAsStringsFlag |
                 kParseCommentsFlag | kParseTrailingCommasFlag   |
                 kParseNanAndInfFlag;
    Reader reader;
    reader.Parse<flags>(stream(text), handler);
*/
    return {nullptr};
}

static Document parse_xml(char* text, size_t size, int flags) {
    return {nullptr};
}

static Document parse_yaml(char* text, size_t size, int flags) {
    return {nullptr};
}

static Document parse_ini(char* text, size_t size, int flags) {
    return {nullptr};
}

Document parse(char* text, size_t size, int flags) {
    using Parser = Document(*)(char* text, size_t size, int flags);
    const static Parser parsers[] = {
        &parse_json, &parse_xml, &parse_yaml, &parse_ini};
    size_t i = flags & 0xff;
    if (i > LEN(parsers)) {
        if (flags | TEXT_OWNERSHIP) free(text);
        LOG_ERROR_RETURN(EINVAL, {nullptr}, "invalid flags ", HEX(flags));
    }
    return parsers[i](text, size, flags);
}

Document parse_stream(IStream* s, int flags, size_t max_size) {
    size_t capacity = 1024, size = 0;
    auto buf = (char*)malloc(capacity);
    while(true) {
        ssize_t ret = s->read(buf + size, capacity - size);
        if (ret < 0) {
            free(buf);
            LOG_ERRNO_RETURN(0, {nullptr}, "failed to read from stream");
        }
        if (ret == 0) { // EOF
            return parse(buf, size, flags);
        }
        size += ret;
        assert(size <= capacity);
        if (size == capacity) {
            if (capacity >= max_size) {
                free(buf);
                LOG_ERROR_RETURN(ENOBUFS, {nullptr}, "content in stream is too large to fit into buffer");
            }
            buf = (char*)realloc(buf, capacity *= 2);
        }
    }
}

Document parse_filename(const char* filename, int flags) {
    using namespace fs;
    auto file = open_localfile_adaptor(filename, O_RDONLY);
    if (!file)
        LOG_ERRNO_RETURN(0, {nullptr}, "failed to open file ", filename);
    DEFER(delete file);
    struct stat st;
    int error = file->fstat(&st);
    size_t max_size = error ? -1 : st.st_size;
    return parse_stream(file, flags, max_size);
}

}
}
