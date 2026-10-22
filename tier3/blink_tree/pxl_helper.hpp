#pragma once
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "cxl_node.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#if defined(MODE_HOST_CXL) || defined(MODE_CXL_HYBRID) || defined(MODE_CXL_FULL)
#ifndef USE_CXL
#define USE_CXL
#endif
#endif


#ifdef USE_CXL
#include <mu/mu.hpp>
#include "pxl/pxl.hpp"
#include <pxl/runtime/runtime_api.hpp>

inline pxl::runtime::Context* g_pxl_context = nullptr;

// Kernel launch handles. A Map is rebuilt for every launch: reusing one Map makes each
// synchronize() ~3.3 us slower than the last (2.5 ms -> 13.5 ms over 4000 launches, PXL
// runtime, reproduced with an empty kernel), while build+execute+sync+destroy stays ~1.2 ms.
inline pxl::runtime::Job* g_pxl_job = nullptr;
inline pxl::kernel::Function* g_pxl_func_write = nullptr;
inline pxl::kernel::Function* g_pxl_func_lookup = nullptr;
inline pxl::kernel::Function* g_pxl_func_resident = nullptr;   // --resident (NDP@CXL)
inline std::uint32_t g_pxl_num_tasks = 1;

template <typename... ARGS>
inline void pxl_launch_sync(pxl::kernel::Function* func, std::uint32_t num_tasks, ARGS&&... args) {
    if (!g_pxl_job || !func) throw std::runtime_error("PXL kernel function not initialized");
    pxl::runtime::Map* map = g_pxl_job->buildMap(func, num_tasks);
    if (!map) throw std::runtime_error("PXL buildMap failed");
    map->setBatchSize(1); // one task per MU thread (default 16 would put 64 tasks on 4 threads)
    if (map->execute(std::forward<ARGS>(args)...) != pxl::PxlResult::Success) {
        g_pxl_job->destroyMap(map);
        throw std::runtime_error("PXL execute failed");
    }
    pxl::PxlResult sync = map->synchronize();
    g_pxl_job->destroyMap(map);
    if (sync != pxl::PxlResult::Success) throw std::runtime_error("PXL synchronize failed");
}

template <typename T>
struct PxlMemoryDeleter {
    pxl::runtime::Context* context{nullptr};

    PxlMemoryDeleter() = default;
    explicit PxlMemoryDeleter(pxl::runtime::Context* ctx) : context(ctx) {}

    void operator()(T* ptr) const {
        if (ptr && context) {
            context->memFree(ptr);
        }
    }
};

template <typename T>
using PxlUniquePtr = std::unique_ptr<T, PxlMemoryDeleter<T>>;

template <typename T>
PxlUniquePtr<T> allocatePxlMemory(pxl::runtime::Context* context, size_t count = 1) {
    if (!context) throw std::runtime_error("PXL Context is null");
    T* ptr = reinterpret_cast<T*>(context->memAlloc(count * sizeof(T)));
    if (!ptr) throw std::runtime_error("Failed to allocate PXL CXL shared memory");
    return PxlUniquePtr<T>(ptr, PxlMemoryDeleter<T>(context));
}

struct PxlResources {
    pxl::runtime::Context* context{nullptr};
    pxl::runtime::Job* job{nullptr}; // a single Job object only


    void clear_jobs() {
        if (context && job) {
            context->destroyJob(job);
            job = nullptr;
            g_pxl_job = nullptr;
            g_pxl_func_write = nullptr;
            g_pxl_func_lookup = nullptr;
            g_pxl_context = nullptr;
        }
    }

    ~PxlResources() {
        clear_jobs();
        if (context) {
            pxl::runtime::destroyContext(context);
            context = nullptr;
        }
    }
};

inline std::unique_ptr<PxlResources> initializePxl(uint32_t numTasks) {
    auto res = std::make_unique<PxlResources>();
    res->context = pxl::runtime::createContext(0);
    if (!res->context) {
        throw std::runtime_error("Failed to create PXL Context");
    }
    g_pxl_context = res->context;

    res->job = res->context->createJob();
    if (!res->job) {
        throw std::runtime_error("Failed to create PXL Job");
    }

    // load the single combined binary module
    auto module = pxl::kernel::createModule("mu_kernel/mu_blink_tree.mubin");
    if (!module) throw std::runtime_error("Cannot load MuBinary (mu_blink_tree.mubin)");
    res->job->load(module);
    g_pxl_job = res->job;
    g_pxl_num_tasks = numTasks;

#if defined(MODE_CXL_HYBRID) || defined(MODE_CXL_FULL)
    {
        g_pxl_func_write = module->createFunction("cxl_process_write_batch");
        if (!g_pxl_func_write) throw std::runtime_error("Cannot create Write function");
    }
#endif

#if defined(MODE_CXL_FULL)
    {
        g_pxl_func_lookup = module->createFunction("cxl_process_lookup_batch");
        if (!g_pxl_func_lookup) throw std::runtime_error("Cannot create Lookup function");
        g_pxl_func_resident = module->createFunction("cxl_resident_worker");
        if (!g_pxl_func_resident) throw std::runtime_error("Cannot create resident worker function");
    }
#endif

    return res;
}
#endif // USE_CXL