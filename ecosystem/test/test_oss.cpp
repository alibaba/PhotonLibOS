#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <photon/common/alog.h>
#include <photon/common/checksum/crc64ecma.h>
#include <photon/common/estring.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>

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

class BasicAuthOssTest : public ::testing::Test {
 protected:
  Client* client = nullptr;
  virtual void CreateOssClient(const ClientOptions& opts) {
    auto auth = new_basic_oss_authenticator({FLAGS_ak, FLAGS_sk, ""});
    client = new_oss_client(opts, auth);
    ASSERT_NE(client, nullptr) << "Failed to create OSS client";
  }
  void SetUp() override {
    auto random_suffix =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    bucket_prefix_ = "test_prefix_" + std::to_string(random_suffix) + "/";

    ClientOptions opts_;
    opts_.bucket = FLAGS_bucket;
    opts_.endpoint = FLAGS_endpoint;
    opts_.max_list_ret_cnt = 2;

    // v4 signature with non-empty region, otherwise v1 signature.
    opts_.region = FLAGS_region;

    CreateOssClient(opts_);
  }

  void TearDown() override {
    if (client && !bucket_prefix_.empty()) {
      std::vector<std::string> objects;
      auto cb = [&](const ListObjectsCBParameters& cb) {
        objects.emplace_back(std::string(cb.key));
        return 0;
      };
      int ret = client->list_objects(bucket_prefix_, cb);
      ASSERT_EQ(ret, 0);

      if (objects.size()) {
        if (rand() % 2) {
          client->delete_objects(
              std::vector<std::string_view>(objects.begin(), objects.end()));
        } else {
          for (auto& obj : objects) {
            ret = client->delete_object(obj);
            ASSERT_EQ(ret, 0);
          }
        }
        objects.clear();
        ret = client->list_objects(bucket_prefix_, cb);
        ASSERT_EQ(ret, 0);
        ASSERT_EQ(objects.size(), 0);
      }
    }
    if (client) delete client;
  }

  estring get_real_test_path(std::string_view suffix) {
    estring path;
    if (suffix.size() && suffix.front() == '/') suffix.remove_prefix(1);
    path.appends(bucket_prefix_, suffix);
    return path;
  }

  void list_objects();
  void put_and_get_meta();
  void copy_and_rename();
  void append_and_get();
  void multipart();
  void symlink();

 private:
  std::string bucket_prefix_;
};

class CachedAuthOssTest : public BasicAuthOssTest {
 protected:
  void CreateOssClient(const ClientOptions& opts) override {
    auto auth = new_basic_oss_authenticator({FLAGS_ak, FLAGS_sk, ""});
    auth = new_cached_oss_authenticator(auth, 3 /*ttl*/);
    client = new_oss_client(opts, auth);
    ASSERT_NE(client, nullptr) << "Failed to create OSS client";
  }

  void repeatedly_get();
};

class RandInvalidateCacheAuthenticator : public Authenticator {
  Authenticator* auth_ = nullptr;

 public:
  RandInvalidateCacheAuthenticator(Authenticator* auth) : auth_(auth) {}
  ~RandInvalidateCacheAuthenticator() { delete auth_; }
  virtual int sign(photon::net::http::Headers& headers,
                   const SignParameters& params) override {
    auto new_params = params;
    new_params.invalidate_cache = rand() % 2;
    return auth_->sign(headers, new_params);
  }

  virtual void set_credentials(CredentialParameters&& credentials) override {
    auth_->set_credentials(std::move(credentials));
  }
};

class CustomCachedAuthOssTest : public CachedAuthOssTest {
 protected:
  void CreateOssClient(const ClientOptions& opts) override {
    auto auth = new_basic_oss_authenticator({FLAGS_ak, FLAGS_sk, ""});
    auth = new_cached_oss_authenticator(auth, 3 /*ttl*/);
    client = new_oss_client(opts, new RandInvalidateCacheAuthenticator(auth));
    ASSERT_NE(client, nullptr) << "Failed to create OSS client";
  }
};

void BasicAuthOssTest::list_objects() {
  std::vector<std::string> test_objects = {
      // they are in lexicographic order
      get_real_test_path("list-test/obj1.txt"),
      get_real_test_path("list-test/obj2.txt"),
      get_real_test_path("list-test/obj3.txt"),
      get_real_test_path("list-test/obj4.txt"),
      get_real_test_path("list-test/obj5.txt"),
      get_real_test_path("list-test/ocomprefix/obj6.txt")};
  auto com_prefix_key = get_real_test_path("list-test/ocomprefix/");

  char buf[3] = {3};
  iovec iov{buf, 3};
  auto crc64 = crc64ecma(buf, 3, 0);
  for (const auto& key : test_objects) {
    int ret = client->put_object(key, &iov, 1, &crc64);
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
  auto list_callback = [&](const ListObjectsCBParameters& params) {
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
    ListObjectsParameters params;
    params.slash_delimiter = delimiter;
    params.max_keys = max_keys;
    params.ver = 2;
    return client->list_objects(prefix, cb, params);
  });
  test_list_func([&](std::string_view prefix, ListObjectsCallback cb,
                     bool delimiter, int max_keys) {
    ListObjectsParameters params;
    params.slash_delimiter = delimiter;
    params.max_keys = max_keys;
    params.ver = 1;
    return client->list_objects(prefix, cb, params);
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
                    bool delimiter, int max_keys, std::string* marker) {
    ListObjectsParameters params;
    params.slash_delimiter = delimiter;
    params.max_keys = max_keys;
    params.ver = 1;
    return client->list_objects(prefix, cb, params, marker);
  });
  test_one_shot([&](std::string_view prefix, ListObjectsCallback cb,
                    bool delimiter, int max_keys, std::string* marker) {
    ListObjectsParameters params;
    params.slash_delimiter = delimiter;
    params.max_keys = max_keys;
    params.ver = 2;
    return client->list_objects(prefix, cb, params, marker);
  });
}

void BasicAuthOssTest::put_and_get_meta() {
  auto path = get_real_test_path("object_meta/testfile.unknown_suffix");
  const size_t file_size = 1025;
  char buf[file_size] = {3};
  auto crc64 = crc64ecma(buf, file_size, 0);
  iovec iov{buf, file_size};
  int ret = client->put_object(path, &iov, 1, &crc64);
  ASSERT_EQ(ret, file_size) << "Failed to upload object for metadata test";
  crc64 += 1;
  ret = client->put_object(path, &iov, 1, &crc64);
  ASSERT_EQ(ret, -1) << "crc64 check did not work";

  // todo: add crc64 comparison
  ObjectHeaderMeta hmeta;
  ret = client->head_object(path, hmeta);
  ASSERT_EQ(ret, 0);
  EXPECT_TRUE(hmeta.has_etag());
  EXPECT_TRUE(hmeta.has_size());
  EXPECT_TRUE(hmeta.has_mtime());
  EXPECT_TRUE(hmeta.has_crc64());
  EXPECT_TRUE(hmeta.has_storage_class());
  EXPECT_TRUE(hmeta.has_type());
  EXPECT_EQ(hmeta.size, file_size);
  EXPECT_TRUE(!hmeta.etag.empty());
  EXPECT_NE(hmeta.mtime, 0);
  EXPECT_TRUE(hmeta.has_crc64());
  EXPECT_TRUE(!hmeta.storage_class.empty());
  EXPECT_TRUE(!hmeta.type.empty());

  ObjectMeta meta;
  ret = client->get_object_meta(path, meta);
  ASSERT_EQ(ret, 0);
  EXPECT_TRUE(meta.has_etag());
  EXPECT_TRUE(meta.has_size());
  EXPECT_TRUE(meta.has_mtime());
  EXPECT_EQ(meta.size, file_size);
  EXPECT_TRUE(!meta.etag.empty());
  EXPECT_EQ(meta.mtime, hmeta.mtime);

  ObjectHeaderMeta hmeta2;
  char buf2[2];
  iovec iov2{buf2, 2};
  ret = client->get_object_range(path, &iov2, 1, file_size - 1, &hmeta2);
  ASSERT_EQ(ret, 1);
  /// From testing, get obj range will return all the following fileds.
  EXPECT_EQ(hmeta2.flags, hmeta.flags);
  EXPECT_EQ(hmeta2.size, hmeta.size);
  EXPECT_EQ(hmeta2.crc64, hmeta.crc64);
  EXPECT_EQ(hmeta2.mtime, hmeta.mtime);
  EXPECT_EQ(hmeta2.storage_class, hmeta.storage_class);
  EXPECT_EQ(hmeta2.crc64, hmeta.crc64);
  EXPECT_EQ(hmeta2.etag, hmeta.etag);

  auto expected_crc64 = hmeta2.crc64;
  ret = client->put_object(path, &iov, 1, &expected_crc64);
  ASSERT_EQ(ret, file_size);
  expected_crc64 = hmeta2.crc64 + 1;
  ret = client->put_object(path, &iov, 1, &expected_crc64);
  ASSERT_EQ(ret, -1);
}

void BasicAuthOssTest::copy_and_rename() {
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

void BasicAuthOssTest::append_and_get() {
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

void BasicAuthOssTest::multipart() {
  auto path = get_real_test_path("multipart_upload/testfile");

  void* context = nullptr;
  int ret = client->init_multipart_upload(path, &context);
  ASSERT_EQ(ret, 0);
  const size_t buf_size = 1048577;
  uint64_t crc64 = 0;
  for (int i = 0; i < 5; i++) {
    char buf[buf_size] = {(char)i};
    crc64 = crc64ecma(buf, buf_size, crc64);
    iovec iov{buf, buf_size};

    auto part_crc64 = crc64ecma(buf, buf_size, 0);
    ret = client->upload_part(context, &iov, 1, i + 1, &part_crc64);
    ASSERT_EQ(ret, buf_size);
  }
  ret = client->complete_multipart_upload(context, &crc64);
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

void BasicAuthOssTest::symlink() {
  auto src = get_real_test_path("symlink/source");
  auto target = "symlink/target";

  int r = client->put_symlink(src, target);
  ASSERT_EQ(r, 0);

  std::string oss_target;
  r = client->get_symlink(src, oss_target);
  ASSERT_EQ(r, 0);

  EXPECT_EQ(oss_target, target);

  std::vector<std::string> objects;
  auto cb = [&](const ListObjectsCBParameters& cb) {
    objects.emplace_back(cb.key);
    if (cb.type == "Symlink") return 0;
    return -1;
  };
  int ret = client->list_objects(get_real_test_path("symlink/"), cb);
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(objects.size(), 1);
  EXPECT_EQ(objects[0], src);
}

void CachedAuthOssTest::repeatedly_get() {
  std::vector<std::string> paths;
  const size_t file_size = 1025;
  char buf[file_size] = {2};
  iovec iov{buf, file_size};
  auto crc64 = crc64ecma(buf, file_size, 0);
  for (int i = 0; i < 10; i++) {
    auto path =
        get_real_test_path("rput_and_get/testfile.suffix" + std::to_string(i));
    int ret = client->put_object(path, &iov, 1, &crc64);
    ASSERT_EQ(ret, file_size) << "Failed to upload object for metadata test";
    paths.push_back(path);
  }

  for (size_t i = 0; i < 10; i++) {
    for (size_t j = 0; j < paths.size(); j++) {
      iov.iov_len = i + 1;
      auto ret = client->get_object_range(paths[j], &iov, 1, i, nullptr);
      EXPECT_EQ(ret, i + 1);
    }
  }

  client->set_credentials({"invalid creds"});
  LOG_INFO("creds invalidated");
  iov.iov_len = 1;
  auto ret = client->get_object_range(paths[0], &iov, 1, 0, nullptr);
  EXPECT_EQ(ret, -1);
  client->set_credentials({FLAGS_ak, FLAGS_sk});
  ret = client->get_object_range(paths[0], &iov, 1, 0, nullptr);
  EXPECT_EQ(ret, 1);
}

TEST_F(BasicAuthOssTest, list_objects) { list_objects(); }
TEST_F(BasicAuthOssTest, multipart) { multipart(); }
TEST_F(BasicAuthOssTest, append_and_get) { append_and_get(); }
TEST_F(BasicAuthOssTest, put_and_get_meta) { put_and_get_meta(); }
TEST_F(BasicAuthOssTest, copy_and_rename) { copy_and_rename(); }
TEST_F(BasicAuthOssTest, symlink) { symlink(); }
TEST_F(CachedAuthOssTest, listobjects) { list_objects(); }
TEST_F(CachedAuthOssTest, multipart) { multipart(); }
TEST_F(CachedAuthOssTest, append_and_get) { append_and_get(); }
TEST_F(CachedAuthOssTest, put_and_get_meta) { put_and_get_meta(); }
TEST_F(CachedAuthOssTest, copy_and_rename) { copy_and_rename(); }
TEST_F(CachedAuthOssTest, repeatedly_get) { repeatedly_get(); }
TEST_F(CachedAuthOssTest, symlink) { symlink(); }
TEST_F(CustomCachedAuthOssTest, repeatedly_get) { repeatedly_get(); }

int main(int argc, char* argv[]) {
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
