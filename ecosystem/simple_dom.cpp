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
#include "rapidjson/reader.h"

using namespace std;

namespace photon {
namespace SimpleDOM {

template<typename Derived>
class DocNode : public NodeImpl {
public:
    vector<Derived> _children;
    DocNode() = default;
    DocNode(const char* text_begin) {
        init(text_begin, sizeof(Derived));
    }
    DocNode(str key, str value, const NodeImpl* root) {
        init(key, value, root, 0);
    }
    DocNode(const NodeImpl* root) : DocNode({0,0}, {0,0}, root) { }
    void print_children(int depth) {
        for (auto& x: _children) {
            auto k = x.get_key(), v = x.get_value();
            LOG_DEBUG(VALUE(depth), k, ':', v);
        }
    }
    void set_children(vector<Derived>&& nodes) {
        if (nodes.empty()) return;
        assert(nodes.size() <= MAX_NCHILDREN);
        if (nodes.size() > MAX_NCHILDREN)
            nodes.resize(MAX_NCHILDREN);
        sort(nodes.begin(), nodes.end());
        nodes.back()._flags |= FLAG_IS_LAST;    // must be after sort!!!
        _nchildren = nodes.size();
        _children = std::move(nodes);
    }
    const NodeImpl* get(size_t i) const override {
        return (i < _children.size()) ? &_children[i] : nullptr;
    }
    const NodeImpl* get(str key) const override {
        if (_children.empty()) return nullptr;
        for (size_t i = 0; i < _children.size() - 1; ++i) {
           assert((_children[i]._flags & FLAG_IS_LAST) == 0);
        }
        assert(_children.back()._flags & FLAG_IS_LAST);
        auto it = std::lower_bound(_children.begin(), _children.end(), key);
        return (it == _children.end() || it->get_key() != key) ? nullptr : &*it;
    }
};

using namespace rapidjson;
class JNode : public DocNode<JNode> {
public:
    using DocNode::DocNode;
};

static NodeImpl* parse_json(char* text, size_t size, int flags) {
    const auto kFlags = kParseNumbersAsStringsFlag | kParseBoolsAsStringFlag |
             kParseInsituFlag | kParseCommentsFlag | kParseTrailingCommasFlag;
    using Encoding = UTF8<>;
    BaseReaderHandler<Encoding> h;
    GenericInsituStringStream<Encoding> s(text);
    GenericReader<Encoding, Encoding> reader;
    reader.Parse<kFlags>(s, h);
    return nullptr;
}

using namespace rapidxml;
class XMLNode : public DocNode<XMLNode> {
public:
    using DocNode::DocNode;
    unique_ptr<XMLNode> __attributes__{nullptr};
    retval<XMLNode*> emplace_back(vector<XMLNode>& nodes, xml_base<char>* x) {
        if (x->name_size() == 0)
            return {ECANCELED, 0};
        str k{x->name(),  x->name_size()};
        str v{x->value(), x->value_size()};
        nodes.emplace_back(k, v, get_root());
        // LOG_DEBUG(k, ':', v);
        return &nodes.back();
    }
    void build(xml_node<char>* xml_node, int depth = 0) {
        vector<XMLNode> nodes;
        for (auto x = xml_node->first_node(); x;
                  x = x->next_sibling()) {
            auto ret = emplace_back(nodes, x);
            if (ret.succeeded())
                ret->build(x, depth + 1);
        }
        set_children(std::move(nodes));

        assert(nodes.empty());
        if (auto x = xml_node->first_attribute()) {
            do { emplace_back(nodes, x); }
            while((x = x->next_attribute()));
            auto a = new XMLNode(get_root());
            a->set_children(std::move(nodes));
            __attributes__.reset(a);
        }
   }
    const NodeImpl* get(str key) const override {
        return (key != "__attributes__") ?
            DocNode::get(key) : __attributes__.get();
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
