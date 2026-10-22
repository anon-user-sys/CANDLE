#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sched.h>

class Task {
public:
    virtual ~Task() = default;
    virtual void execute(std::uint16_t thread_id) = 0;
};

class MorselScheduler {
public:
    static MorselScheduler& get() {
        static MorselScheduler instance;
        return instance;
    }

    void initialize(std::uint16_t host_thread_count) {
        shutdown();

        _shutdown.store(false, std::memory_order_seq_cst);
        _tasks_in_flight.store(0, std::memory_order_seq_cst);
        _host_thread_count = host_thread_count;
        
        _per_worker_queues = std::make_unique<std::queue<std::unique_ptr<Task>>[]>(host_thread_count);
        _per_worker_mutexes = std::make_unique<std::mutex[]>(host_thread_count);
        _per_worker_cvs = std::make_unique<std::condition_variable[]>(host_thread_count);

        // C28 (T42): HJ_PIN_THREADS=1 pins worker i to the i-th CPU of the process's allowed set
        // (numactl --cpunodebind=0 -> node0 core i). Default: unpinned, as before.
        std::vector<int> cpus;
        const char* pin = std::getenv("HJ_PIN_THREADS");
        if (pin && std::strcmp(pin, "1") == 0) {
            cpu_set_t allowed;
            CPU_ZERO(&allowed);
            if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0)
                for (int c = 0; c < CPU_SETSIZE; ++c) if (CPU_ISSET(c, &allowed)) cpus.push_back(c);
            std::fprintf(stderr, "[PIN] %u workers pinned 1:1 to %zu allowed CPUs (first %d)\n",
                         host_thread_count, cpus.size(), cpus.empty() ? -1 : cpus[0]);
        }

        _workers.reserve(host_thread_count);
        for (std::uint16_t i = 0; i < host_thread_count; ++i) {
            _workers.emplace_back([this, worker_id = i]() {
                while (true) {
                    std::unique_ptr<Task> task;
                    {
                        std::unique_lock<std::mutex> lock(_per_worker_mutexes[worker_id]);
                        _per_worker_cvs[worker_id].wait(lock, [this, worker_id]() {
                            return _shutdown.load(std::memory_order_seq_cst) || !_per_worker_queues[worker_id].empty();
                        });

                        if (_shutdown.load(std::memory_order_seq_cst) && _per_worker_queues[worker_id].empty()) return;

                        task = std::move(_per_worker_queues[worker_id].front());
                        _per_worker_queues[worker_id].pop();
                    }

                    if (task) {
                        task->execute(worker_id);
                        if (_tasks_in_flight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                            std::unique_lock<std::mutex> wait_lock(_wait_mutex);
                            _wait_cv.notify_all();
                        }
                    }
                }
            });
            if (!cpus.empty()) {
                cpu_set_t one;
                CPU_ZERO(&one);
                CPU_SET(cpus[i % cpus.size()], &one);
                if (pthread_setaffinity_np(_workers.back().native_handle(), sizeof(one), &one) != 0)
                    std::fprintf(stderr, "[PIN] worker %u: pthread_setaffinity_np failed\n", i);
            }
        }
    }

    void spawn(std::unique_ptr<Task> task, std::uint16_t target_id) {
        if (!task || _host_thread_count == 0) return;
        std::uint16_t worker_id = target_id % _host_thread_count;
        {
            std::unique_lock<std::mutex> lock(_per_worker_mutexes[worker_id]);
            _tasks_in_flight.fetch_add(1, std::memory_order_acq_rel);
            _per_worker_queues[worker_id].push(std::move(task));
        }
        _per_worker_cvs[worker_id].notify_one();
    }

    void wait_until_finished() {
        std::unique_lock<std::mutex> lock(_wait_mutex);
        _wait_cv.wait(lock, [this]() { return _tasks_in_flight.load(std::memory_order_acquire) == 0; });
    }

    void shutdown() {
        _shutdown.store(true, std::memory_order_seq_cst);

        if (_per_worker_mutexes && _per_worker_cvs) {
            for (std::uint16_t i = 0; i < _host_thread_count; ++i) {
                std::lock_guard<std::mutex> lock(_per_worker_mutexes[i]);
                _per_worker_cvs[i].notify_all();
            }
        }

        for (auto& worker : _workers) {
            if (worker.joinable()) worker.join();
        }
        _workers.clear();
        _per_worker_queues.reset();
        _per_worker_mutexes.reset();
        _per_worker_cvs.reset();
        _host_thread_count = 0;
    }

    std::uint16_t host_thread_count() const noexcept { return _host_thread_count; }

private:
    MorselScheduler() = default;
    ~MorselScheduler() { shutdown(); }

    std::vector<std::thread> _workers;
    std::uint16_t _host_thread_count{0};
    
    std::unique_ptr<std::queue<std::unique_ptr<Task>>[]> _per_worker_queues;
    std::unique_ptr<std::mutex[]> _per_worker_mutexes;
    std::unique_ptr<std::condition_variable[]> _per_worker_cvs;
    
    std::mutex _wait_mutex;
    std::condition_variable _wait_cv;
    std::atomic<std::size_t> _tasks_in_flight{0};
    std::atomic<bool> _shutdown{false};
};