#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/io/vdma.h>


#include <string>

int main() {
    photon::init();
    DEFER(photon::fini());

    // transfer procedure simple simulation
    std::string shm_name = "foo";
    size_t shm_size = 65536;    // 64KB
    size_t unit = 4096;         // 4KB
    // (1) create a target (want data)
    // map shm to local addr space, and new a allocator to manage it
    auto target = photon::new_shm_vdma_target(shm_name.c_str(), shm_size, unit);
    // (2) create a initiator (has data)
    auto initiator = photon::new_shm_vdma_initiator(shm_name.c_str());
    // (3) target alloc a shared memory buffer
    auto t_buffer = target->alloc(unit);
    auto physical_addr = std::to_string((uint64_t)t_buffer->physical_address());
    auto logical_addr = t_buffer->logical_address();
    LOG_INFO("step3: shared memory buffer logical addr is ", logical_addr.data(),
             ", physical addr is ", physical_addr.data());
    size_t want_data_size = 4096;
    off_t want_data_offset = 0;
    // (4) target send a request msg to initiator, tell initiator: logical addr + want which data
    // (5) initiator use the logical addr map shm to its local addr space
    auto i_buffer = initiator->map(logical_addr);
    // (6) initiator copy corresponding data
    char thedata[4096];
    memset(thedata, 0x22, 4096);
    memcpy(i_buffer->physical_address() + want_data_offset, thedata, want_data_size);
    // (7) initiator single-side write
    initiator->write(i_buffer, want_data_size, want_data_offset);
    // (8) initiator unmap the shm buffer
    initiator->unmap(i_buffer);
    // (9) initiator send a response msg to target, tell target: transfer done
    // (10) target check data
    uint8_t* ptr = (uint8_t*)(t_buffer->physical_address() + want_data_offset);
    for (size_t i=0; i<want_data_size; i++) {
        if (ptr[i] != 0x22) {
            LOG_ERROR("failed, not same at ", i, "want ", 0x22, ", now ", ptr[i]);
            target->dealloc(t_buffer);
            return -1;
        }
    }
    // (11) target dealloc the shm buffer
    target->dealloc(t_buffer);
    LOG_INFO("success");
}