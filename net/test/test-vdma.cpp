#include "../../test/gtest.h"
#include "../vdma/shm.cpp"
#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/thread/thread11.h>
#include <photon/thread/workerpool.h>

#include <vector>

TEST(Buffer, encode_decode) {
    std::string str;
    photon::SharedMemoryBuffer::encode_to(str, 111, 222);
    uint64_t idx = 0;
    size_t bufsz = 0;
    photon::SharedMemoryBuffer::decode_from(str, &idx, &bufsz);
    EXPECT_EQ(111, idx);
    EXPECT_EQ(222, bufsz);
}

TEST(Target, basic) {
    photon::vDMATarget* target = photon::new_shm_vdma_target("foo", 65536, 4096);
    DEFER(delete target);
    photon::vDMAInitiator* initiator= photon::new_shm_vdma_initiator("foo", 65536);
    DEFER(delete initiator);

    std::string id_0_4096, id_1_4096, id_2_4096, id_3_4096;
    photon::SharedMemoryBuffer::encode_to(id_0_4096, 0, 4096);
    photon::SharedMemoryBuffer::encode_to(id_1_4096, 1, 4096);
    photon::SharedMemoryBuffer::encode_to(id_2_4096, 2, 4096);
    photon::SharedMemoryBuffer::encode_to(id_3_4096, 3, 4096);

    // in this version, alloc size must same as unit
    photon::vDMABuffer* buffer = target->alloc(512);
    EXPECT_EQ(buffer, nullptr);
    
    // target alloc
    photon::vDMABuffer* t_buffer0 = target->alloc(4096);
    EXPECT_EQ(reinterpret_cast<photon::SharedMemoryBuffer*>(t_buffer0)->idx(), 0);
    EXPECT_EQ(t_buffer0->buf_size(), 4096);
    EXPECT_EQ(t_buffer0->id().size(), id_0_4096.size());
    EXPECT_EQ(memcmp(t_buffer0->id().data(), id_0_4096.data(), id_0_4096.size()), 0);

    photon::vDMABuffer* t_buffer1 = target->alloc(4096);
    EXPECT_EQ(t_buffer1->id().size(), id_1_4096.size());
    EXPECT_EQ(memcmp(t_buffer1->id().data(), id_1_4096.data(), id_1_4096.size()), 0);
    EXPECT_EQ(t_buffer1->buf_size(), 4096);
    EXPECT_EQ(t_buffer1->is_valid(), true);
    EXPECT_EQ(t_buffer1->is_registered(), true);
    EXPECT_EQ(t_buffer1->address(), (char*)t_buffer0->address()+4096);

    photon::vDMABuffer* t_buffer2 = target->alloc(4096);
    EXPECT_EQ(t_buffer2->id().size(), id_2_4096.size());
    EXPECT_EQ(memcmp(t_buffer2->id().data(), id_2_4096.data(), id_2_4096.size()), 0);
    EXPECT_EQ(t_buffer2->address(), (char*)t_buffer1->address()+4096);

    // initiator map
    photon::vDMABuffer* i_buffer0 = initiator->map(t_buffer0->id());
    EXPECT_EQ(i_buffer0->buf_size(), t_buffer0->buf_size());
    EXPECT_EQ(i_buffer0->id().size(), t_buffer0->id().size());
    EXPECT_EQ(memcmp(i_buffer0->id().data(), t_buffer0->id().data(), 16), 0);

    char tmp[4096];
    memset(tmp, 0x5F, 4096);
    memcpy(t_buffer0->address(), tmp, 4096);
    EXPECT_EQ(memcmp(i_buffer0->address(), tmp, 4096), 0);
    memset(tmp, 0x22, 4096);
    memcpy(i_buffer0->address(), tmp, 4096);
    EXPECT_EQ(memcmp(t_buffer0->address(), tmp, 4096), 0);

    initiator->unmap(i_buffer0);
    photon::vDMABuffer* t_buffer3 = target->alloc(4096);
    EXPECT_EQ(t_buffer3->id(), id_3_4096);

    i_buffer0 = initiator->map(t_buffer0->id());
    photon::vDMABuffer* i_buffer1 = initiator->map(t_buffer1->id());

    initiator->unmap(i_buffer0);
    initiator->unmap(i_buffer1);

    target->dealloc(t_buffer0);
    target->dealloc(t_buffer1);
    target->dealloc(t_buffer2);
    target->dealloc(t_buffer3);
}

void func1(photon::vDMATarget* target, int id) {
    auto buf = target->alloc(4096);
    LOG_INFO("thread ", id, " get buf ", reinterpret_cast<photon::SharedMemoryBuffer*>(buf)->idx());
    photon::thread_sleep(1);
    target->dealloc(buf);
    LOG_INFO("thread ", id, " release buf");
}

TEST(Target, single_vcpu_multi_thread_alloc_1) {
    // 16 buffer, 16 thread(alloc - sleep 1s - free)
    // The result should be that each thread is allocated a different buffer
    auto target = photon::new_shm_vdma_target("foo", 65536, 4096);
    std::vector<photon::join_handle*> threads;
    for (int i=0; i<16; i++) {
        threads.push_back(photon::thread_enable_join(photon::thread_create11(func1, target, i)));
    }
    LOG_INFO("all create success");
    for (int i=0; i<16; i++) {
        photon::thread_join(threads[i]);
    }
}

void func2(photon::vDMATarget* target, int id) {
    std::vector<int> stats(16, 0);
    for (int i=0; i<1000000; i++) {
        auto buf = target->alloc(4096);
        if (!buf) {
            LOG_INFO("some wrong ", id, " not get buffer in the middle");
        }
        for (int j=0; j<10; j++) {
            photon::thread_yield();
        }
        stats[reinterpret_cast<photon::SharedMemoryBuffer*>(buf)->idx()] += 1;
        target->dealloc(buf);
    }
    std::string output;
    for (const auto& t : stats) output.append(std::to_string(t) + " ");
    LOG_INFO(id, " res ", output.c_str());
}

TEST(Target, single_vcpu_multi_thread_alloc_2) {
    // 16 buffer, 16 thread (repeat "alloc-free" 1000000 times)
    // alloc should be all success, test hold-release lock
    auto target = photon::new_shm_vdma_target("foo", 65536, 4096);
    std::vector<photon::join_handle*> threads;
    for (int i=0; i<16; i++) {
        threads.push_back(photon::thread_enable_join(photon::thread_create11(func2, target, i)));
    }
    LOG_INFO("all create success");
    for (int i=0; i<16; i++) {
        photon::thread_join(threads[i]);
    }
}

void func3(photon::vDMATarget* target, int id) {
    auto buf = target->alloc(4096);
    if (buf) {
        LOG_INFO("thread ", id, " get buf ", reinterpret_cast<photon::SharedMemoryBuffer*>(buf)->idx());
        photon::thread_sleep(10);
        target->dealloc(buf);
        LOG_INFO("thread ", id, " release buf");
    }
    else {
        LOG_INFO("thread ", id, " not get buf");
    }
}

TEST(Target, single_vcpu_multi_thread_alloc_3) {
    // 16 buffer, 20 thread (alloc - sleep 10s - dealloc)
    // only 16 thread can get buffer, other 4 thread will retry.
    // now, sleep time is very long
    // so, other 4 will get a nullptr in the end.
    // After a while, 16 thread release their buffer.
    auto target = photon::new_shm_vdma_target("foo", 65536, 4096);
    std::vector<photon::join_handle*> threads;
    for (int i=0; i<20; i++) {
        threads.push_back(photon::thread_enable_join(photon::thread_create11(func3, target, i)));
    }
    LOG_INFO("all create success");
    for (int i=0; i<20; i++) {
        photon::thread_join(threads[i]);
    }
}

void func4(photon::vDMATarget* target, int id) {
    auto buf = target->alloc(4096);
    if (buf) {
        LOG_INFO("thread ", id, " get buf ", reinterpret_cast<photon::SharedMemoryBuffer*>(buf)->idx());
        photon::thread_usleep(1);
        target->dealloc(buf);
        LOG_INFO("thread ", id, " release buf");
    }
    else {
        LOG_INFO("thread ", id, " not get buf");
    }
}

TEST(Target, single_vcpu_multi_thread_alloc_4) {
    // 16 buffer, 20 thread (alloc - sleep 1s - dealloc)
    // only 16 thread can get buffer at first, other thread will retry
    // sleep time is short, other 4 thread may get a buffer
    auto target = photon::new_shm_vdma_target("foo", 65536, 4096);
    std::vector<photon::join_handle*> threads;
    for (int i=0; i<20; i++) {
        threads.push_back(photon::thread_enable_join(photon::thread_create11(func4, target, i)));
    }
    LOG_INFO("all create success");
    for (int i=0; i<20; i++) {
        photon::thread_join(threads[i]);
    }
}

TEST(Target, multi_vcpu_multi_thread) {
    // 16 buffer, 4 vcpu, each 16 thread (total 64 thread), 10000 task
    auto target = photon::new_shm_vdma_target("foo", 65536, 4096);
    photon::WorkPool wp(4, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE, 32);
    photon::semaphore sem(0);
    // rough statstic
    std::vector<int> stats(16, 0);
    int n_notget = 0;
    for (int i=0; i<10000; i++) {
        wp.async_call(new auto ([&]{
            auto buf = target->alloc(4096);
            if (buf) {
                int buf_idx = reinterpret_cast<photon::SharedMemoryBuffer*>(buf)->idx();
                LOG_INFO("task ", i, " get buf ", buf_idx);
                photon::thread_usleep(1);
                target->dealloc(buf);
                LOG_INFO("task ", i, " release buf ", buf_idx);
                stats[buf_idx] += 1;
            }
            else {
                LOG_INFO("task ", i, " not get buf");
                n_notget++;
            }
            sem.signal(1);
        }));
    }

    sem.wait(10000);    // wait most async task to complete

    std::string output;
    for (int i=0; i<16; i++) output.append(std::to_string(stats[i]) + " ");
    LOG_INFO(output.c_str());
    LOG_INFO("get failed: ", n_notget);
}

int main(int argc, char **argv){
    srand(time(nullptr));
    ::testing::InitGoogleTest(&argc, argv);
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
    return RUN_ALL_TESTS();
}