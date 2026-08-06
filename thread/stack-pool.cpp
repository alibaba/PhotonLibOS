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

#if defined(__linux__)
#include <linux/mman.h>
#endif
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <new>
#include <utility>

#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/thread/arch.h>
#include <photon/thread/stack-allocator.h>
#include <photon/thread/thread.h>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

namespace photon {

// The global pooled stack allocator is a three-layer magazine/depot design:
//   L0  per-vcpu magazine     pointer arrays, no atomics, cache-hot   (fast path)
//   L1  global depot          per-size-class, spinlock protected      (batching)
//   L2  OS                    per-stack mmap / munmap                 (cold path)
//
// The free path never yields and runs from photon thread `dispose()` in the
// die-defer context, so it must not use photon::mutex; the depot uses
// photon::spinlock (pure spin, safe there) and plain atomics only.
class GlobalStackPool {
public:
    static constexpr uint32_t MAX_CLASSES = 8;
    static constexpr uint32_t PASSTHROUGH = ~0u;
    // Stacks larger than this are not pooled: mmap/munmap on every op.
    static constexpr size_t MAX_CLASS_SIZE = 256ULL * 1024 * 1024;
    static constexpr uint32_t ST_FREE = 0x46524545u;    // 'FREE'
    static constexpr uint32_t ST_INUSE = 0x494e5553u;   // 'INUS'

    // Metadata page sits at `stack_ptr + stack_size`, sharing protection with
    // the stack so the two merge into a single VMA (2 VMAs per block total).
    struct BlockMeta {
        uint64_t magic;        // g_magic_seed ^ (uintptr_t)stack_ptr
        uint32_t state;        // ST_FREE / ST_INUSE
        uint32_t class_idx;    // index into classes[], or PASSTHROUGH
        size_t   size;         // authoritative stack size (what photon requested)
        uint64_t seq;          // last alloc/free sequence number, for diagnostics
        uint64_t free_ts;      // photon::now at last free, for diagnostics
        void*    mmap_base;    // start of the whole mapping (may be below stack_ptr)
        size_t   mmap_len;     // length of the whole mapping
        void*    link;         // pending / cold / leak chain
    };

    // A batch of block pointers moved between L0 and L1 in one lock acquisition.
    struct Magazine {
        Magazine* next;
        uint32_t  cap;
        uint32_t  n;
        void*     blk[];
    };

    struct SizeClass {
        size_t     block_size = 0;       // == stack size of this class
        uint32_t   cap = 0;              // magazine capacity K
        spinlock   lock;
        Magazine*  full = nullptr;       // stack of full magazines (resident/pooled)
        Magazine*  empty = nullptr;      // recycled empty magazine structs
        void*      pending = nullptr;    // overflow blocks, resident, awaiting reclaim
        void*      cold = nullptr;       // madvise'd blocks, still mapped
        size_t     pooled_bytes = 0;
        size_t     pending_bytes = 0;
        size_t     cold_bytes = 0;
        size_t     live_bytes = 0;
        uint64_t   os_maps = 0;
        uint64_t   os_unmaps = 0;
        uint64_t   corruptions = 0;
    };

    // Per-vcpu (per-OS-thread) magazine cache. Immortal blocks are avoided; the
    // struct itself is malloc'd and freed by the pthread_key destructor, which
    // first drains its magazines back into the depot.
    struct PerVCPU {
        Magazine* loaded[MAX_CLASSES] = {};
        Magazine* prev[MAX_CLASSES] = {};
        // Empty magazine pre-allocated outside the class lock (per class), so the
        // depot never mallocs while holding the spinlock. Owned by this thread.
        Magazine* spare[MAX_CLASSES] = {};
        PerVCPU*  reg_next = nullptr;    // registry intrusive list
        size_t    last_size = 0;         // inline size->class cache
        uint32_t  last_ci = PASSTHROUGH;
        // Written locklessly by the owning OS thread, read by stats() from
        // another thread -> atomic (relaxed; the values are diagnostic).
        std::atomic<uint64_t> hits{0};
        std::atomic<uint64_t> misses{0};
    };

    // ---- global state -----------------------------------------------------
    GlobalStackPoolOptions opt;
    spinlock       reg_lock;             // guards classes[]/n and the PerVCPU list
    SizeClass      classes[MAX_CLASSES];
    // Published with release after a class's fields are set, so lockless
    // readers never observe a half-initialized slot.
    std::atomic<uint32_t> n_classes{0};
    PerVCPU*       vcpu_list = nullptr;
    uint64_t       magic_seed = 0;
    pthread_key_t  key = 0;
    std::atomic<uint64_t> total_mapped{0};
    std::atomic<uint64_t> seq{0};
    std::atomic<uintptr_t> range_lo{~uintptr_t(0)};
    std::atomic<uintptr_t> range_hi{0};
    // Passthrough (oversized, unpooled) accounting, folded into stats().
    std::atomic<uint64_t> pt_maps{0};
    std::atomic<uint64_t> pt_unmaps{0};
    std::atomic<uint64_t> pt_live{0};

    // ---- low level OS helpers --------------------------------------------
    static void decommit(void* addr, size_t len) {
#if defined(__APPLE__)
        madvise(addr, len, MADV_FREE);
#else
        madvise(addr, len, MADV_DONTNEED);
#endif
    }

    size_t guard_len() const {
        // The first guard page is page 0 of the stack region itself; any extra
        // guard pages are prepended below the returned pointer.
        return (size_t)(opt.guard_pages ? opt.guard_pages - 1 : 0) * PAGE_SIZE;
    }

    // Map one fresh block for a class of `size`. Returns the pointer handed to
    // photon (first page is the guard), or nullptr with errno set.
    //
    // A stack allocation failure is catastrophic for a coroutine, so this never
    // fails preemptively: if the OS refuses the mapping, it dumps the entire
    // idle cache (cold + pending) back to the OS and retries, giving up only
    // when the OS still cannot satisfy the request.
    void* map_block(uint32_t ci, size_t size) {
        size_t extra = guard_len();
        size_t len = extra + size + PAGE_SIZE;   // extra guards + stack + meta
        void* base = mmap(nullptr, len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (base == MAP_FAILED) {
            // Sacrifice the entire idle cache (pooled + pending + cold) to
            // satisfy this live allocation, then retry once.
            trim(0);
            base = mmap(nullptr, len, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (base == MAP_FAILED)
                LOG_ERRNO_RETURN(0, nullptr, "mmap for photon stack failed, ",
                                 VALUE(len));
        }
        total_mapped.fetch_add(len, std::memory_order_relaxed);
        char* stack_ptr = (char*)base + extra;
        // Guard: all guard pages (extra + page 0 of the stack) as one PROT_NONE run.
        mprotect(base, extra + PAGE_SIZE, PROT_NONE);
#if defined(__linux__)
        if (opt.no_huge_page)
            madvise(stack_ptr, size, MADV_NOHUGEPAGE);
#endif
        auto m = meta_of(stack_ptr, size);
        m->magic = magic_seed ^ (uintptr_t)stack_ptr;
        m->state = ST_INUSE;
        m->class_idx = ci;
        m->size = size;
        m->seq = seq.fetch_add(1, std::memory_order_relaxed);
        m->free_ts = 0;
        m->mmap_base = base;
        m->mmap_len = len;
        m->link = nullptr;
        // Publish the address range for the dealloc range gate.
        bump_min(range_lo, (uintptr_t)base);
        bump_max(range_hi, (uintptr_t)base + len);
        if (ci != PASSTHROUGH) {
            SCOPED_LOCK(classes[ci].lock);
            classes[ci].os_maps++;
        } else {
            pt_maps.fetch_add(1, std::memory_order_relaxed);
        }
        return stack_ptr;
    }

    // Unmap one block completely, returning its bytes to the OS.
    void unmap_block(void* stack_ptr, size_t size) {
        auto m = meta_of(stack_ptr, size);
        void* base = m->mmap_base;
        size_t len = m->mmap_len;
        munmap(base, len);
        total_mapped.fetch_sub(len, std::memory_order_relaxed);
    }

    BlockMeta* meta_of(void* stack_ptr, size_t size) const {
        return (BlockMeta*)((char*)stack_ptr + size);
    }

    static void bump_min(std::atomic<uintptr_t>& a, uintptr_t v) {
        uintptr_t cur = a.load(std::memory_order_relaxed);
        while (v < cur && !a.compare_exchange_weak(cur, v,
                std::memory_order_relaxed)) {}
    }
    static void bump_max(std::atomic<uintptr_t>& a, uintptr_t v) {
        uintptr_t cur = a.load(std::memory_order_relaxed);
        while (v > cur && !a.compare_exchange_weak(cur, v,
                std::memory_order_relaxed)) {}
    }

    // ---- size class resolution -------------------------------------------
    // Find or register the exact-size class for `size` (already page-aligned).
    uint32_t get_class(PerVCPU* pv, size_t size) {
        if (pv && pv->last_size == size) return pv->last_ci;
        uint32_t ci = PASSTHROUGH;
        SCOPED_LOCK(reg_lock);
        uint32_t nc = n_classes.load(std::memory_order_relaxed);   // reg_lock held
        for (uint32_t i = 0; i < nc; i++) {
            if (classes[i].block_size == size) { ci = i; break; }
        }
        if (ci == PASSTHROUGH && size <= MAX_CLASS_SIZE && nc < MAX_CLASSES) {
            auto& c = classes[nc];
            c.block_size = size;
            size_t k = opt.per_vcpu_cache_bytes / size / 2;
            if (k < 1) k = 1;
            if (k > 64) k = 64;
            c.cap = (uint32_t)k;
            // Publish the class only after its fields are set, so lockless
            // readers of n_classes never observe a half-built slot.
            n_classes.store(nc + 1, std::memory_order_release);
            ci = nc;
        }
        if (pv) { pv->last_size = size; pv->last_ci = ci; }
        return ci;
    }

    // ---- magazine helpers ------------------------------------------------
    // Allocate a bare empty magazine. Must be called OUTSIDE the class lock;
    // the free path never calls this.
    static Magazine* raw_alloc_mag(uint32_t cap) {
        auto m = (Magazine*)malloc(sizeof(Magazine) + (size_t)cap * sizeof(void*));
        if (m) { m->cap = cap; m->n = 0; }
        return m;
    }
    void mag_put_empty(SizeClass& c, Magazine* m) {
        m->n = 0;
        m->next = c.empty;
        c.empty = m;
    }
    // Pop a recycled empty magazine without allocating; nullptr if none. Safe
    // to call on the free path (never mallocs).
    Magazine* mag_try_recycle(SizeClass& c) {
        if (!c.empty) return nullptr;
        auto m = c.empty;
        c.empty = m->next;
        m->n = 0;
        return m;
    }

    // Spill every block of a full magazine into the pending chain (no syscall),
    // then recycle the magazine struct. Applies back-pressure afterwards.
    void spill_to_pending(SizeClass& c, Magazine* m) {
        for (uint32_t i = 0; i < m->n; i++) {
            auto meta = (BlockMeta*)((char*)m->blk[i] + c.block_size);
            meta->link = c.pending;
            c.pending = m->blk[i];
            c.pending_bytes += c.block_size;
        }
        m->n = 0;
        mag_put_empty(c, m);
        apply_back_pressure(c);
    }

    // Free path back-pressure: keep resident-but-freed memory bounded. This is
    // the only place the free path may issue a syscall.
    void apply_back_pressure(SizeClass& c) {
        while (c.pending_bytes > opt.max_pending_bytes && c.pending) {
            void* b = c.pending;
            auto meta = (BlockMeta*)((char*)b + c.block_size);
            c.pending = meta->link;
            c.pending_bytes -= c.block_size;
            if (c.cold_bytes + c.block_size <= opt.max_cold_bytes) {
                decommit(b, c.block_size);
                meta->link = c.cold;
                c.cold = b;
                c.cold_bytes += c.block_size;
            } else {
                unmap_block(b, c.block_size);
                c.os_unmaps++;
            }
        }
    }

    // ---- depot block-granular slow path (called with class lock held) ----
    void* depot_take_block(SizeClass& c) {
        if (c.pending) {
            void* b = c.pending;
            auto meta = (BlockMeta*)((char*)b + c.block_size);
            c.pending = meta->link;
            c.pending_bytes -= c.block_size;
            return b;
        }
        if (c.cold) {
            void* b = c.cold;
            auto meta = (BlockMeta*)((char*)b + c.block_size);
            c.cold = meta->link;
            c.cold_bytes -= c.block_size;
            return b;   // pages will be faulted back in on use
        }
        return nullptr;
    }

    // ---- global alloc / dealloc ------------------------------------------
    void* alloc(size_t size) {
        auto pv = get_pervcpu();
        uint32_t ci = get_class(pv, size);
        if (ci == PASSTHROUGH) {
            void* b = map_block(PASSTHROUGH, size);
            return b ? prepare(b, size, PASSTHROUGH) : nullptr;
        }
        auto& c = classes[ci];
        // L0: per-vcpu magazine, no lock.
        if (pv) {
            Magazine* m = pv->loaded[ci];
            if (m && m->n) { pv->hits.fetch_add(1, std::memory_order_relaxed); return prepare(m->blk[--m->n], size, ci); }
            Magazine* p = pv->prev[ci];
            if (p && p->n) {
                std::swap(pv->loaded[ci], pv->prev[ci]);
                pv->hits.fetch_add(1, std::memory_order_relaxed);
                m = pv->loaded[ci];
                return prepare(m->blk[--m->n], size, ci);
            }
        }
        // L1: depot. Pre-allocate a spare empty magazine OUTSIDE the lock so the
        // depot never mallocs while holding the spinlock; it stays in the
        // per-vcpu slot for reuse when this call does not consume it.
        if (pv && !pv->spare[ci]) pv->spare[ci] = raw_alloc_mag(c.cap);
        void* blk = nullptr;
        {
            SCOPED_LOCK(c.lock);
            if (pv) {
                Magazine* full = depot_get_full(c, &pv->spare[ci]);
                if (full) {
                    if (pv->loaded[ci]) mag_put_empty(c, pv->loaded[ci]);
                    pv->loaded[ci] = full;
                    pv->hits.fetch_add(1, std::memory_order_relaxed);
                    blk = full->blk[--full->n];
                }
            }
            if (!blk) {
                if (pv) pv->misses.fetch_add(1, std::memory_order_relaxed);
                blk = depot_take_block(c);   // steal pending/cold (no syscall)
            }
        }
        if (blk) return prepare(blk, size, ci);
        // L2: map a fresh block (may issue syscalls / hit the quota).
        blk = map_block(ci, size);
        if (!blk) return nullptr;
        return prepare(blk, size, ci);
    }

    void dealloc(void* stack_ptr, size_t size) {
        // Range gate: reject foreign pointers before dereferencing metadata.
        uintptr_t p = (uintptr_t)stack_ptr;
        if (p < range_lo.load(std::memory_order_relaxed) ||
            p >= range_hi.load(std::memory_order_relaxed)) {
            LOG_FATAL("global stack pool: foreign pointer ` freed", stack_ptr);
            return;
        }
        auto m = meta_of(stack_ptr, size);
        if (m->magic != (magic_seed ^ (uintptr_t)stack_ptr) ||
            m->state != ST_INUSE || m->size != size) {
            // Double-free or corruption: leak the block instead of recycling
            // it, so a poisoned buffer never becomes another thread's stack.
            report_corruption(m, stack_ptr);
            return;
        }
        uint32_t ci = m->class_idx;
        m->state = ST_FREE;
        m->seq = seq.fetch_add(1, std::memory_order_relaxed);
        m->free_ts = m->seq;   // monotonic free marker (diagnostic; avoids racing on photon::now)
        if (ci == PASSTHROUGH) {
            pt_live.fetch_sub(size, std::memory_order_relaxed);
            pt_unmaps.fetch_add(1, std::memory_order_relaxed);
            unmap_block(stack_ptr, size);
            return;
        }
        auto& c = classes[ci];
        note_live(size, -1, ci);
        if (opt.wipe_bytes) wipe(stack_ptr, size);
        if (opt.paranoid) mprotect((char*)stack_ptr + PAGE_SIZE,
                                   size - PAGE_SIZE, PROT_NONE);
        // Free path runs in the die-defer context: never allocate here. Use an
        // existing per-vcpu cache (do not create one) and never malloc under
        // the class lock -- if no recycled magazine is available, stash the
        // block loose in pending; the alloc slow path repopulates magazines.
        auto pv = (PerVCPU*)pthread_getspecific(key);
        if (pv) {
            Magazine* m0 = pv->loaded[ci];
            if (m0 && m0->n < m0->cap) { m0->blk[m0->n++] = stack_ptr; return; }
            Magazine* p0 = pv->prev[ci];
            if (p0 && p0->n < p0->cap) {
                std::swap(pv->loaded[ci], pv->prev[ci]);
                m0 = pv->loaded[ci];
                m0->blk[m0->n++] = stack_ptr;
                return;
            }
        }
        // Loaded magazine full (or no per-vcpu cache): go to the depot.
        SCOPED_LOCK(c.lock);
        if (pv) {
            Magazine* e = mag_try_recycle(c);   // recycle only, never malloc
            if (e) {
                if (pv->loaded[ci]) depot_put_full(c, pv->loaded[ci]);
                pv->loaded[ci] = e;
                e->blk[e->n++] = stack_ptr;
                return;
            }
        }
        // No recycled magazine available: hand the single block to the depot.
        auto meta = (BlockMeta*)((char*)stack_ptr + c.block_size);
        meta->link = c.pending;
        c.pending = stack_ptr;
        c.pending_bytes += c.block_size;
        apply_back_pressure(c);
    }

    // Take a full magazine from the depot, or build one from pending blocks so
    // that batching resumes after a free burst. Returns nullptr if neither.
    // `spare` is a caller-provided empty magazine (allocated outside the lock)
    // used only when a magazine must be built and none is recycled; if it is
    // consumed, *spare is set to null.
    Magazine* depot_get_full(SizeClass& c, Magazine** spare) {
        if (c.full) {
            auto m = c.full;
            c.full = m->next;
            c.pooled_bytes -= (size_t)m->n * c.block_size;
            return m;
        }
        if (c.pending_bytes >= (size_t)c.cap * c.block_size) {
            Magazine* m = mag_try_recycle(c);
            if (!m && spare && *spare) { m = *spare; *spare = nullptr; m->n = 0; }
            if (m) {
                while (m->n < m->cap && c.pending) {
                    void* b = c.pending;
                    auto meta = (BlockMeta*)((char*)b + c.block_size);
                    c.pending = meta->link;
                    c.pending_bytes -= c.block_size;
                    m->blk[m->n++] = b;
                }
                return m;
            }
        }
        return nullptr;
    }

    // Accept a full magazine. If the resident cache is over budget, spill the
    // blocks to pending (no syscall) instead of keeping them resident.
    void depot_put_full(SizeClass& c, Magazine* m) {
        size_t bytes = (size_t)m->n * c.block_size;
        if (c.pooled_bytes + bytes <= opt.max_pooled_bytes) {
            m->next = c.full;
            c.full = m;
            c.pooled_bytes += bytes;
            return;
        }
        spill_to_pending(c, m);
    }

    // ---- diagnostics / accounting ----------------------------------------
    void* prepare(void* stack_ptr, size_t size, uint32_t ci) {
        auto m = meta_of(stack_ptr, size);
        m->state = ST_INUSE;
        m->seq = seq.fetch_add(1, std::memory_order_relaxed);
        if (ci == PASSTHROUGH) {
            pt_live.fetch_add(size, std::memory_order_relaxed);
            return stack_ptr;
        }
        if (opt.paranoid)
            mprotect((char*)stack_ptr + PAGE_SIZE, size - PAGE_SIZE,
                     PROT_READ | PROT_WRITE);
        note_live(size, +1, ci);
        return stack_ptr;
    }

    void wipe(void* stack_ptr, size_t size) {
        size_t w = opt.wipe_bytes;
        if (w > size - PAGE_SIZE) w = size - PAGE_SIZE;
        // Top of the stack (high addresses) holds the most sensitive state.
        memset((char*)stack_ptr + size - w, 0, w);
    }

    void note_live(size_t size, int delta, uint32_t ci) {
        if (ci == PASSTHROUGH) return;
        auto& c = classes[ci];
        SCOPED_LOCK(c.lock);
        if (delta > 0) c.live_bytes += size;
        else c.live_bytes -= size;
    }

    void report_corruption(BlockMeta* m, void* stack_ptr) {
        // Best-effort class attribution; the metadata may be untrustworthy.
        if (m->class_idx < n_classes.load(std::memory_order_acquire)) {
            SCOPED_LOCK(classes[m->class_idx].lock);
            classes[m->class_idx].corruptions++;
        }
        LOG_FATAL("global stack pool: double-free or corrupted metadata at `, "
                  "leaking the block", stack_ptr);
        // The block is intentionally leaked so a poisoned buffer is never
        // handed out again. Abort only in paranoid mode (bug hunting).
        if (opt.paranoid) abort();
    }

    // ---- reclaim / trim ---------------------------------------------------
    // Return up to `target` bytes to the OS, cold first then pending. Returns
    // the bytes actually munmap'd. Runs from alloc-slow-path or trim, never
    // from the free path.
    size_t reclaim_to_os(size_t target) {
        size_t freed = 0;
        uint32_t nc = n_classes.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < nc && freed < target; i++) {
            auto& c = classes[i];
            SCOPED_LOCK(c.lock);
            while (c.cold && freed < target) {
                void* b = c.cold;
                auto meta = (BlockMeta*)((char*)b + c.block_size);
                c.cold = meta->link;
                c.cold_bytes -= c.block_size;
                size_t mlen = meta->mmap_len;   // capture before the page is unmapped
                unmap_block(b, c.block_size);
                c.os_unmaps++;
                freed += mlen;
            }
            while (c.pending && freed < target) {
                void* b = c.pending;
                auto meta = (BlockMeta*)((char*)b + c.block_size);
                c.pending = meta->link;
                c.pending_bytes -= c.block_size;
                size_t mlen = meta->mmap_len;   // capture before the page is unmapped
                unmap_block(b, c.block_size);
                c.os_unmaps++;
                freed += mlen;
            }
        }
        return freed;
    }

    size_t trim(size_t keep_bytes) {
        // Retained = pooled + pending + cold. Free full magazines to pending
        // first, then hand everything above keep_bytes back to the OS.
        size_t freed = 0;
        uint32_t nc = n_classes.load(std::memory_order_acquire);
        // Drain the calling thread's own magazines into the depot; otherwise
        // blocks cached there would be unreachable by trim. Other vcpus' hot
        // magazines are left alone (they are actively in use and bounded).
        if (auto pv = (PerVCPU*)pthread_getspecific(key)) {
            for (uint32_t ci = 0; ci < nc; ci++) {
                drain_one(classes[ci], pv->loaded[ci]);
                drain_one(classes[ci], pv->prev[ci]);
                pv->loaded[ci] = pv->prev[ci] = nullptr;
            }
        }
        size_t retained = 0;
        for (uint32_t i = 0; i < nc; i++) {
            auto& c = classes[i];
            SCOPED_LOCK(c.lock);
            // Drain full magazines into loose blocks so they can be unmapped.
            while (c.full) {
                auto m = c.full;
                c.full = m->next;
                c.pooled_bytes -= (size_t)m->n * c.block_size;
                for (uint32_t j = 0; j < m->n; j++) {
                    void* b = m->blk[j];
                    auto meta = (BlockMeta*)((char*)b + c.block_size);
                    meta->link = c.pending;
                    c.pending = b;
                    c.pending_bytes += c.block_size;
                }
                mag_put_empty(c, m);
            }
            retained += c.pending_bytes + c.cold_bytes;   // read under the class lock
        }
        // Unmap the excess (reclaim_to_os re-locks each class).
        if (retained > keep_bytes)
            freed = reclaim_to_os(retained - keep_bytes);
        return freed;
    }

    GlobalStackPoolStats stats() {
        GlobalStackPoolStats s = {};
        s.mapped_bytes = total_mapped.load(std::memory_order_relaxed);
        SCOPED_LOCK(reg_lock);
        uint32_t nc = n_classes.load(std::memory_order_relaxed);   // reg_lock held
        for (uint32_t i = 0; i < nc; i++) {
            SCOPED_LOCK(classes[i].lock);
            s.pooled_bytes += classes[i].pooled_bytes;
            s.pending_bytes += classes[i].pending_bytes;
            s.cold_bytes += classes[i].cold_bytes;
            s.live_bytes += classes[i].live_bytes;
            s.os_maps += classes[i].os_maps;
            s.os_unmaps += classes[i].os_unmaps;
            s.corruptions += classes[i].corruptions;
        }
        for (PerVCPU* v = vcpu_list; v; v = v->reg_next) {
            s.hits += v->hits.load(std::memory_order_relaxed);
            s.misses += v->misses.load(std::memory_order_relaxed);
        }
        // Fold in the unpooled (passthrough) accounting.
        s.os_maps += pt_maps.load(std::memory_order_relaxed);
        s.os_unmaps += pt_unmaps.load(std::memory_order_relaxed);
        s.live_bytes += pt_live.load(std::memory_order_relaxed);
        return s;
    }

    // ---- per-vcpu lifecycle ----------------------------------------------
    PerVCPU* get_pervcpu() {
        auto pv = (PerVCPU*)pthread_getspecific(key);
        if (likely(pv)) return pv;
        // Constructed (not calloc'd) so the atomic members are properly built.
        pv = new (std::nothrow) PerVCPU();
        if (!pv) return nullptr;
        if (pthread_setspecific(key, pv) != 0) { delete pv; return nullptr; }
        SCOPED_LOCK(reg_lock);
        pv->reg_next = vcpu_list;
        vcpu_list = pv;
        return pv;
    }

    // Drain a vcpu's magazines back to the depot and unlink it. Called by the
    // pthread_key destructor at OS-thread exit.
    void drain_pervcpu(PerVCPU* pv) {
        uint32_t nc = n_classes.load(std::memory_order_acquire);
        for (uint32_t ci = 0; ci < nc; ci++) {
            drain_one(classes[ci], pv->loaded[ci]);
            drain_one(classes[ci], pv->prev[ci]);
            pv->loaded[ci] = pv->prev[ci] = nullptr;
            free(pv->spare[ci]);            // bare empty magazine, owned by pv
            pv->spare[ci] = nullptr;
        }
        SCOPED_LOCK(reg_lock);
        for (PerVCPU** pp = &vcpu_list; *pp; pp = &(*pp)->reg_next) {
            if (*pp == pv) { *pp = pv->reg_next; break; }
        }
    }
    void drain_one(SizeClass& c, Magazine* m) {
        if (!m) return;
        SCOPED_LOCK(c.lock);
        for (uint32_t i = 0; i < m->n; i++) {
            void* b = m->blk[i];
            auto meta = (BlockMeta*)((char*)b + c.block_size);
            meta->link = c.pending;
            c.pending = b;
            c.pending_bytes += c.block_size;
        }
        m->n = 0;
        mag_put_empty(c, m);
        apply_back_pressure(c);
    }

    // ---- fork safety ------------------------------------------------------
    // fork() is not compatible with multiple vcpus: the child inherits the
    // memory of every vcpu but only the calling thread, so photon threads on
    // the other vcpus are gone while their state (and whatever they owned)
    // remains. That is unfixable at the allocator level, so no attempt is made
    // to preserve the cached blocks of the vanished threads -- they are simply
    // leaked in the child, which either exec()s or must not reuse the inherited
    // pool. All these handlers do is guarantee that no depot lock is held
    // across fork(), so the child never inherits a locked spinlock.
    void atfork_prepare() {
        reg_lock.lock();
        uint32_t nc = n_classes.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < nc; i++) classes[i].lock.lock();
    }
    void atfork_parent() {
        uint32_t nc = n_classes.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < nc; i++) classes[i].lock.unlock();
        reg_lock.unlock();
    }
    void atfork_child() {
        uint32_t nc = n_classes.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < nc; i++) classes[i].lock.unlock();
        reg_lock.unlock();
    }
};

// Immortal singleton: constructed on first use, never destroyed, so late
// deallocations during static teardown remain valid.
static GlobalStackPool* g_pool = nullptr;

static void pool_key_dtor(void* p) {
    if (g_pool) g_pool->drain_pervcpu((GlobalStackPool::PerVCPU*)p);
    delete (GlobalStackPool::PerVCPU*)p;
}
static void atfork_prepare_cb() { if (g_pool) g_pool->atfork_prepare(); }
static void atfork_parent_cb()  { if (g_pool) g_pool->atfork_parent(); }
static void atfork_child_cb()   { if (g_pool) g_pool->atfork_child(); }

void* global_pooled_stack_alloc(void*, size_t stack_size) {
    return g_pool->alloc(stack_size);
}
void global_pooled_stack_dealloc(void*, void* stack_ptr, size_t stack_size) {
    g_pool->dealloc(stack_ptr, stack_size);
}

int use_global_pooled_stack_allocator(const GlobalStackPoolOptions& options) {
    if (unlikely(PAGE_SIZE == 0)) PAGE_SIZE = getpagesize();
    // Serialize one-time construction so concurrent first callers cannot double
    // create the pthread_key / atfork handlers. The options update below must
    // still happen while the pool is quiescent (see the header contract).
    static std::once_flag once;
    static bool init_ok = false;
    std::call_once(once, [] {
        static GlobalStackPool pool_storage;
        pool_storage.magic_seed = ((uint64_t)&pool_storage << 1) ^
                                  ((uint64_t)getpid() << 32) ^ (uintptr_t)&once;
        if (pthread_key_create(&pool_storage.key, pool_key_dtor) != 0) {
            LOG_ERROR("pthread_key_create failed");
            return;
        }
        pthread_atfork(atfork_prepare_cb, atfork_parent_cb, atfork_child_cb);
        g_pool = &pool_storage;   // publish only after fully initialized
        init_ok = true;
    });
    if (!init_ok || !g_pool)
        LOG_ERRNO_RETURN(0, -1, "global stack pool init failed");
    g_pool->opt = options;
    if (g_pool->opt.guard_pages < 1) g_pool->opt.guard_pages = 1;
    set_photon_thread_stack_allocator({&global_pooled_stack_alloc, nullptr},
                                      {&global_pooled_stack_dealloc, nullptr});
    return 0;
}

size_t global_pooled_stack_trim(size_t keep_bytes) {
    return g_pool ? g_pool->trim(keep_bytes) : 0;
}

GlobalStackPoolStats global_pooled_stack_stats() {
    if (!g_pool) return GlobalStackPoolStats{};
    return g_pool->stats();
}

}  // namespace photon
