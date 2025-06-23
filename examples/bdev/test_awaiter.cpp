#include <spdk/bdev.h>
#include <spdk/env.h>
#include <spdk/event.h>

#include <photon/photon.h>
#include <photon/thread/awaiter.h>

#include <semaphore.h>

using namespace photon;

spdk_thread* g_app_thread;
std::thread* bg_thread;

struct MyStruct1 {
    Awaiter<PhotonContext>* awaiter1; 
    MyStruct1(Awaiter<PhotonContext>* aw) : awaiter1(aw) {}
};

struct MyStruct2 {
    Awaiter<PhotonContext>* awaiter2;

    MyStruct2(Awaiter<PhotonContext>* aw) : awaiter2(aw) {}
};

void function2(void* arg) {
    MyStruct2* ms2 = reinterpret_cast<MyStruct2*>(arg);
    ms2->awaiter2->resume();
}

void function(void* arg) {
    MyStruct1* ms1 = reinterpret_cast<MyStruct1*>(arg);
    MyStruct2 ms2(ms1->awaiter1);
    function2(&ms2);
}

void start_fn(void* arg) {
    auto sem = (sem_t*)arg;
    g_app_thread = spdk_get_thread();
    sem_post(sem);
}


int main() {
    const char* json_config_file_path = "/home/hongjingxuan.hjx/photon_github_spdk_bdev/examples/bdev/bdev.json";

    spdk_app_opts opts;
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "photon_spdk_bdev";
    opts.json_config_file = json_config_file_path;

    sem_t sem;
    sem_init(&sem, 0, 0);
    int rc = 0;
    bg_thread = new std::thread([](spdk_app_opts* opts, sem_t* sem, int* rc){
        *rc = spdk_app_start(opts, start_fn, sem);
        // std::cout << "spdk app start rc=" << *rc << std::endl;  
        if (*rc != 0) {
            sem_post(sem);
        }
    }, &opts, &sem, &rc);

    // std::cout << "bdev env init before wait" << std::endl;
    sem_wait(&sem);
    // std::cout << "bdev env init after wait" << std::endl;
    assert(rc == 0);


    photon::init();

    Awaiter<PhotonContext> awaiter;
    MyStruct1 ms1(&awaiter);
    spdk_thread_send_msg(g_app_thread, function, &ms1);
    awaiter.suspend();




    photon::fini();

    // std::cout << "bdev env fini get into" << std::endl;
    spdk_thread_send_critical_msg(g_app_thread, [](void* arg){
        spdk_app_stop(0);
    });

    if (bg_thread) {
        bg_thread->join();
        delete bg_thread;
    }

    // std::cout << "bdev env fini exit" << std::endl;
}