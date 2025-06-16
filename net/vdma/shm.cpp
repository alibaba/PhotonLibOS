#include "photon/net/vdma.h"

#include <photon/common/alog-stdstring.h>
#include <photon/common/string-keyed.h>
#include <photon/common/utility.h>
#include <photon/thread/thread.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#include <unordered_map>

#include <sstream>
#include <iomanip>
#include <iostream>
#include <assert.h>

namespace photon {

class SharedMemoryBuffer : public vDMABuffer {
public:
    SharedMemoryBuffer(uint64_t idx, char* begin_ptr, size_t buffer_size, int type)
    :
    idx_(idx), begin_ptr_(begin_ptr), buffer_size_(buffer_size), type_(type), id_(16, 0)
    {
        // encode idx_ and buffer_size_ to id_
        encode_to(id_, idx_, buffer_size_);
    }

    SharedMemoryBuffer(std::string_view id, char* shm_begin_ptr, int type)
    :
    type_(type)
    {
        // store id to id_
        // decode idx_ and buffer_size_ from id
        // calulate begin_ptr_ use shm_begin_ptr, idx_, buffer_size_
        id_.assign(id.data(), id.size());
        decode_from(id, &idx_, &buffer_size_);
        begin_ptr_ = shm_begin_ptr + idx_ * buffer_size_;
    }

    static void encode_to(std::string& str, uint64_t idx, size_t buffer_size) {
        std::pair<uint64_t, uint64_t> tmpbuf = {idx, buffer_size};
        str.assign((char*)&tmpbuf, 16);
    }

    static void decode_from(const std::string_view str, uint64_t* idx, size_t* buffer_size) {
        auto tmpbuf = (std::pair<uint64_t, uint64_t>*)str.data();
        *idx = tmpbuf->first;
        *buffer_size = tmpbuf->second;
    }

    ~SharedMemoryBuffer() {}

    bool is_registered() const override {
        return true; 
    }

    bool is_valid() const override { 
        return true; 
    }

    std::string_view id() const override {
        return id_;
    }

    void* address() const override {
        return begin_ptr_;
    }

    size_t buf_size() const override {
        return buffer_size_;
    }

    int type_code() const override {
        return type_;
    }

    uint64_t idx() const { return idx_; }

private:
    uint64_t idx_;
    char* begin_ptr_;
    size_t buffer_size_;
    int type_;

    std::string id_;
};

class SharedMemoryBufferAllocator {
public:
    SharedMemoryBufferAllocator() : shm_size_(0), unit_(0), is_inited_(false) {}

    int init(const char* shm_name, size_t shm_size, size_t unit) {
        SCOPED_LOCK(mutex_);
        if (is_inited_) {
            LOG_ERROR_RETURN(0, -1, "SharedMemoryBufferAllocator: already init");
        }

        shm_name_.assign(shm_name);
        shm_size_ = shm_size;
        unit_ = unit;

        shm_fd_ = shm_open(shm_name, O_RDWR | O_CREAT, 0666);
        if (shm_fd_ < 0) {
            LOG_ERROR_RETURN(0, -1, "SharedMemoryBufferAllocator::init, shm_open failed");
        }
        LOG_DEBUG("SharedMemoryBufferAllocator: ", VALUE(shm_fd_), VALUE(shm_size_), VALUE(unit_));

        ftruncate(shm_fd_, shm_size_);
        shm_begin_ptr_ = (char*)mmap(NULL, shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
        if (!shm_begin_ptr_) {
            close(shm_fd_);
            LOG_ERROR_RETURN(0, -1, "SharedMemoryBufferAllocator, mmap failed");
        }

        nbuffer_ = shm_size_ / unit_;
        used_mark_.resize(nbuffer_);
        LOG_DEBUG("SharedMemoryBufferAllocator: ", VALUE(shm_begin_ptr_), VALUE(nbuffer_));
        LOG_DEBUG("SharedMemoryBufferAllocator: ", VALUE(used_mark_.size()), VALUE(buffers_.size()));
        
        for (size_t i=0; i<nbuffer_; i++) {
            buffers_.emplace_back(i, shm_begin_ptr_ + i * unit_, unit_, vDMABufferType::kSharedMem);
        }

        is_inited_ = true;  // allocator init success

        LOG_DEBUG("SharedMemoryBufferAllocator: complete");
        return 0;
    }

    ~SharedMemoryBufferAllocator() {
        SCOPED_LOCK(mutex_);
        if (is_inited_) {
            if (shm_begin_ptr_) {
                munmap(shm_begin_ptr_, shm_size_);
            }
            if (shm_fd_ >= 0) {
                close(shm_fd_);
            }
        }
    }

    vDMABuffer* alloc_one() {
        SCOPED_LOCK(mutex_);
        for (size_t i=0; i<nbuffer_; i++) {
            if (!used_mark_[i]) {
                used_mark_[i] = true;
                return &buffers_[i];
            }
        }
        return nullptr;
    }

    int free_one(vDMABuffer* buf) {
        SCOPED_LOCK(mutex_);
        used_mark_[reinterpret_cast<SharedMemoryBuffer*>(buf)->idx()] = false;
        return 0;
    }

    size_t unit() const { return unit_; }
    
private:
    std::string shm_name_;
    int shm_fd_;
    size_t shm_size_;
    char* shm_begin_ptr_;
    
    size_t unit_;
    size_t nbuffer_;

    photon::mutex mutex_;
    std::vector<SharedMemoryBuffer> buffers_;
    std::vector<bool> used_mark_;

    bool is_inited_;
};

class SharedMemoryTarget : public vDMATarget {
public:
    int init(const char* shm_name, size_t shm_size, size_t unit) {
        return allocator_.init(shm_name, shm_size, unit);
    }

    vDMABuffer* alloc(size_t size) override {
        if (size != allocator_.unit()) {
            LOG_ERROR_RETURN(0, nullptr, "current allocator only support ", allocator_.unit(), ", you ", size);
        }

        vDMABuffer* buf = nullptr;
        buf = allocator_.alloc_one();

        int retry_count = 0;
        while (!buf && retry_count < max_retry_) {
            thread_yield();
            buf = allocator_.alloc_one();
            retry_count++;
        }

        return buf;
    }

    int dealloc(vDMABuffer* buf) override {
        return allocator_.free_one(buf);
    }

    vDMABuffer* register_memory(void* buf, size_t size) override {
        LOG_WARN("register_memory is empty function");
        return nullptr;
    }

    int unregister_memory(vDMABuffer* vbuf) override {
        LOG_WARN("unregister_memory is empty function");
        return -1;
    }

private:
    // allocator_ has the mutex lock
    SharedMemoryBufferAllocator allocator_;
    static const int max_retry_ = 10000;
};


class SharedMemoryInitiator : public vDMAInitiator {
public:
    SharedMemoryInitiator(const char* shm_name, size_t shm_size)
    :
    shm_name_(shm_name), shm_size_(shm_size) 
    {
        shm_fd_ = shm_open(shm_name, O_RDWR, 0666);
        if (shm_fd_ < 0) {
            LOG_ERROR("SharedMemoryInitiator::Construct, shm_open failed");
        }
        LOG_DEBUG("SharedMemoryInitiator: ", VALUE(shm_fd_), VALUE(shm_size_));

        ftruncate(shm_fd_, shm_size_);
        shm_begin_ptr_ = (char*)mmap(NULL, shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
        if (!shm_begin_ptr_) {
            LOG_ERROR("SharedMemoryInitiator, mmap failed");
            close(shm_fd_);
        }
    }

    ~SharedMemoryInitiator() {
        SCOPED_LOCK(mutex_);
        if (buf_map_.size() > 0) {
            LOG_ERROR("SharedMemoryInitiator::Destruct, buf_map_ is not empty, some user not unmap, below force delete and unmap");
        }
        if (shm_begin_ptr_) {
            munmap(shm_begin_ptr_, shm_size_);
        }
        if (shm_fd_ >= 0) {
            close(shm_fd_);
        }
    }

    vDMABuffer* map(std::string_view id) override {
        SCOPED_LOCK(mutex_);
        auto it = buf_map_.find(id);
        if (it == buf_map_.end()) {
            SharedMemoryBuffer* buf = new SharedMemoryBuffer(id, shm_begin_ptr_, vDMABufferType::kSharedMem);
            buf_map_.emplace(id, buf);
            return buf;
        }
        LOG_ERROR_RETURN(0, nullptr, "used, map failed: ", id);
    }

    int unmap(vDMABuffer* buffer) override {
        SCOPED_LOCK(mutex_);
        auto key = buffer->id();
        int cnt = buf_map_.erase(key);
        if (cnt == 0) {
            LOG_ERROR_RETURN(0, -1, "not used, unmap failed: ", key);
        }
        return 0;
    }

    int write(vDMABuffer* vbuf, size_t size, off_t offset) override {
        LOG_WARN("write is empty function");
        return -1;
    }

    int read(vDMABuffer* vbuf, size_t size, off_t offset) override {
        LOG_WARN("read is empty function");
        return -1;
    }

private:
    std::string shm_name_;
    int shm_fd_;
    size_t shm_size_;
    char* shm_begin_ptr_;

    photon::mutex mutex_;
    unordered_map_string_key<std::unique_ptr<SharedMemoryBuffer>> buf_map_;
};

vDMATarget* new_shm_vdma_target(const char* shm_name, size_t shm_size, size_t unit) {
    return NewObj<SharedMemoryTarget>()->init(shm_name, shm_size, unit);
}

vDMAInitiator* new_shm_vdma_initiator(const char* shm_name, size_t shm_size) {
    return new SharedMemoryInitiator(shm_name, shm_size);
}

}   // namespace photon
