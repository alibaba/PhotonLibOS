#include <photon/io/spdkbdev-wrapper.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>
#include <photon/common/alog-stdstring.h>

#include <vector>
#include <cassert>

int main(int argc, char** argv) {
    photon::init();
    DEFER(photon::fini());

    // launch spdk app in another std thread, just like a background thread
    // then send msg to it, and embed photon semaphore in the arg of call back
    // so now, the std thread can solve some other things while waiting the "send msg" to be complete

    // this function just launch the spdk app in a background thread 
    // not open any bdev, do it later
    photon::spdk::bdev_env_init(argc, argv);
    DEFER(photon::spdk::bdev_env_fini());

    // although the following operations need to be done in the background thread (app thread) through send msg
    // read/write only can be start solving after getting the bdev descriptor and io channel
    // now, (there is only one block device called Malloc0), open bdev and get io channel is somehow blocking operation
    // or if you have more than one bdev, you can try to open bdevs/io channels in parallel
    struct spdk_bdev_desc* desc = nullptr;
    struct spdk_io_channel* ch = nullptr;

    if (photon::spdk::bdev_open_ext("Malloc0", true, &desc) != 0 || desc == nullptr) {
        LOG_ERROR_RETURN(0, -1, "failed to open bdev");
    }
    DEFER(photon::spdk::bdev_close(desc));

    ch = photon::spdk::bdev_get_io_channel(desc);
    if (ch == nullptr) {
        LOG_ERROR_RETURN(0, -1, "failed to get io channel");
    }
    DEFER(photon::spdk::bdev_put_io_channel(ch));

    // alloc buffer like before
    uint64_t bufsz = 512;
    void* buf_write = spdk_zmalloc(bufsz, 0, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
    void* buf_read = spdk_zmalloc(bufsz, 0, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
    if (buf_write == nullptr || buf_read == nullptr) {
        LOG_ERROR_RETURN(0, -1, "failed to alloc buffer");
    }
    DEFER(spdk_free(buf_write));
    DEFER(spdk_free(buf_read));

    // prepare datas to write
    char test_data[] = "hello world";   
    strncpy((char*)buf_write, test_data, 12);

    // write then read
    if (photon::spdk::bdev_write(desc, ch, buf_write, 0, bufsz) != 0) {
        LOG_ERROR_RETURN(0, -1, "write failed");
    }
    if (photon::spdk::bdev_read(desc, ch, buf_read, 0, bufsz) != 0) {
        LOG_ERROR_RETURN(0, -1, "read failed");
    }

    // print
    LOG_INFO("burwrite=`, bufread=`", (char*)buf_write, (char*)buf_read);
}