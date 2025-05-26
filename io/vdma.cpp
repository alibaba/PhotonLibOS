#include "vdma.h"

#include <photon/common/alog.h>
#include <photon/thread/thread.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string>
#include <vector>

namespace photon {

enum vDMABufferType {
    kSharedMem
};

class vDMABufferImpl : public vDMABuffer {
public:
    vDMABufferImpl(int idx, char* begin_ptr, size_t buffer_size, int type)
    :
    idx_(idx), begin_ptr_(begin_ptr), buffer_size_(buffer_size), type_(type)
    {
        logic_addr_.assign(std::to_string(idx_) + "_" + std::to_string(buffer_size_));
    }

    ~vDMABufferImpl() {}

    bool is_registered() const override {
        LOG_WARN("is_registered is empty function");
        return true; 
    }

    bool is_valid() const override { 
        LOG_WARN("is_valid is empty function");
        return true; 
    }

    std::string_view logical_address() const override {
        return logic_addr_;
    }

    char* physical_address() const override {
        return begin_ptr_;
    }

    size_t buf_size() const override {
        return buffer_size_;
    }

    int type_code() const override {
        return type_;
    }

    int idx() const { return idx_; }

    static int parse_logical_address(std::string_view logical_address, int* idx, size_t* buffer_size) {
        size_t pos = logical_address.find("_");
        if (pos == logical_address.npos) {
            LOG_ERROR("vDMABufferImpl::parse_logical_address, failed");
            return -1;
        }
        *idx = std::stoi(logical_address.substr(0, pos).data());
        *buffer_size = std::stoul(logical_address.substr(pos+1, logical_address.size()-pos-1).data());
        return 0;
    }

private:
    int idx_;
    char* begin_ptr_;
    size_t buffer_size_;
    int type_;
    std::string logic_addr_;
};

class vDMABufferAllocator {
public:
    vDMABufferAllocator(std::string shm_name, size_t shm_size, size_t unit)
    :
    shm_name_(shm_name), unit_(unit)
    {
        shm_fd_ = shm_open(shm_name.c_str(), O_RDWR | O_CREAT, 0666);
        if (shm_fd_ < 0) {
            LOG_ERROR("vDMABufferAllocator::Construct, shm_open failed");
            return;
        }
        shm_size_ = (shm_size + unit_ - 1) / unit_ * unit_;
        LOG_INFO("vDMABufferAllocator: shm_fd=", shm_fd_, ", shm_size=", shm_size_, ", unit=", unit_);

        ftruncate(shm_fd_, shm_size_);
        shm_begin_ptr_ = (char*)mmap(NULL, shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
        if (!shm_begin_ptr_) {
            LOG_ERROR("vDMABufferAllocator, mmap failed");
        }

        nbuffer_ = shm_size_ / unit_;
        used_mark_.resize(nbuffer_);
        buffers_.resize(nbuffer_);
        LOG_INFO("vDMABufferAllocator: shm_begin_ptr=", shm_begin_ptr_, ", nbuffer=", nbuffer_);
        LOG_INFO("vDMABufferAllocator: used_mark.size=", used_mark_.size(), ", buffers.size=", buffers_.size());
        
        for (int i=0; i<nbuffer_; i++) {
            buffers_[i] = new vDMABufferImpl(i, shm_begin_ptr_ + i * unit_, unit_, vDMABufferType::kSharedMem);
        }
        LOG_INFO("vDMABufferAllocator: complete");
    }

    ~vDMABufferAllocator() {
        SCOPED_LOCK(mutex_);
        for (int i=0; i<nbuffer_; i++) {
            vDMABufferImpl* buf = buffers_[i];
            if (buf) delete buf;
        }
    }

    vDMABuffer* alloc_one() {
        SCOPED_LOCK(mutex_);
        for (int i=0; i<nbuffer_; i++) {
            if (!used_mark_[i]) {
                used_mark_[i] = true;
                return buffers_[i];
            }
        }
        return nullptr;
    }

    int free_one(vDMABuffer* buf) {
        SCOPED_LOCK(mutex_);
        used_mark_[reinterpret_cast<vDMABufferImpl*>(buf)->idx()] = false;
        return 0;
    }

    size_t unit() const { return unit_; }
    
private:
    std::string shm_name_;
    int shm_fd_;
    size_t shm_size_;
    char* shm_begin_ptr_;
    
    size_t unit_;
    int nbuffer_;
    const int max_retry_ = 10;

    photon::mutex mutex_;
    std::vector<vDMABufferImpl*> buffers_;
    std::vector<bool> used_mark_;
};

class vDMATargetImpl : public vDMATarget {
public:
    vDMATargetImpl(std::string shm_name, size_t shm_size, size_t unit) {
        allocator_ = new vDMABufferAllocator(shm_name, shm_size, unit);
    }

    ~vDMATargetImpl() {
        if (allocator_) {
            delete allocator_;
        }
    }

    vDMABuffer* alloc(size_t size) override {
        if (size != allocator_->unit()) {
            LOG_ERROR("current allocator only support ", allocator_->unit(), ", you ", size);
            return nullptr;
        }

        vDMABuffer* buf = nullptr;
        buf = allocator_->alloc_one();

        int retry_count = 0;
        while (!buf && retry_count < max_retry_) {
            thread_yield();
            buf = allocator_->alloc_one();
            retry_count++;
        }

        return buf;
    }

    int dealloc(vDMABuffer* buf) override {
        return allocator_->free_one(buf);
    }

    vDMABuffer* register_memory(char* buf, size_t size) override {
        LOG_WARN("register_memory is empty function");
        return nullptr;
    }

    int unregister_memory(vDMABuffer* vbuf) override {
        LOG_WARN("unregister_memory is empty function");
        return -1;
    }

private:
    vDMABufferAllocator* allocator_;
    const int max_retry_ = 10000;
};


class vDMAInitiatorImpl : public vDMAInitiator {
public:
    vDMAInitiatorImpl(std::string shm_name) : shm_name_(shm_name) {
        shm_fd_ = shm_open(shm_name.c_str(), O_RDWR, 0666);
        if (shm_fd_ < 0) {
            LOG_ERROR("vDMAInitiatorImpl::Construct, shm_open failed");
        }
    }

    vDMABuffer* map(std::string_view logical_address) override {
        int idx = -1;
        size_t buffer_size = 0;
        if (vDMABufferImpl::parse_logical_address(logical_address, &idx, &buffer_size) < 0 || idx < 0 || buffer_size == 0) {
            LOG_ERROR("vDMAInitiatorImpl::map, parse logical address failed");
            return nullptr;
        }

        off_t offset = idx * buffer_size;
        char* ptr = (char*)mmap(nullptr, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, offset);
        if (!ptr) {
            LOG_ERROR("vDMAInitiatorImpl::map, mmap failed");
            return nullptr;
        }
        
        return new vDMABufferImpl(idx, ptr, buffer_size, vDMABufferType::kSharedMem);
    }

    int unmap(vDMABuffer* buffer) override {
        munmap(buffer->physical_address(), buffer->buf_size());
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
};

vDMATarget* new_shm_vdma_target(const char* shm_name, size_t shm_size, size_t unit) {
    return new vDMATargetImpl(shm_name, shm_size, unit);
}

vDMAInitiator* new_shm_vdma_initiator(const char* shm_name) {
    return new vDMAInitiatorImpl(shm_name);
}

}   // namespace photon
