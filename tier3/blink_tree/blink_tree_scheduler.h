#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <memory>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <pthread.h>
#include <sched.h>

class Task {
public:
    virtual ~Task() = default;
    virtual void execute(std::uint16_t thread_id) = 0;
};

namespace db::index::blinktree {

template <typename K, typename V, class L> 
class Task : public ::Task {
public:
    constexpr Task(const K key, L &listener) : _listener(listener), _key(key) {}
    ~Task() override = default;

protected:
    L &_listener;
    K _key;
};

class BLinkTreeScheduler {
private:
    struct alignas(64) Channel {
        std::queue<std::unique_ptr<::Task>> queue;
        alignas(64) std::mutex mutex;
        alignas(64) std::condition_variable cv;
    };

public:
    static BLinkTreeScheduler& get() {
        static BLinkTreeScheduler instance;
        return instance;
    }

    std::uint16_t num_workers() const noexcept { 
        std::shared_lock<std::shared_mutex> lock(_channel_mutex);
        return static_cast<std::uint16_t>(_channels.size()); 
    }
    
    // CPUs worker threads are pinned to, in order (worker i -> cpus[i % size]); empty = no pinning.
    void set_pin_cpus(std::vector<int> cpus) { _pin_cpus = std::move(cpus); }

    void initialize(std::uint16_t num_threads) {
        shutdown(); // join the previous threads safely
        _shutdown.store(false, std::memory_order_release);
        
        {
            std::unique_lock<std::shared_mutex> lock(_channel_mutex);
            _channels.clear();
            _channels.reserve(num_threads);
            for (std::uint16_t i = 0; i < num_threads; ++i) {
                _channels.push_back(std::make_unique<Channel>());
            }
        }

        _workers.clear();
        _workers.reserve(num_threads);
        for (std::uint16_t i = 0; i < num_threads; ++i) {
            _workers.emplace_back(&BLinkTreeScheduler::worker_loop, this, i);
        }
    }

    void shutdown() {
        if (_shutdown.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        {
            std::shared_lock<std::shared_mutex> lock(_channel_mutex);
            for (auto& ch : _channels) {
                if (ch) {
                    std::lock_guard<std::mutex> ch_lock(ch->mutex);
                    ch->cv.notify_all(); 
                }
            }
        }

        for (auto& worker : _workers) { 
            if (worker.joinable()) worker.join(); 
        }
        _workers.clear(); 

        std::unique_lock<std::shared_mutex> lock(_channel_mutex);
        _channels.clear();
    }

    void spawn(std::unique_ptr<::Task> task, std::uint16_t channel_id) {
        if (_shutdown.load(std::memory_order_acquire)) return;

        std::shared_lock<std::shared_mutex> lock(_channel_mutex);
        if (_channels.empty()) return;

        std::uint16_t target = channel_id % _channels.size();
        auto& ch = *_channels[target];
        {
            std::lock_guard<std::mutex> ch_lock(ch.mutex);
            ch.queue.push(std::move(task));
        }
        ch.cv.notify_one(); 
    }

    // Runs one queued task of the given channel on the calling thread, if any.
    bool try_run_one(std::uint16_t channel_id) {
        std::unique_ptr<::Task> task;
        {
            std::shared_lock<std::shared_mutex> lock(_channel_mutex);
            if (channel_id >= _channels.size()) return false;
            auto& ch = *_channels[channel_id];
            std::lock_guard<std::mutex> ch_lock(ch.mutex);
            if (ch.queue.empty()) return false;
            task = std::move(ch.queue.front());
            ch.queue.pop();
        }
        task->execute(channel_id);
        return true;
    }

private:
    BLinkTreeScheduler() = default;
    ~BLinkTreeScheduler() { shutdown(); }

    void worker_loop(std::uint16_t channel_id) {
        if (!_pin_cpus.empty()) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(_pin_cpus[channel_id % _pin_cpus.size()], &set);
            pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
        }
        while (true) {
            std::unique_ptr<::Task> task;
            {
                std::shared_lock<std::shared_mutex> ch_map_lock(_channel_mutex);
                if (channel_id >= _channels.size()) break;
                auto& ch = *_channels[channel_id];
                ch_map_lock.unlock();

                std::unique_lock<std::mutex> lock(ch.mutex);
                ch.cv.wait(lock, [this, &ch] {
                    return _shutdown.load(std::memory_order_acquire) || !ch.queue.empty();
                });

                if (_shutdown.load(std::memory_order_acquire) && ch.queue.empty()) {
                    break;
                }

                if (!ch.queue.empty()) {
                    task = std::move(ch.queue.front());
                    ch.queue.pop();
                }
            }

            if (task) {
                task->execute(channel_id);
            }
        }
    }
    
    std::vector<int> _pin_cpus;
    std::vector<std::thread> _workers;
    std::vector<std::unique_ptr<Channel>> _channels;
    mutable std::shared_mutex _channel_mutex;
    alignas(64) std::atomic<bool> _shutdown{false};
};
} // namespace db::index::blinktree