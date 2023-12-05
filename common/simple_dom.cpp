#include "simple_dom.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/common/stream.h>
#include <photon/fs/localfs.h>
#include <photon/fs/filesystem.h>


namespace photon {
namespace SimpleDOM {

static NodeImpl* parse_json(char* text, size_t size, int flags) {
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

static NodeImpl* parse_xml(char* text, size_t size, int flags) {
    return {nullptr};
}

static NodeImpl* parse_yaml(char* text, size_t size, int flags) {
    return {nullptr};
}

static NodeImpl* parse_ini(char* text, size_t size, int flags) {
    return {nullptr};
}

NodeImpl* parse(char* text, size_t size, int flags) {
    if (!text || !size)
        LOG_ERROR_RETURN(EINVAL, nullptr, "invalid argument:", VALUE(text), VALUE(size));
    using Parser = NodeImpl* (*) (char* text, size_t size, int flags);
    constexpr static Parser parsers[] = {&parse_json, &parse_xml,
                                         &parse_yaml, &parse_ini};
    auto i = flags & DOC_TYPE_MASK;
    if (i > LEN(parsers)) {
        if (flags & FLAG_FREE_TEXT_IF_PARSING_FAILED) free(text);
        LOG_ERROR_RETURN(EINVAL, nullptr, "invalid document type ", HEX(i));
    }
    return parsers[i](text, size, flags);
}

NodeImpl* parse_filename(const char* filename, int flags, fs::IFileSystem* fs) {
    using namespace fs;
    auto file = fs ? fs->open(filename, O_RDONLY) :
       open_localfile_adaptor(filename, O_RDONLY);
    if (!file)
        LOG_ERRNO_RETURN(0, nullptr, "failed to open file ", filename);
    DEFER(delete file);
    return parse(file->readall(), flags | FLAG_FREE_TEXT_IF_PARSING_FAILED);
}

}
}
