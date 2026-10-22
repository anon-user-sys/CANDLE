# Coherence-tax results — full suite 2026-10-10

Data: `results/suite_20261010_135135/` (all 11 tests rc=0; canary OK before the suite and after
every test). Configuration: mx_dma kthreads on CPUs 8,9; node 0; default C-states. Host flush =
`pxl::flushHostCache(opt=true)` unless stated. Device cycles are MU cycles (nominal 1.4 GHz).

## Cost of one handoff (k1, k2, k3, k9, k11)
- **Model fits** gamma(n) = gamma_call + lines * gamma_line for 64 KiB-4 MiB (R^2 >= 0.9999):
  per line **7.19 ns dirty (8.9 GB/s), 6.30 clean (10.2 GB/s), 4.00 absent (16.0 GB/s)**.
  Absent intercept 450 ns = measured 64 B call (447). Dirty/clean small ranges sit BELOW the line
  fit (64 B: 691 ns dirty, 123 clean, 447 absent), so quote the model from ~64 KiB up.
- **State held only to ~4 MiB per thread:** at 16 MiB dirty/clean per-line costs fall to 4.8/4.6
  (near absent 4.0); >= 64 MiB all states ~3.5 ns/line. (Setup text said 64 MiB: wrong.)
- **64 B single call:** dirty 691 ns, clean 123, absent 447. Store + flush of a 64 B descriptor
  **739 ns** (retest of the resident-worker measurement's 0.9 us).
- **clflush vs clflushopt:** same for one line; at 1 MiB clflush (+mfence) is 37x (dirty),
  11x (clean), 93x (absent) slower — clflush serializes (~277 ns/line dirty).
- **flushHostCache(opt=false)** = clflush loop WITHOUT the trailing mfence (objdump of libpxl):
  returns in ~50 ns for <= 1 KiB before the flush completes; >= 16 KiB as slow as clflush. Unsafe
  as a publish without a caller fence, and slower.
- **Library overhead:** none (lib vs raw clflushopt within 1%).
- **Dirty fraction (k2, 16 MiB):** cost set by range size: 1.05 ms all-absent, 1.22 ms
  all-clean, 1.31-1.34 ms all-dirty. 1% dirty (2,621 lines) = 1.06 ms vs ~19 us if only those lines
  were flushed (model) -> a dirty-line map would save ~98%. Clustered vs uniform: small difference.
- **Threads (k3):** dirty write-back saturates **26.7 GB/s at 8 threads** (21 at 16-32); clean
  scales to **95 GB/s at 32**; absent plateaus **55-58 GB/s** at 8+ (= Oct 2 bw sweep).
- **Device side (k9a):** `mu::flushCache` on a dirty range: 461-537 cycles (64 B) to 3.8-5.7k
  cycles (256 KiB = 4,096 lines, < 1 cycle/line -> POSTED, returns before write-back);
  clean ~450-500 cycles flat; `flushCacheAll` 2.0k-17k cycles. Exit flush adds nothing to a 1 GiB
  kernel (k9b: 24.2-24.9 ms in none/range/all; first launch 35.5 ms = warm-up).
- **Round trip (k11):** one 64 B line host->device->host **2.22-2.31 us p50, p99 2.33 us**; host
  publish half 0.82-0.85 us; 60,000 round trips, 0 bad, 0 timeouts.

## Which half of each handoff is required (k6, k8, k7) — 3 reps x 64 KiB / 4 MiB / 64 MiB
- **host -> device (k6):** host flush REQUIRED (without it: every word stale at 64 KiB; most at
  larger sizes = lines the host cache still held). Device invalidate at kernel entry NOT needed:
  0 bad with host flush in every case -> launch starts with no stale device copies.
- **device -> host (k8):** host invalidate REQUIRED (stale host copies otherwise). Device flush
  at kernel exit NOT needed: 0 bad with host invalidate for none/range/all -> kernel completion
  writes device caches back.
- **NT stores (k7):** AVX2 stream + sfence, NO flush: **0 bad in all 27 cases** (3 sizes x prior
  host state absent/clean/dirty x 3 reps). The flush adds 60-75% to the write: 14.5 -> 25.3 us
  (64 KiB), 0.91 -> 1.58 ms (4 MiB), 6.9 -> 11.1 ms (64 MiB). NT without flush ~10 GB/s at 64 MiB.
  **Implication for Sec 4.3:** its NT arm included an unnecessary flush.
  **k7 absolute timings are cold-core numbers -- do not cite them** (resolved by k12/k13 below).

## Write timing after a kernel launch (k12, k13) — resolves the k7 vs Sec 4.3 discrepancy
Data: `results/suite_20261010_140918` (k12), `results/suite_20261010_141929` (k13).
- **k12:** a prior DEVICE read does NOT slow host writes (ratio 1.00-1.03, 4 sizes x 3 methods x 5
  reps, 0 bad). Hypothesis rejected.
- **k13 = core frequency.** k7/k12 timed one write right after launch + synchronize(); the core
  then runs at **0.8-1.0 GHz** (scaling_cur_freq; governor powersave, max 2.6 GHz) and the write
  takes 2-3x longer. Back-to-back ("steady", 1.7-2.2 GHz) reproduces Sec 4.3 exactly: NT+flush
  10.7 / 164.7 / 628.7 us at 64 KiB / 1 MiB / 4 MiB vs 4.3's 10.6 / 157.0 / 628.9; memcpy+flush
  1 MiB 411.7 vs 404.7.
- **Flush cost on an NT write (steady):** NT+sfence 5.7 us / 90.2 us / 361.5 us / 6.48 ms vs
  NT+flush 10.7 / 164.7 / 628.7 / 10.72 ms at 64 KiB / 1 MiB / 4 MiB / 64 MiB -> **+88% / +83% /
  +74% / +65%**. NT without flush: **10.4-11.6 GB/s** (14.8 GB/s at 4 MiB after a 50 ms spin).
- **Sec 4.3 consequence:** without the unnecessary flush, direct NT beats DMA host-to-device up to
  at least 4 MiB (1 MiB 90 vs 131 us; 4 MiB 362 vs 435 us; 64 MiB 6.48 vs 6.37 ms = tie). The
  512 KiB-1 MiB H2D crossover disappears / moves to >= 64 MiB. Needs a d1 NT-without-flush sweep.
- **Engine note:** a thread that wakes from synchronize() and immediately writes pays 2-3x until
  the clock ramps (host power management, cf. Sec 4.3 C-state finding).

## Granularity (k4, k5)
- **Invalidate + read (k4):** 1 thread: cold 3.50 GB/s -> invalidate+read 2.86 (whole) / 3.01
  (1 MiB chunks), -14%. 16 threads: 26.1 -> 17.9 (whole) / **20.1 (chunked), -23%**.
  Tier 2 reported 26.2 -> 22 (-16%) with memcpy into host DRAM; k4 sums in registers. Report both.
  Invalidation part = clean/absent flush cost from k1 (e.g. 1 MiB: 113 us single thread).
- **Per-object invalidate (k5):** adds ~0.5 us per object. 64 B: cold 83 -> 618 ns, hot 13 ->
  590 ns (46x). 256 B hot 36 -> 550 (15x); 1 KiB hot 113 -> 750 (6.6x); 4 KiB hot 409 -> 1,628 (4x),
  cold 1,314 -> 2,469 (1.9x). 16 threads similar. Break-even vs one bulk flush of a 4 GiB region
  (~78 ms at 16 threads, k3): ~145k objects; single thread (~290 ms): ~540k.

## Ordering (k10) — 3,048 lanes x 8 chunks x 512 lines, 16 chasing readers, 3 reps
| protocol | lines read before written back (of 12.5 M) | after completion |
|---|---|---|
| flushCache(range) + fence | 53,058-63,735 (~0.5%) | 0 |
| flushCache(range), no fence | 54,425-57,044 | 0 |
| no flush | 75,011-79,980 | 0 |
| flushCacheAll + fence | 0 | 0 |
| flushCacheAll, no fence | 0 | 0 |
| read after kernel completion | 0 | 0 |
All bad lines were poison (not yet written back), none torn. The fence is irrelevant;
`flushCacheAll` orders (it waits; flushCache is posted, k9a). Cost ~0.5-1 ms on a ~20 ms kernel.
The Oct 8 burst sanity run's final_bad > 0 does NOT reproduce here (0 final bad in 18 runs).
