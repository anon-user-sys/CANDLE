#pragma once

#if defined(USE_CXL)

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <cstdio>
#include <string>
#include <immintrin.h>
#include "pxl/pxl.hpp"

namespace pxl_helper {
    inline void safeFlushHostCache(void* ptr, std::size_t size) {
        pxl::flushHostCache(ptr, size); // opt=true: clflushopt + an internal mfence
        _mm_sfence(); // redundant but harmless
    }
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
PxlUniquePtr<T> allocatePxlMemory(pxl::runtime::Context* context, size_t count) {
    T* ptr = reinterpret_cast<T*>(context->memAlloc(count * sizeof(T)));
    if (!ptr) throw std::runtime_error("Failed to allocate PXL CXL shared memory");
    return PxlUniquePtr<T>(ptr, PxlMemoryDeleter<T>(context));
}

// Upper bound on device tasks = the runtime limit (C22; C2's cap of 3,048 assumed one task per thread, which is wrong)
constexpr std::uint32_t kMaxDeviceTasks = pxl::MaxTaskCount;
// C22: a task is not an MU thread. By default the runtime runs 16 tasks one after another on each thread,
// so 3,048 tasks use only ~191 MU threads. There are 3,048 user MU threads (3,072 - 24 Admin/HIO).
// A resident kernel needs every task running at once, so its task count must not exceed this value.
constexpr std::uint32_t kUserMuThreads = 3048;

struct PxlResources {
    pxl::runtime::Context* context{nullptr};
    pxl::kernel::Module* module{nullptr};
    pxl::kernel::Function* function{nullptr};
    pxl::runtime::Job* job{nullptr};
    pxl::runtime::Map* executor{nullptr};

    void clear_job() {
        if (job && executor) job->destroyMap(executor);
        executor = nullptr;
        if (context && job) context->destroyJob(job);
        job = nullptr;
        if (module && function) module->destroyFunction(function);
        function = nullptr;
        if (module) pxl::kernel::destroyModule(module);
        module = nullptr;
    }

    ~PxlResources() {
        clear_job();
        if (context) {
            pxl::runtime::destroyContext(context);
        }
    }
};

// mu_batch: tasks run one after another by one MU thread (Map::setBatchSize). 0 = runtime default (16).
// A resident kernel needs 1: otherwise only the first task per thread starts and the rest wait forever (C21).
inline void loadPxlKernel(PxlResources* res, const char* mubin_path, const char* func_name, uint32_t numTasks,
                          uint32_t mu_batch = 0) {
    if (!res || !res->context) throw std::runtime_error("PXL Context not initialized");

    res->clear_job();

    res->module = pxl::kernel::createModule(mubin_path);
    if (!res->module) throw std::runtime_error("Cannot load MuBinary: " + std::string(mubin_path));

    res->job = res->context->createJob();
    if (!res->job) throw std::runtime_error("Cannot create PXL job");
    if (res->job->load(res->module) != pxl::PxlResult::Success)
        throw std::runtime_error("Cannot load module into job: " + std::string(mubin_path));

    res->function = res->module->createFunction(func_name);
    if (!res->function) throw std::runtime_error("Cannot create function: " + std::string(func_name));

    res->executor = res->job->buildMap(res->function, numTasks);
    if (!res->executor) throw std::runtime_error("Cannot build PXL map executor");
    if (mu_batch > 0) res->executor->setBatchSize(mu_batch);
}

// If synchronize fails, print the kernel error (task range) and throw
inline void synchronizeOrThrow(PxlResources* res, const char* what) {
    if (res->executor->synchronize() == pxl::PxlResult::Success) return;
    for (const auto& err : res->executor->getKernelError()) {
        std::fprintf(stderr, "[PXL] %s kernel error: tasks [%u, %u)\n", what, err.beginIndex, err.endIndex);
    }
    throw std::runtime_error(std::string(what) + " synchronize failed");
}

#endif // USE_CXL