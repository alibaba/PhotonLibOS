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


#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <algorithm>

#include <photon/common/utility.h>
#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/fs/localfs.h>
#include <photon/fs/aligned-file.h>
#include <photon/thread/thread.h>
#include <photon/common/io-alloc.h>
#include <photon/fs/cache/cache.h>

#include "../full_file_cache/cache_pool.h"
#include "random_generator.h"

namespace photon {
namespace fs {


// Cleanup and recreate the test dir
inline void SetupTestDir(const std::string& dir) {
  if (::access(dir.c_str(), F_OK) != 0) {
    ::mkdir(dir.c_str(), 0755);
  }
  std::string cmd = std::string("rm -r ") + dir;
  EXPECT_NE(-1, system(cmd.c_str()));
  cmd = std::string("mkdir -p ") + dir;
  EXPECT_NE(-1, system(cmd.c_str()));
}

void commonTest(bool cacheIsFull, bool enableDirControl, bool dirFull) {
  std::string prefix = "";
  const size_t dirQuota = 32ul * 1024 * 1024;
  const uint64_t refillSize = 1024 * 1024;
  if (enableDirControl) {
    prefix = "/John/bucket/";
  }

  std::string root("/tmp/ease/cache/cache_test/");
  SetupTestDir(root);

  std::string subDir = prefix + "dir/dir/";
  SetupTestDir(root + subDir);
  EXPECT_NE(-1, std::system(std::string("touch " + root + subDir + "testFile").c_str()));

  struct stat st;
  auto ok = ::stat(std::string(root + subDir + "testFile").c_str(), &st);
  EXPECT_EQ(0, ok);

  std::string srcRoot("/tmp/ease/cache/src_test/");
  SetupTestDir(srcRoot);
  auto srcFs = new_localfs_adaptor(srcRoot.c_str(), ioengine_psync);

  auto mediaFs = new_localfs_adaptor(root.c_str(), ioengine_libaio);
  auto alignFs = new_aligned_fs_adaptor(mediaFs, 4 * 1024, true, true);
  auto cacheAllocator = new AlignedAlloc(4 * 1024);
  auto roCachedFs = new_full_file_cached_fs(srcFs, alignFs, refillSize,
      cacheIsFull ? 0 : 512, 1000 * 1000 * 1, 128ul * 1024 * 1024, cacheAllocator, enableDirControl ? 2 : 0);
  auto cachePool = roCachedFs->get_pool();

  if (dirFull) {
    cachePool->set_quota(prefix, dirQuota);
  }
  SetupTestDir(srcRoot + prefix + "testDir");
  auto srcFile = srcFs->open(std::string(prefix + "/testDir/file_1").c_str(),
   O_RDWR|O_CREAT|O_TRUNC, 0644);

  UniformCharRandomGen gen(0, 255);
  off_t offset = 0;
  uint32_t kPageSize = 4 * 1024;
  uint32_t kFileSize = kPageSize * 16384; // 64MB
  uint32_t kPageCount = kFileSize / kPageSize;
  for (uint32_t i = 0; i < kPageCount; ++i) {
    std::vector<unsigned char> data;
    for (uint32_t j = 0; j < kPageSize; ++j) {
      data.push_back(gen.next());
    }
    srcFile->pwrite(data.data(), data.size(), offset);
    offset += kPageSize;
  }

  //  write some unaligned
  off_t lastOffset = offset;
  off_t unAlignedLen = 750;
  {
    std::vector<unsigned char> data;
    for (uint32_t j = 0; j < kPageSize; ++j) {
      data.push_back(gen.next());
    }
    srcFile->pwrite(data.data(), unAlignedLen, offset);
  }

  auto cachedFile = static_cast<ICachedFile*>(roCachedFs->open(
    std::string(prefix + "/testDir/file_1").c_str(), 0, 0644));

  //  test unaligned block
  {
    void* buf = malloc(kPageSize);
    auto ret = cachedFile->pread(buf, kPageSize, lastOffset);

    std::vector<unsigned char> src;
    src.reserve(kPageSize);
    auto retSrc = srcFile->pread(src.data(), kPageSize, lastOffset);

    EXPECT_EQ(0, std::memcmp(buf, src.data(), unAlignedLen));
    EXPECT_EQ(unAlignedLen, retSrc);
    EXPECT_EQ(unAlignedLen, ret);

    LOG_INFO("read again");

    // read again
    ret = cachedFile->pread(buf, kPageSize, lastOffset);
    EXPECT_EQ(unAlignedLen, ret);

    free(buf);
  }

  //  test aligned and unaligned block
  {
    void* buf = malloc(kPageSize * 4);
    auto ret = cachedFile->pread(buf, kPageSize * 4, lastOffset - 2 * kPageSize);

    std::vector<unsigned char> src;
    src.reserve(kPageSize * 4);
    auto retSrc = srcFile->pread(src.data(), kPageSize * 4, lastOffset - 2 * kPageSize);

    EXPECT_EQ(0, std::memcmp(buf, src.data(), 2 * kPageSize + unAlignedLen));
    EXPECT_EQ(2 * kPageSize + unAlignedLen, retSrc);
    EXPECT_EQ(2 * kPageSize + unAlignedLen, ret);

    LOG_INFO("read again");

    // read again
    ret = cachedFile->pread(buf, kPageSize * 4, lastOffset - 2 * kPageSize);
    EXPECT_EQ(2 * kPageSize + unAlignedLen, ret);

    free(buf);
  }

  std::vector<char> readBuf;
  readBuf.reserve(kPageSize);
  std::vector<char> readSrcBuf;
  readSrcBuf.reserve(kPageSize);
  for (int i = 0; i != 5; ++i) {
    EXPECT_EQ(kPageSize, cachedFile->read(readBuf.data(), kPageSize));
    srcFile->read(readSrcBuf.data(), kPageSize);
    EXPECT_EQ(0, std::memcmp(readBuf.data(), readSrcBuf.data(), kPageSize));
  }

  if (enableDirControl && !cacheIsFull) {
    CacheStat cstat = {};
    EXPECT_EQ(0, cachePool->stat(&cstat, std::string(prefix + "/testDir/file_1").c_str()));
    EXPECT_EQ(kFileSize / refillSize, cstat.total_size);
    cstat = {};
    EXPECT_EQ(0, cachedFile->get_store()->stat(&cstat));
    EXPECT_EQ(kFileSize / refillSize, cstat.total_size);
  }

  // test refill(3)
  if (!cacheIsFull) {
    auto inSrcFile = cachedFile->get_source();
    cachedFile->set_source(nullptr);
    struct stat stat;
    inSrcFile->fstat(&stat);
    cachedFile->ftruncate(stat.st_size);
    void* buf = malloc(kPageSize * 3);
    DEFER(free(buf));
    std::vector<char> src;
    src.reserve(kPageSize * 3);
    EXPECT_EQ(kPageSize, srcFile->pread(src.data(), kPageSize, 0));
    memcpy(buf, src.data(), kPageSize);

    EXPECT_EQ(kPageSize, cachedFile->refill(buf, kPageSize, 0));

    memset(buf, 0, kPageSize);
    EXPECT_EQ(kPageSize, cachedFile->pread(buf, kPageSize, 0));
    EXPECT_EQ(0, memcmp(buf, src.data(), kPageSize));

    struct stat st1;
    ::stat(std::string(root + prefix + "/testDir/file_1").c_str(), &st1);
    EXPECT_EQ(0, cachedFile->evict(0, kPageSize));
    struct stat st2;
    ::stat(std::string(root + prefix + "/testDir/file_1").c_str(), &st2);
    EXPECT_EQ(kPageSize, st1.st_blocks * 512 - st2.st_blocks * 512);

    // test refill last block
    src.clear();
    EXPECT_EQ(kPageSize + unAlignedLen, srcFile->pread(src.data(), kPageSize * 3, lastOffset - kPageSize));
    memcpy(buf, src.data(), kPageSize * 3);
    EXPECT_EQ(kPageSize + unAlignedLen, cachedFile->refill(buf, kPageSize * 3, lastOffset - kPageSize));
    memset(buf, 0, kPageSize * 3);
    EXPECT_EQ(kPageSize + unAlignedLen, cachedFile->pread(buf, kPageSize * 3, lastOffset - kPageSize));
    EXPECT_EQ(0, memcmp(buf, src.data(), kPageSize + unAlignedLen));

    cachedFile->set_source(inSrcFile);
  }

  //  test refill(2)
  if (!cacheIsFull) {
    auto inSrcFile = cachedFile->get_source();

    void* buf = malloc(kPageSize * 2);
    DEFER(free(buf));
    EXPECT_EQ(0, cachedFile->refill(kPageSize, 2 * kPageSize));

    cachedFile->set_source(nullptr);
    EXPECT_EQ(2 * kPageSize, cachedFile->pread(buf, 2 * kPageSize, kPageSize));
    std::vector<char> src;
    src.reserve(kPageSize * 2);
    EXPECT_EQ(kPageSize * 2, srcFile->pread(src.data(), 2 * kPageSize, kPageSize));
    EXPECT_EQ(0, memcmp(buf, src.data(), 2 * kPageSize));
    cachedFile->set_source(inSrcFile);

    // prefetch more than 16MB
    EXPECT_EQ(0, cachedFile->fadvise(234, 5000 * kPageSize, POSIX_FADV_WILLNEED));
    // prefetch tail
    EXPECT_EQ(0, cachedFile->fadvise(lastOffset - kPageSize, 5000 * kPageSize, POSIX_FADV_WILLNEED));
  }

  if (dirFull) {
    CacheStat cstat = {};
    EXPECT_EQ(0, cachePool->stat(&cstat, prefix));
    EXPECT_EQ(dirQuota / refillSize, cstat.total_size);
  }

  // test aligned section
  UniformInt32RandomGen genOffset(0, (kPageCount + 1) * kPageSize);
  UniformInt32RandomGen genSize(0, 8 * kPageSize);
  struct stat srcSt = {};
  srcFile->fstat(&srcSt);
  for (int i = 0; i != 10000; ++i) {
    auto tmpOffset = genOffset.next();
    auto size = genSize.next();

    if (tmpOffset >= srcSt.st_size) {
      size = 0;
    } else {
      size = tmpOffset + size > srcSt.st_size ? srcSt.st_size - tmpOffset : size;
    }
    void* buf = malloc(size);
    auto ret = cachedFile->pread(buf, size, tmpOffset);

    std::vector<unsigned char> src;
    src.reserve(size);
    auto retSrc = srcFile->pread(src.data(), size, tmpOffset);

    EXPECT_EQ(0, std::memcmp(buf, src.data(), size));
    EXPECT_EQ(size, retSrc);
    EXPECT_EQ(size, ret);
    free(buf);

    if (9900 == i && dirFull) {
      cachedFile->get_store()->set_quota(0);
    }
  }
  srcFile->close();

  photon::thread_usleep(1000 * 1000ull);
  ok = ::stat(std::string(root + subDir + "testFile").c_str(), &st);
  EXPECT_EQ(cacheIsFull || dirFull ? -1 : 0, ok);

  if (enableDirControl) {
    auto ret = cachePool->evict(std::string(prefix + "/testDir").c_str());
    EXPECT_EQ(0, ret);
  }

  delete cachedFile;

  //  test smaller file
  {
    auto smallFile = srcFs->open(std::string(prefix + "/testDir/small").c_str(),
     O_RDWR|O_CREAT|O_TRUNC, 0644);
    DEFER(delete smallFile);
    int smallSize = 102;
    std::vector<char> smallData;
    for (int i = 0; i != smallSize; ++i) {
      smallData.push_back(gen.next());
    }
    EXPECT_EQ(smallSize, smallFile->pwrite(smallData.data(), smallData.size(), 0));

    auto smallCache = static_cast<ICachedFile*>(roCachedFs->open(
      std::string(prefix + "/testDir/small").c_str(), 0, 0644));
    DEFER(delete smallCache);

    void* sBuffer = malloc(kPageSize);
    DEFER(free(sBuffer));
    EXPECT_EQ(smallSize, smallCache->pread(sBuffer, kPageSize, 0));
    EXPECT_EQ(0, std::memcmp(sBuffer, smallData.data(), smallSize));

    memset(sBuffer, 0, kPageSize);
    EXPECT_EQ(smallSize, smallCache->pread(sBuffer, kPageSize, 0));
    EXPECT_EQ(0, std::memcmp(sBuffer, smallData.data(), smallSize));

    smallFile->close();
  }

  //  test refill
  {
    auto refillFile = srcFs->open(std::string(prefix + "/testDir/refill").c_str(),
     O_RDWR|O_CREAT|O_TRUNC, 0644);
    DEFER(delete refillFile);
    int refillSize = 4097;
    std::vector<char> refillData;
    for (int i = 0; i != refillSize; ++i) {
      refillData.push_back(gen.next());
    }
    EXPECT_EQ(refillSize, refillFile->pwrite(refillData.data(), refillData.size(), 0));

    auto refillCache = static_cast<ICachedFile*>(roCachedFs->open(
      std::string(prefix + "/testDir/refill").c_str(), 0, 0644));
    DEFER(delete refillCache);

    void* sBuffer = malloc(kPageSize * 2);
    DEFER(free(sBuffer));
    memset(sBuffer, 0, kPageSize * 2);
    EXPECT_EQ(kPageSize, refillCache->pread(sBuffer, kPageSize, 0));
    EXPECT_EQ(0, std::memcmp(sBuffer, refillData.data(), kPageSize));

    memset(sBuffer, 0, kPageSize * 2);
    EXPECT_EQ(refillSize, refillCache->pread(sBuffer, kPageSize * 2, 0));
    EXPECT_EQ(0, std::memcmp(sBuffer, refillData.data(), refillSize));

    refillFile->close();
  }

  delete srcFs;
  delete roCachedFs;
}

TEST(RoCachedFs, Basic) {
  commonTest(false, false, false);
}

TEST(RoCachedFs, BasicCacheFull) {
  commonTest(true, false, false);
}

// TEST(RoCachedFs, BasicWithDirControl) {
//   commonTest(false, true, false);
// }

// TEST(RoCachedFs, BasicCacheFullWithDirControl) {
//   commonTest(true, true, false);
// }

TEST(RoCachedFs, CacheWithOutSrcFile) {
  std::string root("/tmp/ease/cache/cache_test_no_src/");
  SetupTestDir(root);

  auto mediaFs = new_localfs_adaptor(root.c_str(), ioengine_libaio);
  auto alignFs = new_aligned_fs_adaptor(mediaFs, 4 * 1024, true, true);
  auto cacheAllocator = new AlignedAlloc(4 * 1024);
  DEFER(delete cacheAllocator);
  auto roCachedFs = new_full_file_cached_fs(nullptr, alignFs, 1024 * 1024,
      512, 1000 * 1000 * 1, 128ul * 1024 * 1024, cacheAllocator, 0);
  DEFER(delete roCachedFs);
  auto cachedFile = static_cast<ICachedFile*>(roCachedFs->open(
      std::string("/testDir/file_1").c_str(), 0, 0644));
  DEFER(delete cachedFile);

  cachedFile->ftruncate(1024 * 1024);
  std::vector<char> buf;
  int len = 8 * 1024;
  buf.reserve(len);
  EXPECT_EQ(len, cachedFile->pwrite(buf.data(), len, 4 * 1024));
  EXPECT_EQ(len / 2, cachedFile->pread(buf.data(), 4 * 1024, 4 * 1024));
  EXPECT_EQ(-1, cachedFile->pread(buf.data(), len, 0));

  auto writeFile = static_cast<ICachedFile*>(roCachedFs->open(
      std::string("/testDir/file_2").c_str(), 0, 0644));
  DEFER(delete writeFile);
  writeFile->ftruncate(1024 * 1024);
  buf.assign(len, 'a');
  EXPECT_EQ(len, writeFile->write(buf.data(), len));
  EXPECT_EQ(len, writeFile->write(buf.data(), len));
  std::vector<char> res;
  res.reserve(len);
  EXPECT_EQ(len, writeFile->pread(res.data(), len, 0));
  EXPECT_EQ(0, std::memcmp(buf.data(), res.data(), len));
  res.assign(len, '0');
  EXPECT_EQ(len, writeFile->pread(res.data(), len, len));
  EXPECT_EQ(0, std::memcmp(buf.data(), res.data(), len));
  EXPECT_EQ(-1, writeFile->pread(res.data(), len, len * 2));
}

TEST(RoCachedFS, xattr) {
  std::string root("/tmp/ease/cache/cache_xattr/");
  SetupTestDir(root);

  auto srcFs = new_localfs_adaptor();
  auto mediaFs = new_localfs_adaptor(root.c_str());
  auto roCachedFs = new_full_file_cached_fs(srcFs, mediaFs, 1024 * 1024, 512, 1000 * 1000 * 1,
                                            128ul * 1024 * 1024, nullptr, 0);
  DEFER(delete roCachedFs);

  std::string path = "/tmp/ease/cache/cache_xattr/filexattr";
  auto xttarFile = srcFs->open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  DEFER(delete xttarFile);
  auto xattrFs = dynamic_cast<IFileSystemXAttr*>(roCachedFs);
  std::string name = "user.testxattr", value = "yes";
  char key[20], val[20];
  auto ret = xattrFs->setxattr(path.c_str(), name.c_str(), value.c_str(), value.size(), 0);
  EXPECT_EQ(0, ret);
  ret = xattrFs->listxattr(path.c_str(), key, 20);
  EXPECT_EQ(0, std::memcmp(key, name.data(), ret));
  ret = xattrFs->getxattr(path.c_str(), key, val, 20);
  EXPECT_EQ((long int)value.size(), ret);
  EXPECT_EQ(0, std::memcmp(val, value.data(), ret));
  ret = xattrFs->removexattr(path.c_str(), key);
  EXPECT_EQ(0, ret);

  auto cachedFile = static_cast<ICachedFile*>(roCachedFs->open(path.c_str(), 0, 0644));
  DEFER(delete cachedFile);
  auto xattrfile = dynamic_cast<IFileXAttr*>(cachedFile);
  ret = xattrfile->fsetxattr(name.c_str(), value.c_str(), value.size(), 0);
  EXPECT_EQ(0, ret);
  ret = xattrfile->flistxattr(key, 20);
  EXPECT_EQ(0, std::memcmp(key, name.data(), ret));
  ret = xattrfile->fgetxattr(key, val, 20);
  EXPECT_EQ((long int)value.size(), ret);
  EXPECT_EQ(0, std::memcmp(val, value.data(), ret));
  ret = xattrfile->fremovexattr(key);
  EXPECT_EQ(0, ret);
}

void* worker(void* arg) {
  auto fs = (ICachedFileSystem*)arg;
  char buffer[1024*1024];
  char buffersrc[1024*1024];
  std::vector<off_t> offset;
  for (auto i = 0; i < 2048; i++) {
    offset.push_back(i * 1024 * 1024);
  }
  auto fd = ::open("/tmp/ease/cache/src_test/huge", O_RDONLY);
  DEFER(::close(fd));
  auto f = fs->open("/huge", O_RDONLY);
  DEFER(delete f);
  for (int i=0;i<4;i++) {
  std::shuffle(offset.begin(), offset.end(), std::mt19937(std::random_device()()));
    for (const auto &x : offset) {
      EXPECT_EQ((ssize_t)(1UL<<20), ::pread(fd, buffersrc, 1024*1024, x));
      f->pread(buffer, 1024*1024, x);
      EXPECT_EQ(0, memcmp(buffer, buffersrc, 1024*1024));
      fs->get_pool()->evict();
      photon::thread_yield();
    }
  }
  return nullptr;
}

TEST(CachedFS, write_while_full) {
  std::string srcRoot("/tmp/ease/cache/src_test/");
  SetupTestDir(srcRoot);
  EXPECT_NE(-1, system("dd if=/dev/urandom of=/tmp/ease/cache/src_test/huge bs=1M count=2048"));

  std::string root("/tmp/ease/cache/cache_test/");
  SetupTestDir(root);
  auto srcFs = new_localfs_adaptor(srcRoot.c_str());
  auto mediaFs = new_localfs_adaptor(root.c_str());
  auto roCachedFs = new_full_file_cached_fs(srcFs, mediaFs, 1024 * 1024, 1, 100 * 1000 * 1,
                                            128ul * 1024 * 1024, nullptr, 0);

  std::vector<photon::join_handle*> jhs;

  for (int i=0;i<2;i++) {
    jhs.emplace_back(photon::thread_enable_join(photon::thread_create(worker, roCachedFs)));
  }
  for (auto &x : jhs) {
    photon::thread_join(x);
  }
  delete roCachedFs;
  delete srcFs;
}

TEST(CachedFS, fn_trans_func) {
  std::string srcRoot("/tmp/ease/cache/src_test/");
  SetupTestDir(srcRoot);
  EXPECT_NE(-1, system("mkdir /tmp/ease/cache/src_test/path_aaa/"));
  EXPECT_NE(-1, system("mkdir /tmp/ease/cache/src_test/path_bbb/"));
  EXPECT_NE(-1, system("dd if=/dev/urandom of=/tmp/ease/cache/src_test/path_aaa/sha256:test bs=1K count=1"));
  EXPECT_NE(-1, system("cp /tmp/ease/cache/src_test/path_aaa/sha256:test /tmp/ease/cache/src_test/path_bbb/sha256:test"));

  std::string root("/tmp/ease/cache/cache_test/");
  SetupTestDir(root);
  auto srcFs = new_localfs_adaptor(srcRoot.c_str());
  DEFER(delete srcFs);
  auto mediaFs = new_localfs_adaptor(root.c_str());
  struct NameTransCB {
    size_t fn_trans_sha256(std::string_view src, char *dest, size_t size) {
      auto p = src.find("/sha256:");
      if (p == std::string_view::npos) {
        return 0;
      }
      size_t len = src.size() - p;
      if (len > size) {
        LOG_WARN("filename length ` exceed `", len, size);
        return 0;
      }
      strcpy(dest, src.data() + p);
      return len;
    }
  }cb;
  auto cachedFs = new_full_file_cached_fs(srcFs, mediaFs, 1024 * 1024, 1, 100 * 1000 * 1,
                                          128ul * 1024 * 1024, nullptr, 0, {&cb, &NameTransCB::fn_trans_sha256});
  DEFER(delete cachedFs);
  char buf1[1024], buf2[1024];
  auto cachedFile1 = static_cast<ICachedFile*>(cachedFs->open("/path_aaa/sha256:test", 0, 0644));
  auto cachedFile2 = static_cast<ICachedFile*>(cachedFs->open("/path_bbb/sha256:test", 0, 0644));
  cachedFile1->read(buf1, 1024);
  auto cFile = mediaFs->open("/sha256:test", 0, 0644);
  DEFER(delete cFile);
  cFile->read(buf2, 1024);
  EXPECT_EQ(0, memcmp(buf1, buf2, 1024));
  auto cs1 = cachedFile1->get_store();
  auto cs2 = cachedFile2->get_store();
  EXPECT_EQ(cs1, cs2);
}

TEST(CachePool, evict_file) {
  std::string root = "/tmp/ease/cache/evict_file_test/";
  SetupTestDir(root);
  auto mediaFs = new_localfs_adaptor(root.c_str(), ioengine_libaio);
  auto alignFs = new_aligned_fs_adaptor(mediaFs, 4 * 1024, true, true);
  auto cacheAllocator = new AlignedAlloc(4 * 1024);
  auto roCachedFs = new_full_file_cached_fs(nullptr, alignFs, 1024 * 1024,
      1, 1000 * 1000 * 1, 128ul * 1024 * 1024, cacheAllocator, 0);
  auto cachePool = roCachedFs->get_pool();
  DEFER({ delete cacheAllocator; delete roCachedFs; });

  auto fileName = "/file_to_evict";
  auto cacheStore = cachePool->open(fileName, O_CREAT | O_RDWR, 0644);
  ASSERT_NE(nullptr, cacheStore);
  
  const size_t bufSize = 1024 * 1024;
  IOVector buffer(*cacheAllocator);
  buffer.push_back(bufSize);

  auto ret = cacheStore->do_pwritev2(buffer.iovec(), buffer.iovcnt(), 0, 0);
  EXPECT_EQ(ret, (ssize_t)bufSize);

  ret = cacheStore->do_preadv2(buffer.iovec(), buffer.iovcnt(), 0, 0);
  EXPECT_EQ(ret, (ssize_t)bufSize);

  struct stat stBefore;
  std::string fullPath = root + fileName;
  EXPECT_EQ(0, ::stat(fullPath.c_str(), &stBefore));
  EXPECT_EQ(stBefore.st_size, (off_t)bufSize);

  EXPECT_EQ(0, cachePool->evict(fileName));

  ret = cacheStore->do_preadv2(buffer.iovec(), buffer.iovcnt(), 0, 0);
  EXPECT_EQ(ret, 0);

  struct stat stAfter;
  EXPECT_EQ(0, ::stat(fullPath.c_str(), &stAfter));
  EXPECT_EQ(stAfter.st_size, 0);

  cacheStore->release();

  cacheStore = cachePool->open(fileName, O_CREAT | O_RDWR, 0644);
  ASSERT_NE(nullptr, cacheStore);

  auto tres = cacheStore->try_preadv2(buffer.iovec(), buffer.iovcnt(), bufSize, 0);
  EXPECT_EQ(tres.refill_offset, (ssize_t)bufSize);
  EXPECT_EQ(tres.refill_size, bufSize);

  ret = cacheStore->do_pwritev2(buffer.iovec(), buffer.iovcnt(), 0, 0);
  EXPECT_EQ(ret, (ssize_t)bufSize);

  ret = cacheStore->do_preadv2(buffer.iovec(), buffer.iovcnt(), 0, 0);
  EXPECT_EQ(ret, (ssize_t)bufSize);

  cacheStore->release();
}

TEST(CachePool, random_evict_file) {
  std::string root = "/tmp/ease/cache/random_evict_file_test/";
  SetupTestDir(root);
  auto mediaFs = new_localfs_adaptor(root.c_str(), ioengine_libaio);
  auto alignFs = new_aligned_fs_adaptor(mediaFs, 4 * 1024, true, true);
  auto cacheAllocator = new AlignedAlloc(4 * 1024);
  auto roCachedFs = new_full_file_cached_fs(nullptr, alignFs, 1024 * 1024,
      1, 1000 * 1000 * 1, 128ul * 1024 * 1024, cacheAllocator, 0);
  auto cachePool = roCachedFs->get_pool();
  DEFER({ delete cacheAllocator; delete roCachedFs; });

  const size_t bufSize = 1024 * 1024;
  IOVector buffer(*cacheAllocator);
  buffer.push_back(bufSize);
  auto fileName = "/huge_file";

  auto cacheStore = cachePool->open(fileName, O_CREAT | O_RDWR, 0644);
  cacheStore->set_actual_size(200 * bufSize);

  off_t last_evict_offset = 0;
  off_t written = 0;
  auto random_release_evict_open = [&]() {
    if (rand() % 5 == 0) {
      cacheStore->release();
      cacheStore = nullptr;
    }
    if (rand() % 3 == 0) {
      cachePool->evict(fileName);
      last_evict_offset = written;
    }
    if (rand() % 5 == 0 && cacheStore) {
      cacheStore->release();
      cacheStore = nullptr;
    }
    if (cacheStore == nullptr) {
      cacheStore = cachePool->open(fileName, O_CREAT | O_RDWR, 0644);
    }
  };

  for (int i = 0; i < 100; i++) {
    random_release_evict_open();
    auto ret = cacheStore->do_pwritev2(buffer.iovec(), buffer.iovcnt(), written, 0);
    EXPECT_EQ(ret, (ssize_t)bufSize);
    written += ret;

    if (rand() % 4 == 0) {
      // write again
      random_release_evict_open();
      auto ret = cacheStore->do_pwritev2(buffer.iovec(), buffer.iovcnt(), written, 0);
      EXPECT_EQ(ret, (ssize_t)bufSize);
      written += ret;
    }

    for (int j = 0; j < 10; j++) {
      random_release_evict_open();
      off_t qoffset = (rand() % ((i + 1) * 3)) * bufSize;
      size_t qsize = (rand() % 10 + 1) * bufSize;
      auto qres = cacheStore->queryRefillRange(qoffset, qsize);
      if (qoffset + qsize <= (size_t)last_evict_offset || qoffset >= written ||
          (qoffset < last_evict_offset && qoffset + qsize > (size_t)written)) {
        EXPECT_EQ(qres.first, qoffset);
        EXPECT_EQ(qres.second, qsize);
      } else if (qoffset < last_evict_offset && qoffset + qsize <= (size_t)written) {
        EXPECT_EQ(qres.first, qoffset);
        EXPECT_EQ(qres.second, size_t(last_evict_offset - qoffset));
      } else if (qoffset >= last_evict_offset && qoffset + qsize > (size_t)written) {
        EXPECT_EQ(qres.first, written);
        EXPECT_EQ(qres.second, size_t(qoffset + qsize - written));
      } else {
        EXPECT_EQ(qres.second, 0UL);
        IOVector read_buffer(*cacheAllocator);
        read_buffer.push_back(qsize);
        ret = cacheStore->do_preadv2(read_buffer.iovec(), read_buffer.iovcnt(), qoffset, 0);
        EXPECT_EQ(ret, (ssize_t)qsize);
      }
    }
  }

  if (cacheStore) cacheStore->release();
}

TEST(CachePool, open_same_file) {
  std::string root = "/tmp/ease/cache/open_same_file/";
  SetupTestDir(root);
  auto mediaFs = new_localfs_adaptor(root.c_str(), ioengine_libaio);
  auto alignFs = new_aligned_fs_adaptor(mediaFs, 4 * 1024, true, true);
  auto cacheAllocator = new AlignedAlloc(4 * 1024);
  auto roCachedFs = new_full_file_cached_fs(nullptr, alignFs, 1024 * 1024,
      1, 1000 * 1000 * 1, 128ul * 1024 * 1024, cacheAllocator, 0);
  auto cachePool = roCachedFs->get_pool();
  DEFER({ delete cacheAllocator; delete roCachedFs; });

  auto fileName = "/testDir/testfile";

  uint64_t written = 0;
  for (int j = 0; j < 5; j++) {
    std::vector<ICacheStore*> cacheStores;
    for (int i = 0; i < 10; i++) {
      cacheStores.push_back(cachePool->open(fileName, O_CREAT | O_RDWR, 0644));
      ASSERT_NE(nullptr, cacheStores.back());
    }

    const size_t bufSize = 1024 * 1024;
    IOVector buffer(*cacheAllocator);
    buffer.push_back(bufSize);

    for (auto cacheStore : cacheStores) {
      if (rand() % 2 == 0) {
        cacheStore->release();
      } else {
        auto ret = cacheStore->do_pwritev2(buffer.iovec(), buffer.iovcnt(), written, 0);
        EXPECT_EQ(ret, (ssize_t)bufSize);
        written += ret;
        cacheStore->release();
      }
    }
  }
}

// Friend accessor — declared as friend in FileCachePool.
struct FileCachePoolTest {
  static void set_hot_lru_limit(FileCachePool *p, uint32_t limit) {
    p->hotLruLimit_ = limit;
  }
  static size_t hot_size (FileCachePool *p) { return p->lru_.size(); }
  static size_t cold_size(FileCachePool *p) { return p->cold_.size(); }
  static bool in_hot (FileCachePool *p, std::string_view n) {
    return p->fileIndex_.find(n) != p->fileIndex_.end();
  }
  static bool in_cold(FileCachePool *p, std::string_view n) {
    return p->coldIndex_.find(n) != p->coldIndex_.end();
  }
};

// Open through the pool then immediately release.
static bool openClose(ICachePool* pool, const char* name) {
  auto s = pool->open(name, O_CREAT | O_RDWR, 0644);
  bool success = (s != nullptr);
  if (s) s->release();
  return success;
}

TEST(CachePool, test_hot_lru_limit) {
  std::string root = "/tmp/ease/cache/test_hot_lru_limit/";
  SetupTestDir(root);
  const size_t hotLimit = 10;
  const size_t fileNum = 100;

  auto mediaFs = new_localfs_adaptor(root.c_str(), ioengine_libaio);
  auto alignFs = new_aligned_fs_adaptor(mediaFs, 4 * 1024, true, true);
  auto cacheAllocator = new AlignedAlloc(4 * 1024);
  auto roCachedFs = new_full_file_cached_fs(nullptr, alignFs, 1024 * 1024,
      1, 1000 * 1000 * 1, 128ul * 1024 * 1024, cacheAllocator, 0, nullptr, 1000);
  auto cachePool = roCachedFs->get_pool();
  DEFER({ delete cacheAllocator; delete roCachedFs; });
  using T = FileCachePoolTest;

  auto pool = dynamic_cast<FileCachePool*>(cachePool);
  ASSERT_NE(nullptr, pool);
  T::set_hot_lru_limit(pool, hotLimit);

  for (size_t i = 0; i < fileNum; i++) {
    photon::thread_usleep(100);
    std::string name = "/f" + std::to_string(i);
    ASSERT_TRUE(openClose(pool, name.c_str()));
  }

  EXPECT_LE(T::hot_size(pool), hotLimit);
  EXPECT_EQ(T::hot_size(pool) + T::cold_size(pool), fileNum);

  // first fileNum-hotLimit files are in cold
  for (size_t i = 0; i < fileNum - hotLimit; i++) {
    std::string name = "/f" + std::to_string(i);
    EXPECT_TRUE(T::in_cold(pool, name));
    EXPECT_FALSE(T::in_hot(pool, name));
  }
  // last hotLimit files are in hot
  for (size_t i = fileNum - hotLimit; i < fileNum; i++) {
    std::string name = "/f" + std::to_string(i);
    EXPECT_TRUE(T::in_hot(pool, name));
    EXPECT_FALSE(T::in_cold(pool, name));
  }
  // re-open /f0 — must promote to hot
  ASSERT_TRUE(openClose(pool, "/f0"));
  EXPECT_TRUE(T::in_hot(pool, "/f0"));
  EXPECT_FALSE(T::in_cold(pool, "/f0"));

  EXPECT_LE(T::hot_size(pool), hotLimit);
  EXPECT_EQ(T::hot_size(pool) + T::cold_size(pool), fileNum);

  // random access
  for (size_t i = 0; i < 100; i++) {
    int r = rand() % fileNum;
    std::string name = "/f" + std::to_string(r);
    ASSERT_TRUE(openClose(pool, name.c_str()));

    if (rand() % 3 == 0) {
      EXPECT_EQ(T::hot_size(pool) + T::cold_size(pool), fileNum);
    }
  }
}

TEST(CachePool, evict_cold_file) {
  std::string root = "/tmp/ease/cache/evict_cold_file/";
  SetupTestDir(root);
  const size_t hotLimit = 10;
  const size_t fileNum = 100;

  auto mediaFs = new_localfs_adaptor(root.c_str(), ioengine_libaio);
  auto alignFs = new_aligned_fs_adaptor(mediaFs, 4 * 1024, true, true);
  auto cacheAllocator = new AlignedAlloc(4 * 1024);
  auto roCachedFs = new_full_file_cached_fs(nullptr, alignFs, 1024 * 1024,
      1, 1000 * 1000 * 1, 128ul * 1024 * 1024, cacheAllocator, 0, nullptr, 1000);
  auto cachePool = roCachedFs->get_pool();
  DEFER({ delete cacheAllocator; delete roCachedFs; });
  using T = FileCachePoolTest;

  auto pool = dynamic_cast<FileCachePool*>(cachePool);
  ASSERT_NE(nullptr, pool);
  T::set_hot_lru_limit(pool, hotLimit);

  for (size_t i = 0; i < fileNum; i++) {
    photon::thread_usleep(100);
    std::string name = "/f" + std::to_string(i);
    ASSERT_TRUE(openClose(pool, name.c_str()));
  }

  photon::thread_sleep(1);
  ASSERT_TRUE(T::in_cold(pool, "/f0"));
  EXPECT_LE(T::hot_size(pool), hotLimit);
  EXPECT_EQ(T::hot_size(pool) + T::cold_size(pool), fileNum);

  ASSERT_EQ(0, pool->evict("/f0"));
  EXPECT_FALSE(T::in_cold(pool, "/f0"));
  EXPECT_FALSE(T::in_hot(pool, "/f0"));
  EXPECT_EQ(T::hot_size(pool), hotLimit);
  EXPECT_EQ(T::hot_size(pool) + T::cold_size(pool), fileNum - 1);

  std::vector<bool> evicted(fileNum, false);
  evicted[0] = true;
  for (size_t i = 0; i < 9; i++) {
    int index = rand() % fileNum;
    std::string name = "/f" + std::to_string(index);
    while (evicted[index] || !T::in_cold(pool, name)) {
      index = rand() % fileNum;
      name = "/f" + std::to_string(index);
    }
    ASSERT_EQ(0, pool->evict(name));
    evicted[index] = true;
    EXPECT_FALSE(T::in_cold(pool, name));
    EXPECT_EQ(T::cold_size(pool), fileNum - hotLimit - 2 - i);
  }
}

TEST(CachePool, evict_cold_first_when_full) {
  std::string root = "/tmp/ease/cache/evict_cold_first_when_full/";
  SetupTestDir(root);
  const uint64_t capacityGB = 1;
  const size_t hotLimit = 5;
  const size_t fileNum = 40;
  const size_t fileSizeMB = 60;
  const size_t fileSizeBytes = fileSizeMB * 1024 * 1024;

  auto mediaFs = new_localfs_adaptor(root.c_str(), ioengine_libaio);
  auto alignFs = new_aligned_fs_adaptor(mediaFs, 4 * 1024, true, true);
  auto cacheAllocator = new AlignedAlloc(4 * 1024);
  auto roCachedFs = new_full_file_cached_fs(nullptr, alignFs, 1024 * 1024,
      capacityGB, 0, 128ul * 1024 * 1024, cacheAllocator, 0, nullptr, 1000);
  auto cachePool = roCachedFs->get_pool();
  DEFER({ delete cacheAllocator; delete roCachedFs; });
  using T = FileCachePoolTest;

  auto pool = dynamic_cast<FileCachePool*>(cachePool);
  ASSERT_NE(nullptr, pool);
  T::set_hot_lru_limit(pool, hotLimit);

  IOVector buffer(*cacheAllocator);
  buffer.push_back(fileSizeBytes);

  for (size_t i = 0; i < fileNum; i++) {
    photon::thread_usleep(1000);
    std::string name = "/g" + std::to_string(i);
    auto store = cachePool->open(name.c_str(), O_CREAT | O_RDWR, 0644);
    ASSERT_NE(nullptr, store);
    store->do_pwritev2(buffer.iovec(), buffer.iovcnt(), 0, 0);
    store->release();
  }

  size_t remaining = T::hot_size(pool) + T::cold_size(pool);
  EXPECT_LT(remaining, fileNum);
  EXPECT_LE(T::hot_size(pool), (size_t)hotLimit);

  for (size_t i = fileNum - hotLimit; i < fileNum; i++) {
    std::string name = "/g" + std::to_string(i);
    EXPECT_TRUE(T::in_hot(pool, name)) << name << " should still be in hot";
    EXPECT_FALSE(T::in_cold(pool, name)) << name << " must not be in cold";
  }

  // Write a new file to trigger eviction, the cold file should be evicted first
  std::string name = "/g" + std::to_string(fileNum);
  auto store = cachePool->open(name.c_str(), O_CREAT | O_RDWR, 0644);
  ASSERT_NE(nullptr, store);
  store->do_pwritev2(buffer.iovec(), buffer.iovcnt(), 0, 0);
  store->release();
  for (size_t i = fileNum - hotLimit; i < fileNum; i++) {
    std::string name = "/g" + std::to_string(i);
    bool existed = T::in_hot(pool, name) || T::in_cold(pool, name);
    EXPECT_TRUE(existed) << name << " must still exist";
  }
}

}
}
int main(int argc, char** argv) {
  log_output_level = ALOG_ERROR;
  // photon::vcpu_init();
  ::testing::InitGoogleTest(&argc, argv);

  photon::init();
  DEFER(photon::fini());

  auto ret = RUN_ALL_TESTS();

  // photon::vcpu_fini();

  return ret;
}
