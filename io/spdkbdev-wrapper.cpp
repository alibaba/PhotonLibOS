#include "spdkbdev-wrapper.h"

namespace photon {
namespace spdk {

spdk_thread* g_app_thread;
std::thread* bg_thread;
spdk_app_opts opts;

void start_fn(void* arg) {
    auto sem = (sem_t*)arg;
    g_app_thread = spdk_get_thread();
    sem_post(sem);
}

void bdev_env_init_impl(spdk_app_opts* opts) {
    sem_t sem;
    sem_init(&sem, 0, 0);

    bg_thread = new std::thread([](spdk_app_opts* opts, sem_t* sem){
        int rc = spdk_app_start(opts, start_fn, sem);
        LOG_DEBUG("spdk app start ", VALUE(rc));
        if (rc != 0) {
            sem_post(sem);
        }
        else {
            spdk_app_fini();
        }
    }, opts, &sem);

    LOG_DEBUG("bdev env init before wait");
    sem_wait(&sem);
    LOG_DEBUG("bdev env init after wait");
}

void bdev_env_init(int argc, char** argv) {
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "photon_spdk_bdev";

    if (spdk_app_parse_args(argc, argv, &opts, nullptr, nullptr, nullptr, nullptr) != SPDK_APP_PARSE_ARGS_SUCCESS) {
        exit(-1);
    }

    bdev_env_init_impl(&opts);
}

void bdev_env_init(const char* json_cfg_path) {
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "photon_spdk_bdev";
    opts.json_config_file = json_cfg_path;

    bdev_env_init_impl(&opts);
}

void bdev_env_fini() {
    LOG_DEBUG("bdev env fini get into");

    spdk_thread_send_critical_msg(g_app_thread, [](void* arg){
        spdk_app_stop(0);
    });

    if (bg_thread) {
        bg_thread->join();
        delete bg_thread;
    }

    LOG_DEBUG("bdev env fini exit");
}


int bdev_open_ext(const char* bdev_name, bool write, struct spdk_bdev_desc** desc) {
    int rc = 0;
    
    sem_t sem;
    sem_init(&sem, 0, 0);
    struct Tmp {
        std::string_view bdev_name;
        bool write;
        sem_t* sem;
        int* rc;
        struct spdk_bdev_desc** desc;
    };
    Tmp tmp = {bdev_name, write, &sem, &rc, desc};
    spdk_thread_send_msg(g_app_thread, [](void* arg){
        Tmp* tmp = (Tmp*)arg;
        *tmp->rc = spdk_bdev_open_ext(tmp->bdev_name.data(), tmp->write, 
        [](enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx){}, 
        nullptr, tmp->desc);

        sem_post(tmp->sem);
    }, &tmp);

    LOG_DEBUG("bdev open ext wait");
    sem_wait(&sem);
    LOG_DEBUG("bdev open ext success");

    return rc;
}

struct spdk_io_channel* bdev_get_io_channel(spdk_bdev_desc* desc) {
    sem_t sem;
    sem_init(&sem, 0, 0);

    struct spdk_io_channel* ch = nullptr;
    
    struct Tmp {
        struct spdk_bdev_desc* desc;
        struct spdk_io_channel** ch;
        sem_t* sem;
    };
    Tmp tmp = {desc, &ch, &sem};

    spdk_thread_send_msg(g_app_thread, [](void* arg) {
        Tmp* tmp = (Tmp*)arg;
        *tmp->ch = spdk_bdev_get_io_channel(tmp->desc);
        sem_post(tmp->sem);
    }, &tmp);

    LOG_DEBUG("get io channel wait");
    sem_wait(&sem);
    LOG_DEBUG("get io channel success");

    return ch;
}

void bdev_put_io_channel(struct spdk_io_channel* ch) {
    sem_t sem;
    sem_init(&sem, 0, 0);
    
    struct Tmp {
        struct spdk_io_channel* ch;
        sem_t* sem;
    };
    Tmp tmp = {ch, &sem};

    spdk_thread_send_msg(g_app_thread, [](void* arg) {
        Tmp* tmp = (Tmp*)arg;
        spdk_put_io_channel(tmp->ch);
        sem_post(tmp->sem);
    }, &tmp);

    LOG_DEBUG("put io channel wait");
    sem_wait(&sem);
    LOG_DEBUG("put io channel success");
}

void bdev_close(struct spdk_bdev_desc* desc) {
    sem_t sem;
    sem_init(&sem, 0, 0);
    
    struct Tmp {
        struct spdk_bdev_desc* desc;
        sem_t* sem;
    };
    Tmp tmp = {desc, &sem};

    spdk_thread_send_msg(g_app_thread, [](void* arg) {
        Tmp* tmp = (Tmp*)arg;
        spdk_bdev_close(tmp->desc);
        sem_post(tmp->sem);
    }, &tmp);

    LOG_DEBUG("bdev close wait");
    sem_wait(&sem);
    LOG_DEBUG("bdev close success");
}


}   // namespace spdk
}   // namespace photon