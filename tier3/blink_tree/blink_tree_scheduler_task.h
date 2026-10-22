#pragma once
#include <vector>
#include <memory>
#include <thread>
#include <algorithm>
#include <unordered_map>
#include <functional>
#include <atomic>
#include "blink_tree.h"
#include "blink_tree_tasks.h"
#include "blink_tree_cxl_batch.h"
#include "request_container.h"

namespace application::blinktree_benchmark {

// Listener for helper tasks inside a batch (leaf splits, batched host writes): the batch reports
// results itself, so only separator bookkeeping goes to the container.
class BatchHelperListener final : public db::index::blinktree::Listener<std::uint64_t, std::int64_t> {
public:
    RequestContainer* container{nullptr};
    void separator_started() override { if (container) container->separator_started(); }
    void separator_finished() override { if (container) container->separator_finished(); }
};
inline BatchHelperListener g_batch_helper_listener;

// --host-dispatch=batched (host modes): the NDP@CXL batch pipeline run by host threads, so both
// plans do the same work under the same timing rule (dedup + replay excluded, see process_host_batch).
inline bool g_host_batched = false;

class LambdaTask final : public ::Task {
public:
    explicit LambdaTask(std::function<void()> fn) : _fn(std::move(fn)) {}
    void execute(std::uint16_t) override { _fn(); }
private:
    std::function<void()> _fn;
};

// Runs f(begin, end) over [0, n) split across the scheduler's workers (the caller does part 0 and
// runs its own queue while it waits, so separator tasks queued on it still make progress).
template <typename F>
inline void parallel_for_workers(std::size_t n, std::uint16_t thread_id, F&& f) {
    auto& env = db::index::blinktree::BLinkTreeScheduler::get();
    const std::size_t workers = env.num_workers();
    const std::size_t parts = std::max<std::size_t>(1, std::min<std::size_t>(workers, (n + 255) / 256));
    std::atomic<std::size_t> done{0};
    auto range = [&](std::size_t p) { return std::make_pair(n * p / parts, n * (p + 1) / parts); };
    for (std::size_t p = 1; p < parts; ++p) {
        env.spawn(std::make_unique<LambdaTask>([&, p] {
            const auto [b, e] = range(p);
            f(b, e);
            done.fetch_add(1, std::memory_order_release);
        }), static_cast<std::uint16_t>((thread_id + p) % workers));
    }
    { const auto [b, e] = range(0); f(b, e); }
    while (done.load(std::memory_order_acquire) + 1 < parts) {
        if (!env.try_run_one(thread_id)) std::this_thread::yield();
    }
}

class RequestSchedulerTask final : public ::Task {
public:
    RequestSchedulerTask(std::uint16_t channel_id, std::shared_ptr<RequestContainer> container,
                         std::vector<db::index::blinktree::BLinkTree<std::uint64_t, std::int64_t>*> trees)
        : _channel_id(channel_id), _container(std::move(container)), _trees(std::move(trees)) {}

    void execute(std::uint16_t thread_id) override {
        auto &res = *_container;
        auto next_reqs = res.next();

        if (next_reqs.has_new()) {
            namespace bt = db::index::blinktree;
            auto& env = bt::BLinkTreeScheduler::get();
            const std::uint64_t batch_begin = next_reqs.index();
            const std::uint64_t batch_end = next_reqs.index() + next_reqs.count();

#if defined(MODE_CXL_HYBRID) || defined(MODE_CXL_FULL)
            process_cxl_batch(res, batch_begin, batch_end, thread_id);
#else
            if (g_host_batched) {
                process_host_batch(res, batch_begin, batch_end, thread_id);
                env.spawn(std::make_unique<RequestSchedulerTask>(*this), thread_id);
                return;
            }
            const std::uint16_t total_workers = env.num_workers();
            for (auto i = batch_begin; i < batch_end; ++i) {
                const auto &tuple = res[i];
                const std::uint16_t target_channel = static_cast<std::uint16_t>(tuple.key() % total_workers);
                auto* target_tree = _trees[target_channel];

#ifdef MEASURE_LATENCY
                auto start_time = db::index::blinktree::Clock::now();
#endif
                if (tuple.op == TupleOp::INSERT) {
                    env.spawn(std::make_unique<bt::InsertValueTask<std::uint64_t, std::int64_t, RequestContainer>>(
                        tuple.key(), tuple.value(), target_tree, res, nullptr
#ifdef MEASURE_LATENCY
                        , start_time
#endif
                    ), target_channel);
                } else if (tuple.op == TupleOp::LOOKUP) {
                    env.spawn(std::make_unique<bt::LookupTask<std::uint64_t, std::int64_t, RequestContainer>>(
                        tuple.key(), res, target_tree, nullptr
#ifdef MEASURE_LATENCY
                        , start_time
#endif
                    ), target_channel);
                } else if (tuple.op == TupleOp::UPDATE) {
                    env.spawn(std::make_unique<bt::UpdateTask<std::uint64_t, std::int64_t, RequestContainer>>(
                        tuple.key(), tuple.value(), res, target_tree, nullptr
#ifdef MEASURE_LATENCY
                        , start_time
#endif
                    ), target_channel);
                }
            }
#endif
            env.spawn(std::make_unique<RequestSchedulerTask>(*this), thread_id);
        } else {
            if (!next_reqs.is_finished()) {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                db::index::blinktree::BLinkTreeScheduler::get().spawn(
                    std::make_unique<RequestSchedulerTask>(*this), thread_id);
            }
        }
    }

private:
#if !defined(MODE_CXL_HYBRID) && !defined(MODE_CXL_FULL)
    // One batch on host threads, mirroring process_cxl_batch: dedup (not timed), parallel locate of
    // every distinct key, in-order replay (not timed), one write per changed key in parallel, report.
    // Host cores are cache-coherent, so there is no hand-off and several threads may share a leaf.
    // Timed (g_cxl_stats): locate_host_ns + host_write_ns + report_ns.
    void process_host_batch(RequestContainer& res, std::uint64_t begin, std::uint64_t end, std::uint16_t thread_id) {
        namespace bt = db::index::blinktree;
        using Tree = bt::BLinkTree<std::uint64_t, std::int64_t>;
        Tree* tree = _trees.front();
        g_batch_helper_listener.container = &res;
        auto& stats = bt::g_cxl_stats;
        const std::size_t n = end - begin;
#ifdef MEASURE_LATENCY
        const auto start_time = bt::Clock::now();
#endif
        const auto t_batch = bt::prof_now_ns();
        auto t_mark = t_batch;
        auto phase = [&](std::atomic<std::uint64_t>& acc) {
            const auto now = bt::prof_now_ns();
            acc.fetch_add(now - t_mark, std::memory_order_relaxed);
            t_mark = now;
        };

        // dedup (not timed)
        std::unordered_map<std::uint64_t, std::uint32_t> slot_of;
        slot_of.reserve(n * 2);
        std::vector<std::uint64_t> keys;
        keys.reserve(n);
        std::vector<std::uint32_t> op_slot(n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto k = res[begin + i].key();
            auto [it, fresh] = slot_of.try_emplace(k, static_cast<std::uint32_t>(keys.size()));
            if (fresh) keys.push_back(k);
            op_slot[i] = it->second;
        }
        phase(stats.dedup_ns);

        // locate every distinct key in parallel (timed)
        struct KeyState { bool pre_exists; bool exists; bool dirty; std::int64_t value; };
        std::vector<KeyState> st(keys.size());
        parallel_for_workers(keys.size(), thread_id, [&](std::size_t b, std::size_t e) {
            for (std::size_t s = b; s < e; ++s) {
                bool found = false;
                std::int64_t v = 0;
                bt::locate_leaf(tree, keys[s], found, v);
                st[s] = {found, found, false, v};
            }
        });
        stats.host_locates.fetch_add(keys.size(), std::memory_order_relaxed);
        phase(stats.locate_host_ns);

        // replay in batch order (not timed)
        enum class Outcome : std::uint8_t { INSERTED, UPDATED, FOUND, MISSING };
        std::vector<Outcome> outcome(n);
        std::vector<std::int64_t> out_val(n, 0);
        for (std::size_t i = 0; i < n; ++i) {
            const auto& t = res[begin + i];
            auto& k = st[op_slot[i]];
            if (t.op == TupleOp::INSERT) {
                k.exists = true; k.dirty = true; k.value = t.value();
                outcome[i] = Outcome::INSERTED; out_val[i] = t.value();
            } else if (t.op == TupleOp::UPDATE) {
                if (k.exists) {
                    k.dirty = true; k.value = t.value();
                    outcome[i] = Outcome::UPDATED; out_val[i] = t.value();
                } else {
                    outcome[i] = Outcome::MISSING;
                }
            } else {
                if (k.exists) { outcome[i] = Outcome::FOUND; out_val[i] = k.value; }
                else outcome[i] = Outcome::MISSING;
            }
        }
        std::vector<std::uint32_t> dirty;
        for (std::uint32_t s = 0; s < keys.size(); ++s) if (st[s].dirty) dirty.push_back(s);
        phase(stats.replay_ns);

        // one write per changed key, in parallel (timed); host latches order concurrent leaf writers
        parallel_for_workers(dirty.size(), thread_id, [&](std::size_t b, std::size_t e) {
            for (std::size_t j = b; j < e; ++j) {
                const auto s = dirty[j];
                if (st[s].pre_exists) {
                    bt::UpdateTask<std::uint64_t, std::int64_t, BatchHelperListener> task(
                        keys[s], st[s].value, g_batch_helper_listener, tree, nullptr);
                    task.execute(thread_id);
                } else {
                    bt::InsertValueTask<std::uint64_t, std::int64_t, BatchHelperListener> task(
                        keys[s], st[s].value, tree, g_batch_helper_listener, nullptr);
                    task.execute(thread_id);
                }
            }
        });
        stats.device_writes.fetch_add(dirty.size(), std::memory_order_relaxed); // (host writes, same counter)
        phase(stats.host_write_ns);

        // report in batch order (timed)
        for (std::size_t i = 0; i < n; ++i) {
            const auto key = res[begin + i].key();
            switch (outcome[i]) {
                case Outcome::INSERTED: res.inserted(thread_id, key, out_val[i]
#ifdef MEASURE_LATENCY
                                                     , start_time
#endif
                                                     ); break;
                case Outcome::UPDATED: res.updated(thread_id, key, out_val[i]
#ifdef MEASURE_LATENCY
                                                   , start_time
#endif
                                                   ); break;
                case Outcome::FOUND: res.found(thread_id, key, out_val[i]
#ifdef MEASURE_LATENCY
                                               , start_time
#endif
                                               ); break;
                case Outcome::MISSING: res.missing(thread_id, key
#ifdef MEASURE_LATENCY
                                                   , start_time
#endif
                                                   ); break;
            }
        }
        phase(stats.report_ns);
        stats.batch_ns.fetch_add(bt::prof_now_ns() - t_batch, std::memory_order_relaxed);
    }
#endif

#if defined(MODE_CXL_HYBRID) || defined(MODE_CXL_FULL)
    // One batch on the device, in three steps:
    //  1. locate: a read-only pass over every distinct key (device lookup kernel in full mode,
    //     host descent in hybrid) gives found/value and the leaf, all from the pre-batch tree;
    //  2. replay the batch's ops in order on those results: this is each op's sequential answer
    //     (later ops on a key see earlier ones in the batch: request combining);
    //  3. one device write per changed key with its final value. Writes are grouped by leaf and
    //     each leaf gets a single owner task, so several MU tasks never write the same leaf.
    // Results are reported in batch order.
    void process_cxl_batch(RequestContainer& res, std::uint64_t begin, std::uint64_t end, std::uint16_t thread_id) {
        namespace bt = db::index::blinktree;
        using Tree = bt::BLinkTree<std::uint64_t, std::int64_t>;
        auto& env = bt::BLinkTreeScheduler::get();
        Tree* tree = _trees.front(); // every shard points to the same tree (main.cpp)
        g_batch_helper_listener.container = &res;

        const std::size_t n = end - begin;
#ifdef MEASURE_LATENCY
        const auto start_time = bt::Clock::now();
#endif
        auto& stats = bt::g_cxl_stats;
        const auto t_batch = bt::prof_now_ns();
        auto t_mark = t_batch;
        auto phase = [&](std::atomic<std::uint64_t>& acc) {
            const auto now = bt::prof_now_ns();
            acc.fetch_add(now - t_mark, std::memory_order_relaxed);
            t_mark = now;
        };

        // distinct keys in first-appearance order
        std::unordered_map<std::uint64_t, std::uint32_t> slot_of;
        slot_of.reserve(n * 2);
        std::vector<std::uint64_t> keys;
        keys.reserve(n);
        std::vector<std::uint32_t> op_slot(n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto k = res[begin + i].key();
            auto [it, fresh] = slot_of.try_emplace(k, static_cast<std::uint32_t>(keys.size()));
            if (fresh) keys.push_back(k);
            op_slot[i] = it->second;
        }
        phase(stats.dedup_ns);

        // Host separator tasks modify inner nodes with host atomics, which are not atomic against
        // device AMOs: let them drain first. They may be queued on this worker, so run our queue.
        while (res.pending_separators() > 0) {
            if (!env.try_run_one(thread_id)) std::this_thread::yield();
        }
        phase(stats.drain_ns);

        // 1. locate
        struct KeyState { bool pre_exists; bool exists; bool dirty; std::int64_t value; std::uint64_t leaf; };
        std::vector<KeyState> st(keys.size());
        const bool no_combine = bt::g_cxl_no_combine;
        // locate requests: one per distinct key, or one per op with --no-combine (the first op of
        // each key supplies its pre-batch state; the rest are the extra device work being measured)
        std::vector<std::uint64_t> locate_keys;
        if (no_combine) {
            locate_keys.resize(n);
            for (std::size_t i = 0; i < n; ++i) locate_keys[i] = res[begin + i].key();
        }
        const std::vector<std::uint64_t>& lkeys = no_combine ? locate_keys : keys;
        std::vector<char> slot_done(no_combine ? keys.size() : 0, 0);
#if defined(MODE_CXL_FULL)
        {
            std::vector<CXLLookupRequest> lk(lkeys.size());
            for (std::size_t r = 0; r < lkeys.size(); ++r) {
                lk[r] = CXLLookupRequest{};
                lk[r].key = lkeys[r];
            }
            bt::process_cxl_lookup_batch(lk, tree->root(), g_pxl_num_tasks);
            for (std::size_t r = 0; r < lkeys.size(); ++r) {
                if (lk[r].status != 1 && lk[r].status != 2) {
                    std::cerr << "❌ [FATAL ERROR] CXL lookup kernel failed! key: " << lk[r].key
                              << ", status: " << static_cast<int>(lk[r].status) << "\n";
                    std::exit(1);
                }
                const std::size_t s = no_combine ? op_slot[r] : r;
                if (no_combine) { if (slot_done[s]) continue; slot_done[s] = 1; }
                const bool found = lk[r].status == 1;
                st[s] = {found, found, false, found ? lk[r].value : 0, lk[r].leaf};
            }
        }
#else
        // Parallel locate on all --threads workers (BLINK_LOCATE_PARALLEL=0: one thread). Combining
        // mode only (--no-combine keeps the single-thread path below).
        static const bool par = !(std::getenv("BLINK_LOCATE_PARALLEL") && std::string(std::getenv("BLINK_LOCATE_PARALLEL")) == "0") && !no_combine;
        if (par) {
            parallel_for_workers(lkeys.size(), thread_id, [&](std::size_t b, std::size_t e) {
                for (std::size_t r = b; r < e; ++r) {
                    bool found = false;
                    std::int64_t v = 0;
                    auto* leaf = bt::locate_leaf(tree, lkeys[r], found, v);
                    st[r] = {found, found, false, v, reinterpret_cast<std::uint64_t>(leaf)};
                }
            });
        } else
        for (std::size_t r = 0; r < lkeys.size(); ++r) {
            bool found = false;
            std::int64_t v = 0;
            auto* leaf = bt::locate_leaf(tree, lkeys[r], found, v);
            const std::size_t s = no_combine ? op_slot[r] : r;
            if (no_combine) { if (slot_done[s]) continue; slot_done[s] = 1; }
            st[s] = {found, found, false, v, reinterpret_cast<std::uint64_t>(leaf)};
        }
        bt::g_cxl_stats.host_locates.fetch_add(lkeys.size(), std::memory_order_relaxed);
        // Drop every leaf the host just read from the host cache before the device writes. Without
        // this, ~40% of hybrid runs lost device updates (2026-10-17 soak: 10/24; full mode 0/24): host
        // copies of tree lines left by these reads were later written back over the device's updates.
        // With it: 0 lost in every run, and no invalidation is needed BEFORE reading (the copies are
        // gone, so the next batch's reads come from memory). BLINK_POST_LOCATE_INVAL=0 disables it.
        static const bool post_inval = !(std::getenv("BLINK_POST_LOCATE_INVAL") && std::string(std::getenv("BLINK_POST_LOCATE_INVAL")) == "0");
        if (post_inval) {
            const auto t_inv = bt::prof_now_ns();
            // Each leaf once: mark leaves in a bitmap over their 4 KiB-aligned address range, then
            // flush the marked ones; both steps on all workers (a single-threaded sort + flush was
            // ~690 ms per 8M ops, the parallel flush after a sort ~306 ms).
            constexpr std::uint64_t kNode = sizeof(CXLNode);
            std::uint64_t lo = ~0ULL, hi = 0;
            for (const auto& k : st) if (k.leaf) { lo = std::min(lo, k.leaf); hi = std::max(hi, k.leaf); }
            const std::uint64_t nbits = lo <= hi ? (hi - lo) / kNode + 1 : 0;
            if (nbits > 0 && nbits <= (1ULL << 27)) {               // <= 16 MiB bitmap (512 GiB span)
                const std::size_t nwords = (nbits + 63) / 64;
                std::unique_ptr<std::atomic<std::uint64_t>[]> bits(new std::atomic<std::uint64_t>[nwords]());
                parallel_for_workers(st.size(), thread_id, [&](std::size_t b, std::size_t e) {
                    for (std::size_t j = b; j < e; ++j) {
                        if (!st[j].leaf) continue;
                        const std::uint64_t i = (st[j].leaf - lo) / kNode;
                        bits[i >> 6].fetch_or(1ULL << (i & 63), std::memory_order_relaxed);
                    }
                });
                parallel_for_workers(nwords, thread_id, [&](std::size_t b, std::size_t e) {
                    for (std::size_t w = b; w < e; ++w) {
                        std::uint64_t m = bits[w].load(std::memory_order_relaxed);
                        while (m) {
                            const int bit = __builtin_ctzll(m);
                            m &= m - 1;
                            cxl_inval_cache_range(reinterpret_cast<const void*>(lo + ((w << 6) + bit) * kNode), kNode);
                        }
                    }
                });
            } else if (nbits > 0) {                                  // huge span: sort instead
                std::vector<std::uint64_t> leaves;
                leaves.reserve(st.size());
                for (const auto& k : st) if (k.leaf) leaves.push_back(k.leaf);
                std::sort(leaves.begin(), leaves.end());
                leaves.erase(std::unique(leaves.begin(), leaves.end()), leaves.end());
                parallel_for_workers(leaves.size(), thread_id, [&](std::size_t b, std::size_t e) {
                    for (std::size_t j = b; j < e; ++j)
                        cxl_inval_cache_range(reinterpret_cast<const void*>(leaves[j]), kNode);
                });
            }
            bt::g_cxl_stats.post_inval_ns.fetch_add(bt::prof_now_ns() - t_inv, std::memory_order_relaxed);
        }
#endif

        { // locate: device time is in launch/stage/readback; host descent in locate_host
            std::atomic<std::uint64_t> scratch{0};
#if defined(MODE_CXL_FULL)
            phase(scratch);
#else
            phase(stats.locate_host_ns);
#endif
        }

        // 2. replay in batch order
        enum class Outcome : std::uint8_t { INSERTED, UPDATED, FOUND, MISSING };
        std::vector<Outcome> outcome(n);
        std::vector<std::int64_t> out_val(n, 0);
        std::vector<std::uint8_t> op_write(n, 0xFF); // --no-combine: device write op for op i (0xFF: none)
        for (std::size_t i = 0; i < n; ++i) {
            const auto& t = res[begin + i];
            auto& k = st[op_slot[i]];
            if (t.op == TupleOp::INSERT) {
                k.exists = true; k.dirty = true; k.value = t.value();
                outcome[i] = Outcome::INSERTED; out_val[i] = t.value();
                op_write[i] = 0;
            } else if (t.op == TupleOp::UPDATE) {
                if (k.exists) {
                    k.dirty = true; k.value = t.value();
                    outcome[i] = Outcome::UPDATED; out_val[i] = t.value();
                    op_write[i] = 1;
                } else {
                    outcome[i] = Outcome::MISSING;
                }
            } else {
                if (k.exists) { outcome[i] = Outcome::FOUND; out_val[i] = k.value; }
                else outcome[i] = Outcome::MISSING;
            }
        }

        phase(stats.replay_ns);

        // 3. device writes, one owner task per leaf
        std::vector<CXLWriteRequest> writes;
        if (no_combine) {
            // every write op, in batch order (the stable sort below keeps same-key order per owner)
            for (std::size_t i = 0; i < n; ++i) {
                if (op_write[i] == 0xFF) continue;
                CXLWriteRequest w{};
                w.key = res[begin + i].key();
                w.value = out_val[i];
                w.op = op_write[i];
                w.leaf = st[op_slot[i]].leaf;
                writes.push_back(w);
            }
        } else {
            for (std::size_t s = 0; s < keys.size(); ++s) {
                if (!st[s].dirty) continue;
                CXLWriteRequest w{};
                w.key = keys[s];
                w.value = st[s].value;
                w.op = st[s].pre_exists ? 1 : 0;
                w.leaf = st[s].leaf;
                writes.push_back(w);
            }
        }
        if (!writes.empty()) {
            std::stable_sort(writes.begin(), writes.end(),
                             [](const CXLWriteRequest& a, const CXLWriteRequest& b) { return a.leaf < b.leaf; });
            const std::size_t tasks = std::max<std::uint32_t>(1, g_pxl_num_tasks);
            const std::size_t target = (writes.size() + tasks - 1) / tasks;
            std::uint16_t owner = 0;
            std::size_t in_owner = 0;
            std::vector<CXLWriteRequest> aligned;
            aligned.reserve(writes.size() + tasks);
            for (std::size_t i = 0; i < writes.size(); ++i) {
                const bool new_leaf = i == 0 || writes[i].leaf != writes[i - 1].leaf;
                if (new_leaf && in_owner >= target && owner + 1u < tasks) {
                    // the next owner's slice must start on a fresh cache line (2 records per line)
                    if (aligned.size() & 1) {
                        CXLWriteRequest pad{};
                        pad.op = 0xFF; pad.status = 4; pad.owner = owner;
                        aligned.push_back(pad);
                    }
                    ++owner; in_owner = 0;
                }
                writes[i].owner = owner;
                aligned.push_back(writes[i]);
                ++in_owner;
            }
            writes.swap(aligned);
            phase(stats.write_prep_ns);
            bt::process_cxl_write_batch(writes, tree->root(), owner + 1u);
            { std::atomic<std::uint64_t> scratch{0}; phase(scratch); } // write launch accounted in run_cxl_batch

            // Keys whose insert fell back to the host: their later writes in this batch (only possible
            // with --no-combine) missed on the device and are applied on the host, in order.
            std::unordered_map<std::uint64_t, char> on_host;
            for (const auto& w : writes) {
                if (w.op == 0xFF) continue;
                if (on_host.count(w.key)) {
                    bt::InsertValueTask<std::uint64_t, std::int64_t, BatchHelperListener> task(
                        w.key, w.value, tree, g_batch_helper_listener, nullptr);
                    task.execute(thread_id);
                    continue;
                }
                if (w.status == 1) continue;
                if (w.op == 0 && w.status == 3) {
                    // leaf full: the host splits it and inserts (results are reported below)
                    bt::InsertValueTask<std::uint64_t, std::int64_t, BatchHelperListener> task(
                        w.key, w.value, tree, g_batch_helper_listener, nullptr);
                    task.execute(thread_id);
                    bt::g_cxl_stats.host_split_inserts.fetch_add(1, std::memory_order_relaxed);
                    on_host[w.key] = 1;
                    continue;
                }
                std::cerr << "❌ [FATAL ERROR] CXL write kernel disagrees with locate! key: " << w.key
                          << ", op: " << static_cast<int>(w.op) << ", status: " << static_cast<int>(w.status) << "\n";
                std::exit(1);
            }
        }

        phase(stats.split_ns);

        // report in batch order
        for (std::size_t i = 0; i < n; ++i) {
            const auto key = res[begin + i].key();
            switch (outcome[i]) {
                case Outcome::INSERTED: res.inserted(thread_id, key, out_val[i]
#ifdef MEASURE_LATENCY
                                                     , start_time
#endif
                                                     ); break;
                case Outcome::UPDATED: res.updated(thread_id, key, out_val[i]
#ifdef MEASURE_LATENCY
                                                   , start_time
#endif
                                                   ); break;
                case Outcome::FOUND: res.found(thread_id, key, out_val[i]
#ifdef MEASURE_LATENCY
                                               , start_time
#endif
                                               ); break;
                case Outcome::MISSING: res.missing(thread_id, key
#ifdef MEASURE_LATENCY
                                                   , start_time
#endif
                                                   ); break;
            }
        }
        phase(stats.report_ns);
        stats.batch_ns.fetch_add(bt::prof_now_ns() - t_batch, std::memory_order_relaxed);
    }
#endif

    std::uint16_t _channel_id;
    std::shared_ptr<RequestContainer> _container;
    std::vector<db::index::blinktree::BLinkTree<std::uint64_t, std::int64_t>*> _trees;
};

// Partitioned host dispatch (--host-dispatch=partitioned): every worker walks the workload in
// chunks and runs, inline on its own thread, the ops whose key maps to it (key % workers). One
// worker per key keeps per-key order; there is no central dispatcher or per-request task. After
// each chunk the task requeues itself so separator tasks queued on this worker get to run.
class HostPartitionTask final : public ::Task {
public:
    static constexpr std::uint64_t chunk = 4096;

    // indices == nullptr: scan [cursor, end) of the workload and skip other workers' keys (every worker
    // reads every op: --host-dispatch=scan). Otherwise walk this worker's pre-built index list.
    HostPartitionTask(std::uint16_t worker, std::uint16_t workers, std::shared_ptr<RequestContainer> container,
                      db::index::blinktree::BLinkTree<std::uint64_t, std::int64_t>* tree,
                      std::uint64_t cursor, std::uint64_t end,
                      std::shared_ptr<const std::vector<std::uint64_t>> indices = nullptr)
        : _worker(worker), _workers(workers), _container(std::move(container)), _tree(tree), _cursor(cursor), _end(end),
          _indices(std::move(indices)) {}

    void execute(std::uint16_t thread_id) override {
        namespace bt = db::index::blinktree;
        auto& res = *_container;
        const std::uint64_t stop = std::min(_cursor + chunk, _end);
        for (std::uint64_t j = _cursor; j < stop; ++j) {
            const std::uint64_t i = _indices ? (*_indices)[j] : j;
            const auto& t = res[i];
            if (!_indices && t.key() % _workers != _worker) continue;
#ifdef MEASURE_LATENCY
            const auto start_time = bt::Clock::now();
#endif
            if (t.op == TupleOp::INSERT) {
                bt::InsertValueTask<std::uint64_t, std::int64_t, RequestContainer> task(t.key(), t.value(), _tree, res, nullptr
#ifdef MEASURE_LATENCY
                    , start_time
#endif
                );
                task.execute(thread_id);
            } else if (t.op == TupleOp::LOOKUP) {
                bt::LookupTask<std::uint64_t, std::int64_t, RequestContainer> task(t.key(), res, _tree, nullptr
#ifdef MEASURE_LATENCY
                    , start_time
#endif
                );
                task.execute(thread_id);
            } else {
                bt::UpdateTask<std::uint64_t, std::int64_t, RequestContainer> task(t.key(), t.value(), res, _tree, nullptr
#ifdef MEASURE_LATENCY
                    , start_time
#endif
                );
                task.execute(thread_id);
            }
        }
        _cursor = stop;
        if (_cursor < _end) {
            bt::BLinkTreeScheduler::get().spawn(std::make_unique<HostPartitionTask>(*this), thread_id);
        }
    }

private:
    std::uint16_t _worker;
    std::uint16_t _workers;
    std::shared_ptr<RequestContainer> _container;
    db::index::blinktree::BLinkTree<std::uint64_t, std::int64_t>* _tree;
    std::uint64_t _cursor;
    std::uint64_t _end;
    std::shared_ptr<const std::vector<std::uint64_t>> _indices;
};

} // namespace application::blinktree_benchmark
