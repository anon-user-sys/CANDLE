#include <stdint.h>
#include <algorithm>

extern "C" {
    void* __dso_handle __attribute__((weak)) = nullptr;
    int __cxa_atexit(void (*destructor)(void*), void* arg, void* dso) {
        (void)destructor;
        (void)arg;
        (void)dso;
        return 0;
    }
}

#include "mu/mu.hpp"
#include "cxl_node.h"


// Both kernels run on a quiescent tree: the host drains its separator tasks and stays idle during a
// launch, lookup and write launches never overlap, the device never splits, and every leaf has a
// single owner task in a write launch. So no latch is taken on the device. The memory-side version
// AMOs this used to do were the device bottleneck (a 1M-request lookup launch: 84 ms with them,
// 21.5 ms without, independent of task count), and the latch never stopped two device writers on
// one leaf from losing updates anyway.
static inline std::uint32_t quiescent_version() { return 0; }

// ==========================================
// 1. CXL WRITE BATCH KERNEL
// ==========================================

// fresh = true (resident worker): the request buffer is reused across jobs, so a task drops its cached
// copies of every request line it reads (binary-search probes, its slice) before reading them.
static void write_batch_impl(CXLNode* tree_root, CXLWriteRequest* req_batch, int num_requests_or_bytes, bool fresh) {

    auto task_idx = mu::getTaskIdx();
    auto active_tasks = mu::getTaskCount();

    int real_num_requests = num_requests_or_bytes / static_cast<int>(sizeof(CXLWriteRequest));

    if (active_tasks <= 0 || real_num_requests <= 0 || !tree_root || !req_batch) return;
    if (task_idx < 0 || task_idx >= active_tasks) return;

    // Requests arrive sorted by owner; this task handles exactly the ones it owns, so every
    // leaf is written by a single task (several MU writers on one leaf lose updates).
    const std::uint16_t me = static_cast<std::uint16_t>(task_idx);
    int lo = 0, hi = real_num_requests;
    auto owner_at = [&](int i) {
#if defined(CXL_MU_DEVICE)
        if (fresh) mu::flushCache(&req_batch[i], sizeof(CXLWriteRequest));
#endif
        return req_batch[i].owner;
    };
    while (lo < hi) { int mid = (lo + hi) / 2; if (owner_at(mid) < me) lo = mid + 1; else hi = mid; }
    const int start = lo;
    hi = real_num_requests;
    while (lo < hi) { int mid = (lo + hi) / 2; if (owner_at(mid) <= me) lo = mid + 1; else hi = mid; }
    const int end = lo;
#if defined(CXL_MU_DEVICE)
    if (fresh && end > start) mu::flushCache(&req_batch[start], static_cast<std::uint64_t>(end - start) * sizeof(CXLWriteRequest));
#else
    (void)fresh;
#endif

    // Every task ends here: write back its own lines.
    auto finish = [&]() {
#if defined(CXL_MU_DEVICE)
        mu::flushCacheAll();
#else
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
    };

    if (start >= end) { finish(); return; }

    auto* root_node = tree_root;

    for (int i = start; i < end; ++i) {
        auto& req = req_batch[i];
        if (req.status != 0) continue;

        // Start at the leaf the locate pass found; B-link right moves cover any change since.
        CXLNode* curr = req.leaf ? reinterpret_cast<CXLNode*>(req.leaf) : root_node;
        std::uint32_t hop_count = 0;

        while (curr != nullptr) {
            std::uintptr_t curr_addr = reinterpret_cast<std::uintptr_t>(curr);
            if (curr_addr < 0x1000 || curr_addr % 8 != 0) {
                curr = root_node;
                if (++hop_count > 500) { req.status = (req.op == 1) ? 2 : 3; break; }
                continue;
            }

#if defined(CXL_MU_DEVICE)
            mu::flushCache(curr, sizeof(CXLNode));
#endif

            std::uint32_t v1 = quiescent_version();
            if (v1 & 1) {
#if defined(CXL_MU_DEVICE)
                __asm__ __volatile__ (".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");
#endif
                if (++hop_count > 500) { req.status = (req.op == 1) ? 2 : 3; break; }
                continue;
            }

            std::uint64_t hk = curr->high_key();
            CXLNode* ns = curr->right_sibling();
            if (false) {
#if defined(CXL_MU_DEVICE)
                __asm__ __volatile__ (".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");
#endif
                if (++hop_count > 500) { req.status = (req.op == 1) ? 2 : 3; break; }
                continue;
            }

            if (hk <= req.key) {
                if (ns != nullptr) {
                    if (++hop_count > 500) { req.status = (req.op == 1) ? 2 : 3; break; }
                    curr = ns;
                    continue;
                } else if (curr != root_node && hk < 0xFFFFFFFFFFFFFFFFULL) {
                    curr = root_node;
                    if (++hop_count > 500) { req.status = (req.op == 1) ? 2 : 3; break; }
                    continue;
                }            
            }

            if (curr->is_inner()) {
                CXLNode* child_node = curr->child(req.key);
                std::uintptr_t child_addr = reinterpret_cast<std::uintptr_t>(child_node);
                
                if (false) {
#if defined(CXL_MU_DEVICE)
                    __asm__ __volatile__ (".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");
#endif
                    if (++hop_count > 500) { req.status = (req.op == 1) ? 2 : 3; break; }
                    continue;
                }

                if (child_node == nullptr || child_addr < 0x1000 || child_addr % 8 != 0) {
                    if (curr == root_node) {
#if defined(CXL_MU_DEVICE)
                        __asm__ __volatile__ (".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");
#endif
                        if (++hop_count > 500) { req.status = (req.op == 1) ? 2 : 3; break; }
                        continue;
                    }
                    curr = root_node;
                    if (++hop_count > 500) { req.status = (req.op == 1) ? 2 : 3; break; }
                    continue;
                }
                
                if (++hop_count > 500) { req.status = (req.op == 1) ? 2 : 3; break; }
                curr = child_node;
                continue;
            }
            
            if (false) {
#if defined(CXL_MU_DEVICE)
                __asm__ __volatile__ (".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");
#endif
                if (++hop_count > 500) { req.status = (req.op == 1) ? 2 : 3; break; }
                continue;
            }
#if defined(CXL_MU_DEVICE)
            // Lines read before the lock may predate the previous holder's write-back.
            mu::flushCache(curr, sizeof(CXLNode));
#endif

            if (curr->high_key() <= req.key) {
                CXLNode* next_ns = curr->right_sibling();
                if (next_ns != nullptr) {
#if defined(CXL_MU_DEVICE)
                    mu::flushCache(curr, sizeof(CXLNode));
#endif
                            if (++hop_count > 500) { req.status = (req.op == 1) ? 2 : 3; break; }
                    curr = next_ns;
                    continue;
                } else if (curr != root_node && curr->high_key() < 0xFFFFFFFFFFFFFFFFULL) {
#if defined(CXL_MU_DEVICE)
                    mu::flushCache(curr, sizeof(CXLNode));
#endif
                    
                    curr = root_node;
                    if (++hop_count > 500) { req.status = (req.op == 1) ? 2 : 3; break; }
                    continue;
                }
            }

            std::uint16_t idx = curr->index(req.key);
            bool key_exists = (idx < curr->size() && curr->leaf_key(idx) == req.key);

            if (key_exists) {
                curr->value(idx, req.value);
                req.status = 1;
            } else if (req.op == 0) {
                if (curr->size() < CXLLeafNode::max_items) {
                    curr->insert_leaf(idx, req.key, req.value);
                    req.status = 1;
                } else {
                    req.status = 3; // Need Host Split
                }
            } else {
                req.status = 2; // Missing
            }

#if defined(CXL_MU_DEVICE)
            mu::flushCache(curr, sizeof(CXLNode));
#endif
            break;
        }
    }

    finish();
}

// ==========================================
// 2. CXL LOOKUP BATCH KERNEL
// ==========================================
static void lookup_batch_impl(CXLNode* tree_root, CXLLookupRequest* req_batch, int num_requests_or_bytes, bool fresh) {

    auto task_idx = mu::getTaskIdx();
    auto active_tasks = mu::getTaskCount();

    int real_num_requests = num_requests_or_bytes / static_cast<int>(sizeof(CXLLookupRequest));

    if (active_tasks <= 0 || real_num_requests <= 0 || !tree_root || !req_batch) return;
    if (task_idx < 0 || task_idx >= active_tasks) return;

    int chunk_size = (real_num_requests + active_tasks - 1) / active_tasks;
    chunk_size += chunk_size & 1; // two records per line: keep each task's slice line-aligned
    int start = task_idx * chunk_size;
    int end = std::min(start + chunk_size, real_num_requests);

    if (start >= real_num_requests) return;
#if defined(CXL_MU_DEVICE)
    if (fresh) mu::flushCache(&req_batch[start], static_cast<std::uint64_t>(end - start) * sizeof(CXLLookupRequest));
#else
    (void)fresh;
#endif

    auto* root_node = tree_root;

    for (int i = start; i < end; ++i) {
        auto& req = req_batch[i];
        if (req.status != 0) continue;

        CXLNode* curr = root_node;
        std::uint32_t hop_count = 0;

        while (curr != nullptr) {
            std::uintptr_t curr_addr = reinterpret_cast<std::uintptr_t>(curr);
            if (curr_addr < 0x1000 || curr_addr % 8 != 0) {
                curr = root_node;
                if (++hop_count > 500) { req.status = 2; break; }
                continue;
            }

#if defined(CXL_MU_DEVICE)
            mu::flushCache(curr, sizeof(CXLNode));
#endif

            std::uint32_t v1 = quiescent_version();
            if (v1 & 1) {
#if defined(CXL_MU_DEVICE)
                __asm__ __volatile__ (".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");
#endif
                if (++hop_count > 500) { req.status = 2; break; }
                continue;
            }

            std::uint64_t hk = curr->high_key();
            CXLNode* ns = curr->right_sibling();
            if (false) {
#if defined(CXL_MU_DEVICE)
                __asm__ __volatile__ (".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");
#endif
                if (++hop_count > 500) { req.status = 2; break; }
                continue;
            }

            if (hk <= req.key) {
                if (ns != nullptr) {
                    if (++hop_count > 500) { req.status = 2; break; }
                    curr = ns;
                    continue;
                } else if (curr != root_node && hk < 0xFFFFFFFFFFFFFFFFULL) {
                    curr = root_node;
                    if (++hop_count > 500) { req.status = 2; break; }
                    continue;
                }            
            }

            if (curr->is_inner()) {
                CXLNode* child_node = curr->child(req.key);
                std::uintptr_t child_addr = reinterpret_cast<std::uintptr_t>(child_node);
                
                if (false) {
#if defined(CXL_MU_DEVICE)
                    __asm__ __volatile__ (".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");
#endif
                    if (++hop_count > 500) { req.status = 2; break; }
                    continue;
                }

                if (child_node == nullptr || child_addr < 0x1000 || child_addr % 8 != 0) {
                    if (curr == root_node) {
#if defined(CXL_MU_DEVICE)
                        __asm__ __volatile__ (".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");
#endif
                        if (++hop_count > 500) { req.status = 2; break; }
                        continue;
                    }
                    curr = root_node;
                    if (++hop_count > 500) { req.status = 2; break; }
                    continue;
                }
                
                if (++hop_count > 500) { req.status = 2; break; }
                curr = child_node;
                continue;
            }

            std::uint16_t idx = curr->index(req.key);
            bool is_found = (idx < curr->size() && curr->leaf_key(idx) == req.key);
            std::int64_t found_val = is_found ? curr->value(idx) : 0;

            if (false) {
#if defined(CXL_MU_DEVICE)
                __asm__ __volatile__ (".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");
#endif
                if (++hop_count > 500) { req.status = 2; break; }
                continue;
            }

            req.leaf = reinterpret_cast<std::uint64_t>(curr);
            if (is_found) {
                req.value = found_val;
                req.status = 1;
            } else {
                req.status = 2;
            }
            break;
        }
    }

#if defined(CXL_MU_DEVICE)
    mu::flushCacheAll();
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

void cxl_process_write_batch(CXLNode* tree_root, CXLWriteRequest* req_batch, int num_requests_or_bytes) {
    write_batch_impl(tree_root, req_batch, num_requests_or_bytes, false);
}

void cxl_process_lookup_batch(CXLNode* tree_root, CXLLookupRequest* req_batch, int num_requests_or_bytes) {
    lookup_batch_impl(tree_root, req_batch, num_requests_or_bytes, false);
}

// Resident worker (--resident, NDP@CXL): launched once, all tasks resident (setBatchSize(1), tasks <= 3,048
// usable MU threads). The host posts jobs into ctrl (CXL memory, two 64 B lines, written with NT stores):
//   line 0: [0] seq (job number, from 1)  [1] kind (1 = lookup, 2 = write)  [2] request buffer
//           [3] request count  [4] stop  [5] tree root
//   line 1: [8] done (int32; each task atomicAdds 1 per finished job, in memory)
// Same pattern as the hash join's resident probe kernel: write results, flushCacheAll (in the job), then
// signal done with a memory-side atomic.
void cxl_resident_worker(std::uint64_t* ctrl, int numTasks) {
    if (!ctrl || numTasks <= 0) return;
    const int task_idx = static_cast<int>(mu::getTaskIdx());
    if (task_idx < 0 || task_idx >= numTasks) return;
    volatile std::uint64_t* c = ctrl;
    std::uint64_t last = 0;
    for (;;) {
        mu::flushCache(ctrl, 64);                     // drop the cached ctrl line, re-read from memory
        if (c[4]) break;
        const std::uint64_t seq = c[0];
        if (seq == last) {
            for (volatile int d = 0; d < 256; ++d) {}  // back off: fewer reads of the ctrl line
            continue;
        }
        last = seq;
        auto* root = reinterpret_cast<CXLNode*>(c[5]);
        const int bytes = static_cast<int>(c[3] * 32);   // both request records are 32 B
        if (c[1] == 1) lookup_batch_impl(root, reinterpret_cast<CXLLookupRequest*>(c[2]), bytes, true);
        else write_batch_impl(root, reinterpret_cast<CXLWriteRequest*>(c[2]), bytes, true);
        mu::flushCacheAll();                          // results in memory before the done signal
        mu::atomicAdd(reinterpret_cast<std::int32_t*>(ctrl + 8), 1);
    }
    mu::flushCacheAll();
}

MU_KERNEL_ADD(cxl_process_write_batch)
MU_KERNEL_ADD(cxl_process_lookup_batch)
MU_KERNEL_ADD(cxl_resident_worker)
