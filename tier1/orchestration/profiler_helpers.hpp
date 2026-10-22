// SPDX-License-Identifier: Apache-2.0
// XPTI profiler aggregation utilities for the orchestration_tax suite.
//
// Wraps xpti::startProcessSession + delta queries so each test can:
//   1. mark a baseline offset before running its REPS,
//   2. read only the events generated during the test,
//   3. emit per-stage CSV breakdowns alongside the wall-clock CSV.
//
// Stage attribution (DeviceEvent.type, see xpti_types.hpp::muprobe::ProbeDataType):
//   Launch    — runtime told the device to start  (DeviceInit)
//   Start     — kernel actually started executing  (Request → kernel entry)
//   End       — kernel finished                    (kernel exit)
//   Terminate — runtime acknowledged completion    (DeviceFinalize)
//
// HostEvent pairs Enter/Exit on functionName.  We mostly care about the
// PXL public API entries (execute, synchronize) for host-side validation.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "xpti/xpti.hpp"
#include "xpti/xpti_types.hpp"

#include "stats.hpp"

namespace prof
{

// ── Host event subscriber ────────────────────────────────────────────────────
//
// PXL public APIs (Map::execute, Map::synchronize, Context::memAlloc, …) are
// wrapped with XPTI_PUBLIC_API_SCOPE() which emits to XPTI_CB_DOMAIN_HOST_API.
// The auto-enabled (XPTI_ENABLE=1) session subscribes only to the device
// probe domain, so HostEvent queries against that session return empty.
//
// We bridge that gap here by registering our own subscriber on HOST_API and
// USER domains and accumulating CallbackData into a thread-safe buffer that
// imitates xpti::HostEvent.  Tests then drain the buffer using a cursor.

struct HostEventStore
{
    std::mutex mu;
    std::vector<xpti::HostEvent> events;
};

inline HostEventStore& host_store()
{
    static HostEventStore s;
    return s;
}

inline void host_event_callback(void* /*userdata*/,
                                xpti::CallbackDomain /*domain*/,
                                xpti::CallbackId /*cbid*/,
                                const xpti::CallbackData* d)
{
    if (!d) return;
    xpti::HostEvent e{};
    e.site         = d->site;
    e.functionName = d->functionName ? d->functionName : "";
    e.timestamp    = d->timestamp;
    e.pid          = d->pid;
    e.tid          = d->tid;
    e.taskIdx      = d->taskIdx;
    e.profileKey   = d->profileKey;

    HostEventStore& s = host_store();
    std::lock_guard<std::mutex> lk(s.mu);
    s.events.push_back(std::move(e));
}

// Register our subscriber on HOST_API + USER domains.  Returns the handle so
// the caller can keep it alive for the life of the process (no-op cleanup).
inline xpti::SubscriberHandle subscribe_host_events()
{
    xpti::SubscriberHandle h{};
    xpti::subscribe(&h, host_event_callback, nullptr);
    // subId=0 because we want events from any sub.
    xpti::enableCallback(0, h, xpti::XPTI_CB_DOMAIN_HOST_API,
                         xpti::CallbackId::Generic);
    xpti::enableCallback(0, h, xpti::XPTI_CB_DOMAIN_USER,
                         xpti::CallbackId::Generic);
    return h;
}

// Returns events appended since `last_offset` and updates the offset in place.
inline std::vector<xpti::HostEvent>
drain_host_events_since(uint64_t& last_offset)
{
    HostEventStore& s = host_store();
    std::lock_guard<std::mutex> lk(s.mu);
    if (last_offset > s.events.size())
        last_offset = s.events.size();
    std::vector<xpti::HostEvent> out(
        s.events.begin() + last_offset,
        s.events.end());
    last_offset = s.events.size();
    return out;
}

// ── Session lifecycle ────────────────────────────────────────────────────────

// Starts a process-level XPTI session AND registers our host-event subscriber.
// Call once at the top of main_profiler::main().
//
// Empty `enabledProbeTypes` = all probes enabled (Mu, L1, L2, L3, MTC, PCU,
// Gaia, HostEvent, DeviceEvent — see xpti_types.hpp ProbeType enum).
inline uint64_t start_session(uint32_t deviceId = 0)
{
    xpti::ProfileConfig cfg;
    cfg.mode               = xpti::ProfileMode::HostAndDevice;
    cfg.csvAppendTimestamp = false;
    xpti::startProcessSession(cfg, deviceId);
    subscribe_host_events();  // captures HOST_API + USER domain events
    return xpti::getProcessSessionId();
}

inline void end_session()
{
    xpti::endProcessSession();
}

// ── Delta query helpers ──────────────────────────────────────────────────────
//
// The process session accumulates events forever, so each test must capture
// "what was new during the timed loop".  XPTI supports this via session-data
// delta queries that take a per-call lastReadOffset and update it in place.

struct Cursor
{
    uint64_t hostOffset   = 0;  // index into prof::HostEventStore (our subscriber)
    uint64_t deviceOffset = 0;
    uint64_t l1Offset     = 0;
    uint64_t muOffset     = 0;
};

// Reads new HostEvents from our own subscriber buffer.  The XPTI session
// query (EventsHost) does not pick up HOST_API domain events, so we maintain
// our own store via subscribe_host_events().
inline std::vector<xpti::HostEvent>
read_host_events_delta(uint64_t /*sessionId*/, Cursor& cur)
{
    return drain_host_events_since(cur.hostOffset);
}

inline std::vector<xpti::DeviceEvent>
read_device_events_delta(uint64_t sessionId, Cursor& cur)
{
    auto data = xpti::getSessionDataDelta<xpti::QueryKey::EventsDevice>(
        sessionId, cur.deviceOffset);
    return data.data();
}

inline std::vector<xpti::L1RawData>
read_l1_delta(uint64_t sessionId, Cursor& cur)
{
    auto data = xpti::getSessionDataDelta<xpti::QueryKey::MetricsRawL1>(
        sessionId, cur.l1Offset);
    return data.data();
}

inline std::vector<xpti::MURawData>
read_mu_delta(uint64_t sessionId, Cursor& cur)
{
    auto data = xpti::getSessionDataDelta<xpti::QueryKey::MetricsRawMU>(
        sessionId, cur.muOffset);
    return data.data();
}

// ── Stage aggregation ────────────────────────────────────────────────────────

// Builds per-rep stage durations from a flat DeviceEvent stream.
// Stages emitted (each in microseconds, MU-clock domain):
//   launch_to_start      Launch → Start    (device-side init)
//   start_to_end         Start  → End      (actual kernel exec)
//   end_to_terminate     End    → Terminate (device-side completion)
//   launch_to_terminate  Launch → Terminate (full device cycle)
//
// Events are grouped by (subId, cluster, mu, thread, key) — `key` identifies
// the dispatch instance.  If a rep is incomplete (e.g. only Launch present)
// it is skipped.
struct DeviceStages
{
    std::vector<double> launch_to_start;
    std::vector<double> start_to_end;
    std::vector<double> end_to_terminate;
    std::vector<double> launch_to_terminate;
};

inline DeviceStages aggregate_device_events(
    const std::vector<xpti::DeviceEvent>& evs,
    double mu_freq_mhz = 1000.0)
{
    DeviceStages out;
    if (evs.empty()) return out;

    // Group events by (subId, cluster, mu, thread, key).
    struct Key {
        uint32_t sub, cluster, mu, thread, key;
        bool operator<(const Key& o) const {
            return std::tie(sub, cluster, mu, thread, key)
                 < std::tie(o.sub, o.cluster, o.mu, o.thread, o.key);
        }
    };
    struct Bundle {
        uint64_t launch = 0, start = 0, end = 0, terminate = 0;
        bool has_launch = false, has_start = false, has_end = false, has_term = false;
    };
    std::map<Key, Bundle> groups;

    for (const auto& e : evs)
    {
        Key k{ e.subId, e.cluster, e.mu, e.thread, e.key };
        auto& b = groups[k];
        switch (e.type)
        {
            case xpti::muprobe::ProbeDataType::Launch:
                b.launch = e.muTimeStamp;    b.has_launch = true; break;
            case xpti::muprobe::ProbeDataType::Start:
                b.start  = e.muTimeStamp;    b.has_start  = true; break;
            case xpti::muprobe::ProbeDataType::End:
                b.end    = e.muTimeStamp;    b.has_end    = true; break;
            case xpti::muprobe::ProbeDataType::Terminate:
                b.terminate = e.muTimeStamp; b.has_term   = true; break;
        }
    }

    // Convert MU cycles → microseconds (cycles / freq_MHz).
    auto to_us = [mu_freq_mhz](uint64_t a, uint64_t b) -> double {
        if (a >= b) return 0.0;
        return static_cast<double>(b - a) / mu_freq_mhz;
    };

    for (const auto& [_k, b] : groups)
    {
        if (b.has_launch && b.has_start)
            out.launch_to_start.push_back(to_us(b.launch, b.start));
        if (b.has_start && b.has_end)
            out.start_to_end.push_back(to_us(b.start, b.end));
        if (b.has_end && b.has_term)
            out.end_to_terminate.push_back(to_us(b.end, b.terminate));
        if (b.has_launch && b.has_term)
            out.launch_to_terminate.push_back(to_us(b.launch, b.terminate));
    }
    return out;
}

// Builds per-function name stage durations from a flat HostEvent stream.
// Pairs adjacent Enter/Exit events on the same (tid, functionName).
//
// Returns: name → vector of durations (us).
inline std::map<std::string, std::vector<double>>
aggregate_host_events(const std::vector<xpti::HostEvent>& evs)
{
    // Per-thread, per-name stack of pending Enters.
    std::map<std::pair<uint32_t, std::string>, std::vector<uint64_t>> pending;
    std::map<std::string, std::vector<double>> out;

    for (const auto& e : evs)
    {
        auto key = std::make_pair(e.tid, e.functionName);
        if (e.site == xpti::CallbackSite::Enter)
        {
            pending[key].push_back(e.timestamp);
        }
        else  // Exit
        {
            auto it = pending.find(key);
            if (it != pending.end() && !it->second.empty())
            {
                uint64_t enter_ts = it->second.back();
                it->second.pop_back();
                if (e.timestamp >= enter_ts)
                    out[e.functionName].push_back(
                        static_cast<double>(e.timestamp - enter_ts));
                // Host event timestamps are in microseconds (xpti.hpp impl).
            }
        }
    }
    return out;
}

// ── Activation introspection ─────────────────────────────────────────────────

// Returns the set of (subId, cluster) tuples that had at least one L1 sample
// during the captured window.  Used by T5/T8 to validate locality / bitmap.
inline std::vector<std::pair<uint32_t, uint32_t>>
active_clusters_from_l1(const std::vector<xpti::L1RawData>& rows)
{
    std::vector<std::pair<uint32_t, uint32_t>> out;
    out.reserve(rows.size());
    for (const auto& r : rows)
        out.emplace_back(r.subId, r.cluster);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Returns the set of (subId, cluster, mu, thread) tuples observed in MU stats.
// Used by T7 to verify that batchSize actually reduced active MU thread count.
struct MuKey
{
    uint32_t sub, cluster, mu, thread;
    bool operator<(const MuKey& o) const {
        return std::tie(sub, cluster, mu, thread)
             < std::tie(o.sub, o.cluster, o.mu, o.thread);
    }
};
inline std::vector<MuKey>
active_mus_from_mu_raw(const std::vector<xpti::MURawData>& rows)
{
    std::vector<MuKey> out;
    out.reserve(rows.size());
    for (const auto& r : rows)
        out.push_back({ r.subId, r.cluster, r.mu, r.thread });
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end(),
        [](const MuKey& a, const MuKey& b) {
            return std::tie(a.sub, a.cluster, a.mu, a.thread)
                == std::tie(b.sub, b.cluster, b.mu, b.thread);
        }), out.end());
    return out;
}

// ── Host event breakdown ─────────────────────────────────────────────────────
//
// PXL public APIs are wrapped with XPTI_PUBLIC_API_SCOPE() (xpti.hpp), so each
// call to e.g. Map::execute / Map::synchronize / Context::memAlloc emits an
// Enter/Exit HostEvent pair.  aggregate_host_events() pairs them into per-API
// duration vectors; this helper turns that into a CSV breakdown.
//
// HostEvent.functionName comes from __PRETTY_FUNCTION__, which is verbose
// (full return type, arg list, etc.).  short_name() trims it to the last
// "Class::method" or "function" segment for readability.

inline std::string short_name(const std::string& full)
{
    auto paren = full.find('(');
    std::string head = (paren == std::string::npos) ? full : full.substr(0, paren);
    auto last_space = head.rfind(' ');
    if (last_space != std::string::npos)
        head = head.substr(last_space + 1);
    while (!head.empty() && (head.front() == '*' || head.front() == '&'))
        head.erase(head.begin());
    auto colon2 = head.rfind("::");
    if (colon2 != std::string::npos)
    {
        auto colon1 = head.rfind("::", colon2 - 1);
        if (colon1 != std::string::npos)
            return head.substr(colon1 + 2);
    }
    return head;
}

// Aggregates host events by (cleaned) function name and emits one CSV row
// per API.  `row_prefix` is prepended to each row so callers can distinguish
// configurations (taskCount, mode, K, etc.).
inline void print_host_breakdown(const char* row_prefix,
                                 const std::vector<xpti::HostEvent>& evs)
{
    auto buckets = aggregate_host_events(evs);
    if (buckets.empty()) return;
    for (auto& [name, samples] : buckets)
    {
        std::vector<double> copy = samples;  // Stats sorts in place
        auto s = Stats::from(copy);
        printf("%s,\"%s\",%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%zu\n",
               row_prefix, short_name(name).c_str(),
               s.mean, s.stddev, s.min, s.p50, s.p95, s.p99, s.max, s.n);
    }
}

// ── PXL getStats() decomposition ─────────────────────────────────────────────
//
// map->getStats() is a PXL API (pxl/runtime/map.hpp), NOT XPTI: it returns the
// runtime's own per-dispatch phase accounting and costs nothing.  Unlike the
// XPTI device stages it needs no profiling session, so it is valid in runs
// where wall clock must stay undistorted.
//
// Emits one CSV row per named phase:
//   <prefix>,<phase>,<avg_us>,<min_us>,<max_us>
// plus the computation span and computeUtilization.

inline void print_getstats_csv(const char* row_prefix, const std::string& js)
{
    if (js.empty()) return;
    auto num_after = [&](size_t from, const char* key) -> double {
        size_t k = js.find(key, from);
        if (k == std::string::npos) return -1.0;
        size_t c = js.find(':', k);
        if (c == std::string::npos) return -1.0;
        return strtod(js.c_str() + c + 1, nullptr);
    };
    // Named elapsedTimes entries.
    size_t pos = 0;
    while ((pos = js.find("{\"name\":", pos)) != std::string::npos)
    {
        size_t q1 = js.find('"', pos + 8), q2 = js.find('"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) break;
        std::string name = js.substr(q1 + 1, q2 - q1 - 1);
        double mx = num_after(q2, "\"max\"");
        double mn = num_after(q2, "\"min\"");
        double av = num_after(q2, "\"avg\"");
        printf("GS,%s,%s,%.1f,%.1f,%.1f\n", row_prefix, name.c_str(), av, mn, mx);
        pos = q2 + 1;
    }
    size_t comp = js.find("\"computation\"");
    if (comp != std::string::npos)
        printf("GS,%s,computation,%.1f,%.1f,%.1f\n", row_prefix,
               num_after(comp, "\"avg\""), num_after(comp, "\"min\""), num_after(comp, "\"max\""));
    size_t cu = js.find("\"computeUtilization\"");
    if (cu != std::string::npos)
        printf("GS,%s,computeUtilization_pct,%.2f,%.2f,%.2f\n", row_prefix,
               num_after(cu, "\"avg\""), num_after(cu, "\"min\""), num_after(cu, "\"max\""));
}

// C1: every getStats row carries a literal "GS" sentinel in column 0 so it
// can never be confused with a wall-clock row (which leads with an integer
// key such as taskCount / numSub / batchSize).  Parse with:  awk -F, '$1=="GS"'
inline const char* getstats_csv_header()
{
    return "tag,key,phase,avg_us,min_us,max_us";
}

inline const char* host_csv_header()
{
    return "api_function,mean_us,std_us,min_us,p50_us,p95_us,p99_us,max_us,n";
}

// ── CSV helpers ──────────────────────────────────────────────────────────────

inline void print_stage_csv(const char* prefix,
                            const char* stage,
                            std::vector<double>& samples)
{
    if (samples.empty())
    {
        printf("%s,%s,nan,nan,nan,nan,nan,nan,nan,0\n", prefix, stage);
        return;
    }
    auto s = Stats::from(samples);
    printf("%s,%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%zu\n",
           prefix, stage,
           s.mean, s.stddev, s.min, s.p50, s.p95, s.p99, s.max, s.n);
}

inline void print_device_stages(const char* row_prefix, DeviceStages& ds)
{
    print_stage_csv(row_prefix, "launch_to_start",     ds.launch_to_start);
    print_stage_csv(row_prefix, "start_to_end",        ds.start_to_end);
    print_stage_csv(row_prefix, "end_to_terminate",    ds.end_to_terminate);
    print_stage_csv(row_prefix, "launch_to_terminate", ds.launch_to_terminate);
}

// ── Device freq lookup ───────────────────────────────────────────────────────
// MU clock frequency for converting muTimeStamp deltas to microseconds.
// Reads from xpti::DeviceInfo if available; falls back to 1000 MHz (1 GHz).

inline double mu_freq_mhz(uint64_t sessionId)
{
    auto info = xpti::getDeviceInfo(sessionId);
    if (info.muFrequency > 0)  return static_cast<double>(info.muFrequency);
    if (info.coreFrequency > 0) return static_cast<double>(info.coreFrequency);
    return 1000.0;
}

}  // namespace prof
