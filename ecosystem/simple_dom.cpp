#define protected public
#include "simple_dom.h"
#undef protected

#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <vector>
#include <algorithm>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/utility.h>
#include <photon/common/estring.h>
#include <photon/common/stream.h>
#include <photon/common/retval.h>
#include <photon/fs/localfs.h>
#include <photon/fs/filesystem.h>
#include <rapidxml.hpp>
#include <rapidjson/reader.h>
#define RYML_SINGLE_HDR_DEFINE_NOW
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <rapidyaml-0.5.0.hpp>
#pragma GCC diagnostic pop

using namespace std;

namespace photon {
namespace SimpleDOM {

inline int NodeImpl::init_non_root(str key, str value,
                const NodeImpl* root, uint32_t flags) {
    _root = root;
    assert(root);
    _flags = flags & ~FLAG_IS_ROOT;

    assert(key.length() <= MAX_KEY_LENGTH);
    assert(value.length() <= MAX_VALUE_LENGTH);
    _k_len = key.length();
    _v_len = value.length();

    auto text_begin = root->_text_begin;
    assert(key.empty() || key.data() > text_begin);
    assert(value.empty() || value.data() > key.end());
    uint64_t koff, voff;
    switch ((key.empty() << 1) | value.empty()) {
        case 0:     // key && value
            koff = key.data() - text_begin;
            voff = value.data() - key.end();
            break;
        case 1:     // key && !value
            koff = key.data() - text_begin;
            voff = 0;
            break;
        case 2:     // !key && value
            koff = value.data() - text_begin;
            voff = 0;
            break;
        case 3:     // !key && !value
            _k_off = 0; _k_len = 0;
            _v_off = 0; _v_len = 0;
            return 0;
        default:
            assert(false);
            return -1;
    }
    assert(koff <= MAX_KEY_OFFSET);
    assert(voff <= MAX_VALUE_OFFSET);
    _k_off = koff;
    _v_off = voff;
    return 0;
}

inline int NodeImpl::init_root(const char* text_begin,
                uint32_t node_size, bool text_ownership) {
    _flags = FLAG_IS_ROOT | FLAG_IS_LAST;
    if (text_ownership)
        _flags |= FLAG_TEXT_OWNERSHIP;
    _text_begin = text_begin;
    assert(node_size <= MAX_NODE_SIZE);
    _node_size = node_size;
    _refcnt = 0;
    return 0;
}

template<typename Derived>
class DocNode : public NodeImpl {
public:
    vector<Derived> _children;
    DocNode() = default;
    DocNode(DocNode&&) = default;
    DocNode& operator=(DocNode&&) = default;
    DocNode(const char* text_begin, bool text_ownership) {
        init_root(text_begin, sizeof(Derived), text_ownership);
    }
    DocNode(str key, str value, const NodeImpl* root) {
        init_non_root(key, value, root, 0);
    }
    DocNode(const NodeImpl* root) : DocNode({}, {}, root) { }
    void print_children(int depth) {
        for (auto& x: _children) {
            auto k = x.get_key(), v = x.get_value();
            LOG_DEBUG(VALUE(depth), k, ':', v);
        }
    }
    struct Compare {
        const DocNode* _this;
        bool operator()(uint32_t i, uint32_t j) const {
            auto si = _this->_children[i].get_key();
            auto sj = _this->_children[j].get_key();
            return si < sj || (si == sj && i < j);
        }
        bool operator()(str s, uint32_t j) const {
            return s < _this->_children[j].get_key();
        }
        bool operator()(uint32_t i, str s) const {
            return _this->_children[i].get_key() < s;
        }
    };
    std::unique_ptr<uint32_t[]> _index;
    void set_children(vector<Derived>&& nodes, bool _indexing = true) {
        if (nodes.empty()) return;
        assert(nodes.size() <= MAX_NCHILDREN);
        if (nodes.size() > MAX_NCHILDREN)
            nodes.resize(MAX_NCHILDREN);
        _children = std::move(nodes);
        _nchildren = _children.size();
        if (_indexing) {
            _index.reset(new uint32_t[_nchildren]);
            for (size_t i = 0; i < _nchildren; ++i) _index[i] = i;
            std::sort(_index.get(), _index.get() + _nchildren, Compare{this});
            Derived *a, *b = &_children[_index[0]];
            for (size_t i = 1; i < _nchildren; ++i) {
                a = b; b = &_children[_index[i]];
                if (a->get_key() != b->get_key())
                    a->_flags |= FLAG_EQUAL_KEY_LAST;
            }
            b->_flags |= FLAG_EQUAL_KEY_LAST;
        }
        // user-side has no idea about # of children,
        // so we use this flag to indicate ending
        _children.back()._flags |= FLAG_IS_LAST;
    }
    ~DocNode() override {
        if (is_root()) {
            assert(_refcnt == 0);
            if (_flags & FLAG_TEXT_OWNERSHIP)
                free((void*)_text_begin);
        }
    }
    const NodeImpl* get(size_t i) const override {
        return (i < _nchildren) ? &_children[i] : nullptr;
    }
    const NodeImpl* get(str key) const override {
        if (_children.empty()) return nullptr;
        for (size_t i = 0; i < _nchildren - 1U; ++i) {
           assert((_children[i]._flags & FLAG_IS_LAST) == 0);
        }
        assert(_children.back()._flags & FLAG_IS_LAST);
        if (!_index) return nullptr;
        auto end = _index.get() + _nchildren;
        auto it = std::lower_bound(_index.get(), end, key, Compare{this});
        if (it == end || *it >= _nchildren || key != _children[*it].get_key())
            return nullptr;
        return &_children[*it];
    }
};

using namespace rapidjson;
class JNode : public DocNode<JNode> {
public:
    using DocNode::DocNode;
};

struct JHandler : public BaseReaderHandler<UTF8<>, JHandler> {
    vector<vector<JNode>> _nodes{1};
    str _key;
    JNode* _root;
    JHandler(const char* text, bool text_ownership) {
        assert(_nodes.size() == 1);
        _root = new JNode(text, text_ownership);
    }
    ~JHandler() {
        assert(_nodes.size() == 1);
        assert(_nodes.front().size() == 1);
        _root->set_children(std::move(_nodes.front().front()._children));
    }
    JNode* get_root() {
        return _root;
    }
    void emplace_back(const char* s, size_t length) {
        str val{s, length};     // _key may be empty()
        _nodes.back().emplace_back(_key, val, _root);
        // LOG_DEBUG(_key, ": ", val);
        _key = {};
    }
    bool Null() {
        emplace_back(0, 0);
        return true;
    }
    bool Key(const char* s, SizeType len, bool copy) {
        assert(!copy);
        _key = {s, len};
        return true;
    }
    bool String(const char* s, SizeType len, bool copy) {
        assert(!copy);
        emplace_back(s, len);
        return true;
    }
    bool RawNumber(const Ch* s, SizeType len, bool copy) {
        assert(!copy);
        // LOG_DEBUG(ALogString(s, len));
        emplace_back(s, len);
        return true;
    }
    bool RawBool(const Ch* s, SizeType len, bool copy) {
        assert(!copy);
        emplace_back(s, len);
        return true;
    }
    bool StartObject() {
        emplace_back(0, 0);
        _nodes.emplace_back();
        return true;
    }
    bool EndObject(SizeType memberCount) {
        commit(true);
        return true;
    }
    void commit(bool _indexing) {
        assert(_nodes.size() > 1);
        auto temp = std::move(_nodes.back());
        _nodes.pop_back();
        assert(_nodes.back().size() > 0);
        // LOG_DEBUG(temp.size(), " elements to ", _nodes.back().back().get_key(), VALUE(_indexing));
        _nodes.back().back().set_children(std::move(temp), _indexing);
    }
    bool StartArray() {
        emplace_back(0, 0);
        _nodes.emplace_back();
        return true;
    }
    bool EndArray(SizeType elementCount) {
        commit(false);
        return true;
    }
};

static NodeImpl* parse_json(char* text, size_t size, int flags) {
    const auto kFlags = kParseNumbersAsStringsFlag | kParseBoolsAsStringFlag |
             kParseInsituFlag | kParseCommentsFlag | kParseTrailingCommasFlag;
    JHandler h(text, flags & DOC_FREE_TEXT_ON_DESTRUCTION);
    using Encoding = UTF8<>;
    GenericInsituStringStream<Encoding> s(text);
    GenericReader<Encoding, Encoding> reader;
    reader.Parse<kFlags>(s, h);
    return h.get_root();
}

using namespace rapidxml;
class XMLNode : public DocNode<XMLNode> {
public:
    using DocNode::DocNode;
    unique_ptr<XMLNode> __attributes__{nullptr};
    retval<XMLNode*> emplace_back(vector<XMLNode>& nodes, xml_base<char>* x) {
        if (x->name_size() == 0)
            return {nullptr, ECANCELED};
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
    auto root = new XMLNode(text, flags & DOC_FREE_TEXT_ON_DESTRUCTION);
    assert(root);
    root->build(&doc);
    return root;
}

class YAMLNode : public DocNode<YAMLNode> {
public:
    using DocNode::DocNode;
    str _to_str(ryml::csubstr s) {
        return {s.str, s.len};
    }
    void build(ryml::ConstNodeRef yaml_node, int depth = 0) {
        vector<YAMLNode> nodes;
        for (const auto& x: yaml_node.children()) {
            assert(x.has_key() != yaml_node.is_seq());
            str k, v;
            if (x.has_key()) k = _to_str(x.key());
            if (x.has_val()) v = _to_str(x.val());
            // LOG_DEBUG(k, ':', v);
            nodes.emplace_back(k, v, get_root());
            nodes.back().build(x, depth + 1);
        }
        set_children(std::move(nodes), !yaml_node.is_seq());
    }
};

static NodeImpl* parse_yaml(char* text, size_t size, int flags) {
    auto yaml = ryml::parse_in_place({text, size});
    auto root = new YAMLNode(text, flags & DOC_FREE_TEXT_ON_DESTRUCTION);
    assert(root);
    root->build(yaml.rootref());
    return root;
}

class IniNode : public DocNode<IniNode> {
public:
    using DocNode<IniNode>::DocNode;
};

using ini_handler = void (*)(void*, estring_view, estring_view, estring_view);
static int do_parse_ini(estring_view text, ini_handler h, void* user) {
    int err_cnt = 0;
    estring_view section;
    for (auto line: text.split_lines()) {
        auto comment = line.rfind(" ;");
        if (comment < line.size())
            line = line.substr(0, comment);
        line = line.trim();
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            if (line.back() == ']') {
                section = line.substr(1, line.size() - 2).trim();
            } else {
                err_cnt++;
                LOG_DEBUG("section with no ending: ", line);
            }
        } else {
            auto eq = line.find_first_of(charset("=:"));
            if (eq > 0 && eq < line.size() - 1) {
                auto key = line.substr(0, eq).trim();
                auto val = line.substr(eq + 1).trim();
                h(user, section, key, val);
            } else {
                err_cnt++;
                LOG_DEBUG("ill formed kv: ", line);
            }
        }
    }
    return -err_cnt;
}

static NodeImpl* parse_ini(char* text, size_t size, int flags) {
    struct Item {
        estring_view section, key, val;
        bool operator < (const Item& rhs) const {
            return section < rhs.section;
        }
    };
    vector<Item> ctx;
    auto handler = [](void* user, estring_view section,
                estring_view key, estring_view val) {
        auto ctx = (vector<Item>*)user;
        ctx->push_back({section, key, val});
        LOG_DEBUG(VALUE(section), VALUE(key), VALUE(val));
    };
    int ret = do_parse_ini({text, size}, handler, &ctx);
    if (ret < 0 && ctx.empty())
        LOG_ERROR_RETURN(-1, nullptr, "ini_parse_string_length() failed: ", ret);

    sort(ctx.begin(), ctx.end());
    vector<IniNode> sections, nodes;
    estring_view prev_sect;
    auto root = new IniNode(text, flags & DOC_FREE_TEXT_ON_DESTRUCTION);
    for (auto& x : ctx) {
        if (prev_sect != x.section) {
            prev_sect  = x.section;
            if (!nodes.empty() && !sections.empty()) {
                sections.back().set_children(std::move(nodes));
                assert(nodes.empty());
            }
            sections.emplace_back(x.section, str{}, root);
        }
        nodes.emplace_back(x.key, x.val, root);
    }
    if (!sections.empty()) {
        if (!nodes.empty())
            sections.back().set_children(std::move(nodes));
        root->set_children(std::move(sections));
    }
    return root;
}

Node parse(char* text, size_t size, int flags) {
    if (!text || !size)
        LOG_ERROR_RETURN(EINVAL, nullptr, "invalid argument:", VALUE(text), VALUE(size));
    using Parser = NodeImpl* (*) (char* text, size_t size, int flags);
    constexpr static Parser parsers[] = {&parse_json, &parse_xml,
                                         &parse_yaml, &parse_ini};
    auto i = flags & DOC_TYPE_MASK;
    if ((size_t) i > LEN(parsers)) {
        if (flags & DOC_FREE_TEXT_IF_PARSING_FAILED) free(text);
        LOG_ERROR_RETURN(EINVAL, nullptr, "invalid document type ", HEX(i));
    }
    auto r = parsers[i](text, size, flags);
    if (!r && (flags & DOC_FREE_TEXT_IF_PARSING_FAILED)) free(text);
    return r;
}

Node parse_file(fs::IFile* file, int flags) {
    return parse(file->readall(), flags | DOC_OWN_TEXT);
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
