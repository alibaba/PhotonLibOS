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
// #include <gtest/gtest.h>

using namespace std;
using namespace photon::SimpleDOM;

#define EXPECT_EQ(a, b)
#define EXPECT_TRUE(x)

// OSS list response
const char xml[] = R"(
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

const static ObjectList truth = {
    {0, DT_REG, "test10.txt", 1, false},
    {0, DT_REG, "test100.txt", 1, false},
};

static __attribute__((noinline))
int do_list_object(const string& prefix,
                ObjectList& result, string* marker) {
    auto doc = parse_copy(xml, sizeof(xml), DOC_XML);
    EXPECT_TRUE(doc);
    auto list_bucket_result = doc["ListBucketResult"];
    auto attr = list_bucket_result.get_attributes();
    EXPECT_EQ(attr.num_children(), 1);
    EXPECT_EQ(attr["category"], "flowers");
    for (auto child: list_bucket_result.enumerable_children("Contents")) {
        auto key = child["Key"];
        EXPECT_TRUE(key);
        auto size = child["Size"];
        EXPECT_TRUE(size);
        auto text = key.to_string();
        auto dsize = size.to_integer();
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
        auto dirname = key.to_string();
        if (dirname.back() == '/') dirname.remove_suffix(1);
        // update_stat_cache(dirname, 0, OSS_DIR_MODE);
        dirname.remove_prefix(prefix.size());
        result.emplace_back(0, DT_DIR, dirname, 0, false);
    }
    if (marker) {
        auto next_marker = list_bucket_result["NextMarker"];
        *marker = next_marker ? next_marker.to_string() : "";
    }
    return 0;
}

// TEST(simple_dom, oss_list) {
void simple_dom_oss_list() {
    ObjectList list;
    string marker;
    do_list_object("", list, &marker);
    EXPECT_EQ(list, truth);
    EXPECT_EQ(marker, "test100.txt");
}

// TEST(simple_dom, example) {
void simple_dom_examples() {
    auto doc = parse(nullptr, 0, DOC_JSON);
    auto a = doc["asdf"].to_string();
    auto j = doc["jkl"].to_integer();
    auto sb = doc["foo"]["bar"];
    for (auto x: sb.enumerable_children()) {
        // ...
    }
    for (auto x: sb.enumerable_same_key_siblings()) {
        // ...
    }
}

// int main(int argc, char **argv) {
//     ::testing::InitGoogleTest(&argc, argv);
//     return RUN_ALL_TESTS();
// }
