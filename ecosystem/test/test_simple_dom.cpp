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

#include "../simple_dom.h"
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
#include <memory>
#include <string>
#include <algorithm>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include "../../test/gtest.h"

using namespace std;
using namespace photon::SimpleDOM;

// OSS list response
const static char xml[] = R"(
<?xml version="1.0" encoding="UTF-8"?>
<ListBucketResult category = "flowers">
  <Name>examplebucket</Name>
  <Prefix></Prefix>
  <Marker>test1.txt</Marker>
  <MaxKeys>2</MaxKeys>
  <Delimiter></Delimiter>
  <EncodingType>url</EncodingType>
  <IsTruncated>true</IsTruncated>
  <NextMarker>test100.txt</NextMarker>
  <Contents>
    <Key>test10.txt</Key>
    <LastModified>2020-05-26T07:50:18.000Z</LastModified>
    <ETag>"C4CA4238A0B923820DCC509A6F75****"</ETag>
    <Type>Normal</Type>
    <Size>1</Size>
    <StorageClass>Standard</StorageClass>
    <Owner>
      <ID>1305433xxx</ID>
      <DisplayName>1305433xxx</DisplayName>
    </Owner>
  </Contents>
  <asdf>asdf</asdf>
  <Contents>
    <Key>test100.txt</Key>
    <LastModified>2020-05-26T07:50:20.000Z</LastModified>
    <ETag>"C4CA4238A0B923820DCC509A6F75****"</ETag>
    <Type>Normal</Type>
    <Size>1</Size>
    <StorageClass>Standard</StorageClass>
    <Owner>
      <ID>1305433xxx</ID>
      <DisplayName>1305433xxx</DisplayName>
    </Owner>
  </Contents>
</ListBucketResult>
)";

using ObjectList = vector<tuple<long, unsigned char, string_view, int64_t, bool>>;

const long DT_DIR = 10;
const long DT_REG = 20;

void print_all1(Node node) {
    for (size_t i = 0; i < node.num_children(); ++i) {
        auto x = node.get(i);
        LOG_DEBUG(x.key(), '=', x.value());
    }
}

void print_all2(Node node) {
    for (auto x = node.get(0); x; x = x.next()) {
        LOG_DEBUG(x.key(), '=', x.value());
    }
}

static __attribute__((noinline))
int do_list_object(string_view prefix, ObjectList& result, string* marker) {
    auto doc = parse_copy(xml, sizeof(xml), DOC_XML);
    EXPECT_TRUE(doc);
    auto list_bucket_result = doc["ListBucketResult"];
    auto attr = list_bucket_result.get_attributes();
    EXPECT_EQ(attr.num_children(), 1);
    EXPECT_EQ(attr["category"], "flowers");
/*
    print_all1(list_bucket_result);
    auto c = list_bucket_result.get("Contents");
    LOG_DEBUG(VALUE(c.key()));
    print_all1(c);
    c = c.next();
    LOG_DEBUG(VALUE(c.key()));
    print_all2(c);
*/
    for (auto child: list_bucket_result.enumerable_children("Contents")) {
        auto key = child["Key"];
        EXPECT_TRUE(key);
        auto size = child["Size"];
        EXPECT_TRUE(size);
        auto text = key.to_string_view();
        auto dsize = size.to_int64_t();
        LOG_DEBUG(VALUE(text), VALUE(dsize));
        result.emplace_back(0, DT_REG, text.substr(prefix.size()),
                                dsize, text.size() == prefix.size());
/*      if (m_stat_pool) {
            string_view fname(text);
            fname.back() == '/' ? update_stat_cache(fname, 0, OSS_DIR_MODE)
                                : update_stat_cache(fname, dsize, OSS_FILE_MODE);
        }
*/  }
    for (auto child: list_bucket_result.enumerable_children("CommonPrefixes")) {
        auto key = child["Prefix"];
        EXPECT_TRUE(key);
        auto dirname = key.to_string_view();
        if (dirname.back() == '/') dirname.remove_suffix(1);
        // update_stat_cache(dirname, 0, OSS_DIR_MODE);
        dirname.remove_prefix(prefix.size());
        result.emplace_back(0, DT_DIR, dirname, 0, false);
    }
    if (marker) {
        auto next_marker = list_bucket_result["NextMarker"];
        if  (next_marker) *marker = next_marker.to_string_view();
        else marker->clear();
    }
    return 0;
}

TEST(simple_dom, oss_list) {
    ObjectList list;
    string marker;
    do_list_object("", list, &marker);
    static ObjectList truth = {
        {0, DT_REG, "test10.txt",  1, false},
        {0, DT_REG, "test100.txt", 1, false},
    };
    EXPECT_EQ(list, truth);
    EXPECT_EQ(marker, "test100.txt");
}

void expect_eq_kvs(Node node, const char * const *  truth, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        auto x = truth + i * 2;
        auto q = node[x[0]];
        LOG_DEBUG("expect node['`'] => '`' (got `)", x[0], x[1], q.to_string_view());
        EXPECT_EQ(q, x[1]);
    }
}

template<size_t N> inline
void expect_eq_kvs(Node node, const char* const (&truth)[N][2]) {
     expect_eq_kvs(node, &truth[0][0], N);
}

void expect_eq_vals(Node node, const char * const *  truth, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        auto x = truth[i];
        auto q = node[i];
        LOG_DEBUG("expect node[`] => '`' (got '`')", i, x, q.to_string_view());
        EXPECT_EQ(q, x);
    }
}

template<size_t N> inline
void expect_eq_vals(Node node, const char * const (&truth)[N]) {
     expect_eq_vals(node, truth, N);
}

TEST(simple_dom, json) {
    const static char json0[] = R"({
        "hello": "world",
        "t": true ,
        "f": false,
        "n": null,
        "i": -123,
        "pi": 3.1416,
        "a": [1, 2, 3, 4],
    })";
    auto doc = parse_copy(json0, sizeof(json0), DOC_JSON);
    EXPECT_TRUE(doc);
    expect_eq_kvs(doc, {
        {"hello",   "world"},
        {"t",       "true"},
        {"f",       "false"},
        {"i",       "-123"},
        {"pi",      "3.1416"},
    });
    EXPECT_EQ(doc["i"].to_int64_t(), -123);
    EXPECT_NEAR(doc["pi"].to_double(), 3.1416, 1e-5);
    expect_eq_vals(doc["a"], {"1", "2", "3", "4"});
}

TEST(simple_dom, yaml0) {
    static char yaml0[] = "{foo: 1, bar: [2, 3], john: doe}";
    auto doc = parse(yaml0, sizeof(yaml0), DOC_YAML);
    EXPECT_TRUE(doc);
    expect_eq_kvs(doc, {{"foo", "1"}, {"john", "doe"}});
    expect_eq_vals(doc["bar"], {"2", "3"});
}

TEST(simple_dom, yaml1) {
    static char yaml1[] = R"(
foo: says who
bar:
- 20
- 30
- oh so nice
- oh so nice (serialized)
john: in_scope
float: 2.4
digits: 2.400000
newkeyval: shiny and new
newkeyval (serialized): shiny and new (serialized)
newseq: []
newseq (serialized): []
newmap: {}
newmap (serialized): {}
I am something: indeed
)";
    auto doc = parse(yaml1, sizeof(yaml1), DOC_YAML);
    EXPECT_TRUE(doc);
    expect_eq_kvs(doc, {
        {"foo",         "says who"},
        {"john",        "in_scope"},
        {"float",       "2.4"},
        {"digits",      "2.400000"},
        {"newkeyval",   "shiny and new"},
        {"I am something", "indeed"},
    });
    expect_eq_vals(doc["bar"], {"20", "30",
        "oh so nice", "oh so nice (serialized)"});
}

const static char example_ini[] = R"(
[protocol]               ; Protocol configuration
version=6                ; IPv6

[user]
name = Bob Smith         ; Spaces around '=' are stripped
email = bob@smith.com    ; And comments (like this) ignored
active = true            ; Test a boolean
pi = 3.14159             ; Test a floating point number
trillion = 1000000000000 ; Test 64-bit integers

[protocol]               ; Protocol configuration
  ver  =   4                ; IPv4

[section1]
single1 = abc
single2 = xyz
[section3]
single: ghi
multi: the quick
name = bob smith  ; comment line 1
                  ; comment line 2
foo = bar ;c1

[comment_test]
test1 = 1;2;3 ; only this will be a comment
test2 = 2;3;4;this won't be a comment, needs whitespace before ';'
test;3 = 345 ; key should be "test;3"
test4 = 4#5#6 ; '#' only starts a comment at start of line
#test5 = 567 ; entire line commented
 # test6 = 678 ; entire line commented, except in MULTILINE mode
test7 = ; blank value, except if inline comments disabled
test8 =; not a comment, needs whitespace before ';'

[colon_tests]
Content-Type: text/html
foo:bar
adams : 42
funny1 : with = equals
funny2 = with : colons
funny3 = two = equals
funny4 : two : colons


)";

TEST(simple_dom, ini) {
    auto doc = parse_copy(example_ini, sizeof(example_ini) - 1, DOC_INI);
    EXPECT_TRUE(doc);
    EXPECT_EQ(doc.num_children(), 6);
    expect_eq_kvs(doc["protocol"], {
        {"version",  "6"},
        {"ver",      "4"},
    });
    expect_eq_kvs(doc["user"], {
        {"name",     "Bob Smith"},
        {"email",    "bob@smith.com"},
        {"active",   "true"},
        {"pi",       "3.14159"},
        {"trillion", "1000000000000"},
    });
    expect_eq_kvs(doc["section1"], {
        {"single1",   "abc"},
        {"single2",   "xyz"},
    });
    expect_eq_kvs(doc["section3"], {
        {"single",    "ghi"},
        {"multi",     "the quick"},
        {"name",      "bob smith"},
        {"foo",       "bar"},
    });
    expect_eq_kvs(doc["comment_test"], {
        {"test1",  "1;2;3"},
        {"test2",  "2;3;4;this won't be a comment, needs whitespace before ';'"},
        {"test;3", "345"},
        {"test4",  "4#5#6"},
        {"test7",  ""},
        {"test8",  "; not a comment, needs whitespace before ';'"},
    });
    expect_eq_kvs(doc["colon_tests"], {
        {"Content-Type",  "text/html"},
        {"foo",     "bar"},
        {"adams",   "42"},
        {"funny1",  "with = equals"},
        {"funny2",  "with : colons"},
        {"funny3",  "two = equals"},
        {"funny4",  "two : colons"},
    });
}
