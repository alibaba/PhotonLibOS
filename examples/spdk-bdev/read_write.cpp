#include <photon/io/spdkbdev-wrapper.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>

#include <vector>
#include <cassert>

/* how to compile:
 * compile spdk:
 * Download spdk source code in /path/to/spdk
 * cd /path/to/spdk
 * ./configure --with-shared
 * make -j 8
 * photon spdk bdev rely on some spdk dynamic libraries, and these libraries rely on somd dpdk libraries and the isal library
 * so, change the ${SPDK_ROOT} in CMake/Findspdk.cmake to yours
 * then compile photon:
 * cd /path/to/photon
 * cmake -B build -DPHOTON_ENABLE_SPDK_BDEV=ON -DPHOTON_BUILD_TESTING=ON
 * make -C build -j 8
*/

/* how to run:
 * example:
 * sudo ./build/examples-output/bdev-example --json ./examples/spdk-bdev/bdev.json
 * some simple tests:
 * sudo ./build/output/test-spdkbdev
*/

int main(int argc, char** argv) {
    // launch spdk app in another std thread, just like a background thread
    // then send msg to it, and embed photon thread yield mechanism in the arg of call back
    // so now, the std thread can solve some other things while waiting the "send msg" to be complete

    // this function just launch the spdk app in a background thread 
    // (i.e. just mapping block devices configured in the json to this process)
    // not open any bdev, do it later
    photon::spdk::bdev_env_init(argc, argv);

    // although the following operations need to be done in the background thread (app thread) through send msg
    // read/write only can be done after getting the bdev descriptor and io channel
    // now, (there is only one block device called Malloc0), open bdev and get io channel is somehow blocking operation
    // or if you have more than one bdev, you can try to open bdevs/io channels in parallel
    struct spdk_bdev_desc* desc = nullptr;
    struct spdk_io_channel* ch = nullptr;
    int rc = photon::spdk::bdev_open_ext("Malloc0", true, &desc);
    assert (rc == 0);
    assert (desc != nullptr);
    ch = photon::spdk::bdev_get_io_channel(desc);
    assert (ch != nullptr);
    (void)ch;
    (void)rc;

    // alloc buffer through spdk_dma_malloc / spdk_zmalloc/ ... like before
    uint64_t bufsz = 4096;
    void* buf_write = spdk_zmalloc(bufsz, 4096, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
    void* buf_read = spdk_zmalloc(bufsz, 4096, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);

    {
        photon::init();
        DEFER(photon::fini());

        uint64_t block_sz = 512;
        uint64_t nblocks = bufsz / block_sz;

        char test_data[] = "hello world";   // prepare datas to write
        for (uint64_t i=0; i<nblocks; i++) {
            strncpy((char*)buf_write + i*block_sz, test_data, 12);
        }

        // writes in parallel
        std::vector<photon::join_handle*> ths_write;
        for (uint64_t i=0; i<nblocks; i++) {
            LOG_INFO("write ", VALUE(i));
            void* buf = (void*)((char*)buf_write + i * block_sz);
            uint64_t off = i * block_sz, cnt = block_sz;
            ths_write.emplace_back(
                photon::thread_enable_join(
                    photon::thread_create11(
                    [](int idx, struct spdk_bdev_desc* desc, struct spdk_io_channel* ch, void* buffer, uint64_t offset, uint64_t nbytes) {
                        LOG_INFO("write in ", VALUE(idx));
                        int rc = photon::spdk::bdev_write(desc, ch, buffer, offset, nbytes);
                        LOG_INFO("write out ", VALUE(idx), VALUE(rc));
                        (void)rc;
                    }, i, desc, ch, buf, off, cnt)
                )
            );
        }
        for (auto th : ths_write) { // wait all writes complete
            photon::thread_join(th);
        }


        // reads in parallel
        std::vector<photon::join_handle*> ths_read;
        for (uint64_t i=0; i<nblocks; i++) {
            LOG_INFO("read ", VALUE(i));
            void* buf = (void*)((char*)buf_read + i * block_sz);
            uint64_t off = i * block_sz, cnt = block_sz;
            ths_read.emplace_back(
                photon::thread_enable_join(
                    photon::thread_create11(
                    [](int idx, struct spdk_bdev_desc* desc, struct spdk_io_channel* ch, void* buffer, uint64_t offset, uint64_t nbytes) {
                        LOG_INFO("read in ", VALUE(idx));
                        int rc = photon::spdk::bdev_read(desc, ch, buffer, offset, nbytes);
                        LOG_INFO("read out ", VALUE(idx), VALUE(rc));
                        (void)rc;
                    }, i, desc, ch, buf, off, cnt)
                )
            );
        }
        for (auto th: ths_read) {   // wait all reads complete
            photon::thread_join(th);
        }

        if (memcmp(buf_write, buf_read, bufsz) != 0) {  // checking
            LOG_ERROR("buf_write != buf_read");
        }
        else {
            LOG_INFO("buf_write == buf_read");
        }
    }

    spdk_free(buf_write);
    spdk_free(buf_read);

    photon::spdk::bdev_put_io_channel(ch);
    photon::spdk::bdev_close(desc);

    photon::spdk::bdev_env_fini();
}