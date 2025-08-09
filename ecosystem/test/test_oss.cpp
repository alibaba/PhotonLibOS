#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <photon/common/estring.h>
#include <photon/photon.h>

#include <chrono>
#include <cstdlib>
#include <string>

#include "../oss.h"

DEFINE_string(ak, "", "OSS Access Key ID");
DEFINE_string(sk, "", "OSS Access Key Secret");
DEFINE_string(endpoint, "", "OSS Endpoint");
DEFINE_string(bucket, "", "OSS Bucket Name");
DEFINE_string(region, "", "OSS Region");

using namespace photon::objstore;

class SimpleCredentialsProvider : public CredentialsProvider {
 public:
  SimpleCredentialsProvider(const std::string &accessKeyId,
                            const std::string &accessKeySecret,
                            const std::string &sessionToken = "")
      : m_credentials{accessKeyId, accessKeySecret, sessionToken} {}
  OSSCredentials getCredentials() override { return m_credentials; }

  void setCredentials(const OSSCredentials &cred) override {
    m_credentials = cred;
  }

 private:
  OSSCredentials m_credentials;
};

class OssTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto random_suffix =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    bucket_prefix_ = "test_prefix_" + std::to_string(random_suffix) + "/";

    OssOptions opts_;
    opts_.bucket = FLAGS_bucket;
    opts_.endpoint = FLAGS_endpoint;
    opts_.max_list_ret_cnt = 2;

    // v4 signature with non-empty region, otherwise v1 signature.
    opts_.region = FLAGS_region;

    cp_ = new_simple_credentials_provider(FLAGS_ak, FLAGS_sk);
    client = new_oss_client(opts_, cp_);
    ASSERT_NE(client, nullptr) << "Failed to create OSS client";
  }

  void TearDown() override {
    if (client && !bucket_prefix_.empty()) {
      std::vector<std::string> objects;
      auto cb = [&](const ListObjectsCBParams &cb) {
        objects.emplace_back(std::string(cb.key));
        return 0;
      };
      int ret = client->list_objects(bucket_prefix_, cb, false);
      ASSERT_EQ(ret, 0);

      if (objects.size()) {
        if (rand() % 2) {
          client->delete_objects("", objects);
        } else {
          for (auto &obj : objects) {
            ret = client->delete_object(obj);
            ASSERT_EQ(ret, 0);
          }
        }
        objects.clear();
        ret = client->list_objects(bucket_prefix_, cb, false);
        ASSERT_EQ(ret, 0);
        ASSERT_EQ(objects.size(), 0);
      }
    }
    if (client) delete client;
    if (cp_) delete cp_;
  }

  estring get_real_test_path(std::string_view suffix) {
    estring path;
    if (suffix.size() && suffix.front() == '/') suffix.remove_prefix(1);
    path.appends(bucket_prefix_, suffix);
    return path;
  }

  OssClient *client = nullptr;

 private:
  CredentialsProvider *cp_ = nullptr;
  std::string bucket_prefix_;
};

TEST_F(OssTest, list_objects) {
  std::vector<std::string> test_objects = {
      // there are in lexicographic order
      get_real_test_path("list-test/obj1.txt"),
      get_real_test_path("list-test/obj2.txt"),
      get_real_test_path("list-test/obj3.txt"),
      get_real_test_path("list-test/obj4.txt"),
      get_real_test_path("list-test/obj5.txt"),
      get_real_test_path("list-test/ocomprefix/obj6.txt")};
  auto com_prefix_key = get_real_test_path("list-test/ocomprefix/");

  char buf[3] = {3};
  iovec iov{buf, 3};
  for (const auto &key : test_objects) {
    int ret = client->put_object(key, &iov, 1);
    ASSERT_EQ(ret, 3);
  }

  struct ListObjectsInfo {
    estring key;
    estring etag;
    size_t size;
    time_t mtime;
    bool is_com_prefix;
    ListObjectsInfo(estring_view k, estring_view e, size_t s, time_t m,
                    bool is_com)
        : key(k), etag(e), size(s), mtime(m), is_com_prefix(is_com) {}
  };
  std::vector<ListObjectsInfo> listed_objects;
  auto list_callback = [&](const ListObjectsCBParams &params) {
    listed_objects.emplace_back(params.key, params.etag, params.size,
                                params.mtime, params.is_com_prefix);
    return 0;
  };

  auto test_list_func = [&](auto list_fun) {
    listed_objects.clear();
    int ret =
        list_fun(get_real_test_path("list-test/"), list_callback, false, 2);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(listed_objects.size(), test_objects.size());

    for (size_t i = 0; i < listed_objects.size(); ++i) {
      EXPECT_EQ(listed_objects[i].key, test_objects[i]);
      EXPECT_FALSE(listed_objects[i].etag.empty());
      EXPECT_FALSE(listed_objects[i].is_com_prefix);
      EXPECT_EQ(listed_objects[i].size, 3);
      EXPECT_NE(listed_objects[i].mtime, 0);
    }

    listed_objects.clear();
    ret = list_fun(get_real_test_path("list-test/"), list_callback, true, 2);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(listed_objects.size(), test_objects.size());

    for (size_t i = 0; i < listed_objects.size() - 1; ++i) {
      EXPECT_EQ(listed_objects[i].key, test_objects[i]);
      EXPECT_FALSE(listed_objects[i].etag.empty());
      EXPECT_FALSE(listed_objects[i].is_com_prefix);
      EXPECT_EQ(listed_objects[i].size, 3);
      EXPECT_NE(listed_objects[i].mtime, 0);
    }

    EXPECT_TRUE(listed_objects.back().is_com_prefix);
    EXPECT_EQ(listed_objects.back().key, com_prefix_key);
  };

  test_list_func([&](std::string_view prefix, ListObjectsCallback cb,
                     bool delimiter, int max_keys) {
    return client->list_objects(prefix, cb, delimiter, max_keys);
  });
  test_list_func([&](std::string_view prefix, ListObjectsCallback cb,
                     bool delimiter, int max_keys) {
    return client->list_objects_v1(prefix, cb, delimiter, max_keys);
  });
  test_list_func([&](std::string_view prefix, ListObjectsCallback cb,
                     bool delimiter, int max_keys) {
    return client->list_objects_v2(prefix, cb, delimiter, max_keys);
  });

  auto test_one_shot = [&](auto list_fun) {
    listed_objects.clear();
    std::string marker;
    int ret = list_fun(get_real_test_path("list-test/"), list_callback, false,
                       2, &marker);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(listed_objects.size(), 2);

    ret = list_fun(get_real_test_path("list-test/"), list_callback, false, 2,
                   &marker);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(listed_objects.size(), 4);

    for (size_t i = 0; i < listed_objects.size(); ++i) {
      EXPECT_EQ(listed_objects[i].key, test_objects[i]);
    }
  };
  test_one_shot([&](std::string_view prefix, ListObjectsCallback cb,
                    bool delimiter, int max_keys, std::string *marker) {
    return client->list_objects(prefix, cb, delimiter, max_keys, marker);
  });
  test_one_shot([&](std::string_view prefix, ListObjectsCallback cb,
                    bool delimiter, int max_keys, std::string *marker) {
    return client->list_objects_v1(prefix, cb, delimiter, max_keys, marker);
  });
  test_one_shot([&](std::string_view prefix, ListObjectsCallback cb,
                    bool delimiter, int max_keys, std::string *marker) {
    return client->list_objects_v2(prefix, cb, delimiter, max_keys, marker);
  });

  int ret = client->check_prefix_with_list_objects(get_real_test_path("list-test/"), true);
  EXPECT_EQ(ret, 0);
  ret = client->check_prefix_with_list_objects(get_real_test_path("list-test/"), false);
  EXPECT_EQ(ret, 0);
}

TEST_F(OssTest, put_and_get_meta) {
  auto path = get_real_test_path("object_meta/testfile.html");
  size_t file_size = 1025;
  char buf[file_size] = {3};
  iovec iov{buf, file_size};
  int ret = client->put_object(path, &iov, 1);
  ASSERT_EQ(ret, file_size) << "Failed to upload object for metadata test";

  // todo: add crc64 comparison
  ObjectHeaderMeta hmeta;
  ret = client->head_object(path, hmeta);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(hmeta.size, file_size);
  EXPECT_TRUE(!hmeta.etag.empty());
  EXPECT_NE(hmeta.mtime, 0);
  EXPECT_TRUE(hmeta.crc64.first);
  EXPECT_TRUE(!hmeta.storage_class.empty());
  EXPECT_TRUE(!hmeta.type.empty());

  ObjectMeta meta;
  ret = client->get_object_meta(path, meta);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(meta.size, file_size);
  EXPECT_TRUE(!meta.etag.empty());
  EXPECT_EQ(meta.mtime, hmeta.mtime);
}

TEST_F(OssTest, copy_and_rename) {
  auto src = get_real_test_path("copy_and_rename_object/src.mp3");
  auto dst = get_real_test_path("copy_and_rename_object/dst.mp4");

  iovec iov{nullptr, 0};
  int ret = client->put_object(src, &iov, 1);
  ASSERT_EQ(ret, 0);
  ret = client->copy_object(src, dst, false, true);
  ASSERT_EQ(ret, 0);
  ret = client->copy_object(src, dst, false, true);
  ASSERT_EQ(ret, -1);  // overwrite is not allowed
  ret = client->rename_object(src, dst);
  ASSERT_EQ(ret, 0);
}

TEST_F(OssTest, append_and_get) {
  auto path = get_real_test_path("append_and_get/testfile");

  char buf[2] = {'1', '2'};
  iovec iov{buf, 2};
  int ret = client->append_object(path, &iov, 1, 0);
  ASSERT_EQ(ret, 2);
  ret = client->append_object(path, &iov, 1, 2);
  ASSERT_EQ(ret, 2);

  char buf2[4];
  iovec iov2{buf2, 4};
  ret = client->get_object_range(path, &iov2, 1, 0);
  ASSERT_EQ(ret, 4);
  ASSERT_EQ(std::string(buf2, 4), "1212");
}

TEST_F(OssTest, multipart) {
  auto path = get_real_test_path("multipart_upload/testfile");

  void *context = nullptr;
  int ret = client->init_multipart_upload(path, &context);
  ASSERT_EQ(ret, 0);
  size_t buf_size = 1048577;
  for (int i = 0; i < 5; i++) {
    char buf[buf_size] = {(char)i};
    iovec iov{buf, buf_size};
    ret = client->upload_part(context, &iov, 1, i + 1);
    ASSERT_EQ(ret, buf_size);
  }
  ret = client->complete_multipart_upload(context, nullptr);
  ASSERT_EQ(ret, 0);

  ObjectMeta meta;
  ret = client->get_object_meta(path, meta);
  ASSERT_EQ(ret, 0);
  ASSERT_EQ(meta.size, buf_size * 5);

  auto path4copy = get_real_test_path("multipart_upload/testfile_copy");
  auto ctx = client->init_multipart_upload(path4copy);
  if (!ctx) {
    ASSERT_TRUE(false);
  }
  for (int i = 0; i < 5; i++) {
    ret = ctx.copy(i * buf_size, buf_size, i + 1, path);
    ASSERT_EQ(ret, 0);
  }
  ret = ctx.complete(nullptr);
  ASSERT_EQ(ret, 0);

  ObjectMeta meta_copyed;
  ret = client->get_object_meta(path4copy, meta_copyed);
  ASSERT_EQ(ret, 0);
  EXPECT_EQ(meta.size, meta_copyed.size);
  EXPECT_EQ(meta.etag, meta_copyed.etag);

  auto path4abort = get_real_test_path("multipart_upload/testfile_abort");
  ctx = client->init_multipart_upload(path4abort);
  if (!ctx) {
    ASSERT_TRUE(false);
  }
  for (int i = 0; i < 5; i++) {
    ret = ctx.copy(i * buf_size, buf_size, i + 1, path);
    ASSERT_EQ(ret, 0);
  }
  ret = ctx.abort();
  ASSERT_EQ(ret, 0);
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_ak.empty() || FLAGS_sk.empty() || FLAGS_endpoint.empty() ||
      FLAGS_bucket.empty()) {
    std::cerr << "Error: All parameters (ak, sk, endpoint, bucket) are "
                 "required! region is optional."
              << std::endl;
    std::cerr
        << "Usage: " << argv[0]
        << " --ak=<access_key_id> --sk=<access_key_secret> "
        << "--endpoint=<endpoint> --bucket=<bucket_name> --region=<region>"
        << std::endl;
    gflags::ShowUsageWithFlags(argv[0]);
    return 1;
  }
  photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
  DEFER(photon::fini());

  return RUN_ALL_TESTS();
}