#define protected public
#include "simple_dom.h"
#undef protected

#include <fcntl.h>
#include <sys/stat.h>
#include <vector>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/utility.h>
#include <photon/common/stream.h>
#include <photon/common/retval.h>
#include <photon/fs/localfs.h>
#include <photon/fs/filesystem.h>
#include "RapidXml/rapidxml.hpp"

using namespace std;

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
    return nullptr;
}

using namespace rapidxml;
class XMLNode : public NodeImpl {
public:
    vector<XMLNode> _children;
    XMLNode* _attributes = nullptr;
    XMLNode() = default;
    XMLNode(const char* text_begin) {
        init(text_begin, sizeof(*this));
    }
    XMLNode(str key, str value, const NodeImpl* root) {
        init(key, value, root, 0);
    }
    XMLNode(const NodeImpl* root) : XMLNode({0,0}, {0,0}, root) { }
    void print_nodes(int depth, const vector<XMLNode>& nodes) {
        for (auto& x: nodes) {
            auto k = x.get_key(), v = x.get_value();
            LOG_DEBUG(VALUE(depth), k, ':', v);
        }
    }
    void set_children(vector<XMLNode>&& nodes) {
        if (nodes.size()) {
            assert(nodes.size() <= MAX_NCHILDREN);
            if (nodes.size() > MAX_NCHILDREN)
                nodes.resize(MAX_NCHILDREN);
            sort(nodes.begin(), nodes.end());
            nodes.back()._flags |= FLAG_IS_LAST;    // must be after sort!!!
            _nchildren = nodes.size();
            _children = std::move(nodes);
        }
    }
    void build(xml_node<char>* xml_node, int depth = 0) {
        assert(xml_node);
        vector<XMLNode> nodes;
        auto root = get_root();
        for (auto x = xml_node->first_attribute(); x;
                  x = x->next_attribute()) {
            str k{x->name(),  x->name_size()};
            if (k.empty()) continue;
            str v{x->value(), x->value_size()};
            nodes.emplace_back(k, v, root);
        }
        if (nodes.size()) {
            _attributes = new XMLNode(root);
            _attributes->set_children(std::move(nodes));
            print_nodes(depth, _attributes->_children);
        }

        assert(nodes.empty());
        for (auto x = xml_node->first_node(); x;
                  x = x->next_sibling()) {
            str k{x->name(),  x->name_size()};
            if (k.empty()) continue;
            str v{x->value(), x->value_size()};
            nodes.emplace_back(k, v, root);
            nodes.back().build(x, depth + 1);
        }
        set_children(std::move(nodes));
        print_nodes(depth, _children);
    }
    virtual const NodeImpl* get(size_t i) const override {
        return (i < _children.size()) ? &_children[i] : nullptr;
    }
    virtual const NodeImpl* get(str key) const override {
        if ("__attributes__" == key)
            return _attributes;

        assert(_children.empty() || (_children.back()._flags & FLAG_IS_LAST));
        auto it = std::lower_bound(_children.begin(), _children.end(), key);
        return (it == _children.end() || it->get_key() != key) ? nullptr : &*it;
    }
};

static NodeImpl* parse_xml(char* text, size_t size, int flags) {
    xml_document<char> doc;
    doc.parse<0>(text);
    auto root = new XMLNode(text);
    assert(root);
    root->build(&doc);
    return root;
}

static NodeImpl* parse_yaml(char* text, size_t size, int flags) {
    return nullptr;
}

static NodeImpl* parse_ini(char* text, size_t size, int flags) {
    return nullptr;
}

Node parse(char* text, size_t size, int flags) {
    if (!text || !size)
        LOG_ERROR_RETURN(EINVAL, nullptr, "invalid argument:", VALUE(text), VALUE(size));
    using Parser = NodeImpl* (*) (char* text, size_t size, int flags);
    constexpr static Parser parsers[] = {&parse_json, &parse_xml,
                                         &parse_yaml, &parse_ini};
    auto i = flags & DOC_TYPE_MASK;
    if ((size_t) i > LEN(parsers)) {
        if (flags & FLAG_FREE_TEXT_IF_PARSING_FAILED) free(text);
        LOG_ERROR_RETURN(EINVAL, nullptr, "invalid document type ", HEX(i));
    }
    return parsers[i](text, size, flags);
}

Node parse_file(fs::IFile* file, int flags) {
    return parse(file->readall(), flags | FLAG_FREE_TEXT_IF_PARSING_FAILED);
}

Node parse_file(const char* filename, int flags, fs::IFileSystem* fs) {
    using namespace fs;
    auto file = fs ? fs->open(filename, O_RDONLY) :
       open_localfile_adaptor(filename, O_RDONLY) ;
    if (!file)
        LOG_ERRNO_RETURN(0, nullptr, "failed to open file ", filename);
    DEFER(delete file);
    return parse_file(file, flags);
}

}
}
