#pragma once

#include <photon/common/object.h>
#include <photon/common/string_view.h>
#include <unistd.h>

namespace photon {

enum vDMABufferType {
    kSharedMem
};

class vDMABuffer {
public:
    // transportable identity for buffer
    // both initialtor and target able to translate id to `real` address
    // id haven't to be a visible string
    // just simple binary serialized address
    virtual std::string_view id() const = 0;

    // `real` address of buffer (memory pointer)
    virtual void* address() const = 0;

    // size of buffer
    virtual size_t buf_size() const = 0;

    // vDMABuffer type
    virtual int type_code() const = 0;

    // check if buffer is registered (not allocated)
    virtual bool is_registered() const = 0;

    // check if buffer is valid
    virtual bool is_valid() const = 0;

protected:
    // vDMABuffer cannot be destroyed using `delete` by user
    // vDMABuffer's Instance will be free by Target and Initiator
    virtual ~vDMABuffer() {}
};

class vDMATarget : public Object {
public:
    /// alloc vDMABuffer, result in `buf`
    /// nullptr as failure
    virtual vDMABuffer* alloc(size_t size) = 0;

    // dealloc vDMABuffer `buf`
    // 0 for success, non-zero for failure
    virtual int dealloc(vDMABuffer* buf) = 0;

    // Make allocated memory as vDMA blocks
    // maybe not implemented in some certian targets
    // nullptr as failure
    virtual vDMABuffer* register_memory(void* buf, size_t size) = 0;

    // Unregister vDMABuffer, which is registered by register_memory
    // maybe not implemented in some certian targets
    // 0 for success, non-zero for failure
    virtual int unregister_memory(vDMABuffer* vbuf) = 0;
};

class vDMAInitiator : public Object {
public:
    // map id to vDMABuffer
    virtual vDMABuffer* map(std::string_view id) = 0;

    // unmap vDMABuffer
    virtual int unmap(vDMABuffer* buffer) = 0;

    // write vDMABuffer to target
    // should check if vDMABuffer is valid
    virtual int write(vDMABuffer* vbuf, size_t size, off_t offset) = 0;

    // read vDMABuffer from target
    // should check if vDMABuffer is valid
    virtual int read(vDMABuffer* vbuf, size_t size, off_t offset) = 0;
};

vDMATarget* new_shm_vdma_target(const char* shm_name, size_t shm_size, size_t unit);
vDMAInitiator* new_shm_vdma_initiator(const char* shm_name, size_t shm_size);

}  // namespace photon