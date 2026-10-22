# Coherence tax (gamma) — microbenchmark suite

Paper Sec. 4.4. The CXL 2.0 link has no back-invalidation (Sec. 3.5.1), so every handoff of
modified data is done in software:

- **host -> device:** host writes, host flushes (`pxl::flushHostCache` = `clflushopt` loop +
  `mfence`), device drops stale copies (`mu::flushCache` / `mu::flushCacheAll`), device reads.
- **device -> host:** device writes, device flushes (`mu::flushCache`), host invalidates
  (`flushHostCache` on clean lines), host reads.

Every test re-measures from scratch, including results that exist elsewhere (marked "retest").
Built 2026-10-10. Harness conventions follow `data_movement_tax`: root + devdax guards, every
result verified bit-exact, a canary before and after each test, device state captured on a
mismatch, the pinned host configuration of Sec. 4.3 (mx_dma kthreads on CPUs 8,9, process on
node 0, default C-states). No DMA anywhere (copies past the first GiB are broken, see memory
xcena-dma-1gib-boundary): config goes in by direct store + flush, results come back by
invalidate + read.

| Test | Question | Paper use | Retest of |
|---|---|---|---|
| k1 flush_size | host flush cost vs range size (64 B-1 GiB), cache state dirty/clean/absent, clflushopt vs clflush, library vs raw loop | "Cost of one handoff": gamma(n) = gamma_call + lines * gamma_line | bw --path flush (Oct 2; states not real, 17 GB >> L3) |
| k2 dirty_frac | flush cost of a 16 MiB range with 0-100% of lines dirty, uniform vs clustered, rest clean or absent | write sparsity, dirty-bit-map future work | never run |
| k3 flush_threads | bulk flush throughput, 1-32 threads, in-L3 dirty and 16 GiB absent | phase-boundary flush of a large table | bw --path flush |
| k4 read_inval | host read of device memory: cold vs invalidate-then-read vs cached; 1-16 threads | invalidation cost apart from read-back; return-path tax | Tier 2's 26.2 -> 22 GB/s (16%) |
| k5 fine_grained | per-object invalidate+read (64 B-4 KiB objects) vs plain read vs one bulk flush, hot and cold objects | "Granularity": B-Link SDBI vs phase boundary | never run |
| k6 h2d_vis | host writes; {host flush, none} x {device invalidate at entry, none}: does the device see the data? | which half of a h->d handoff is required | resident_worker (one line) |
| k7 nt_vis | NT stores + sfence without flush: visible to the device? prior host cache state absent/clean/dirty; cost vs memcpy+flush | "Do NT stores still need a flush?" (connects to 4.3) | never run |
| k8 d2h_vis | device writes; {device flush, none} x {host invalidate, none}: does the host see the data? | which half of a d->h handoff is required | tpch return modes |
| k9 dev_flush | device-side flush cost: mu::flushCache (dirty/clean) and flushCacheAll per task, plus kernel time with/without end flush | device half of gamma | never run |
| k10 publish | in-kernel publish race, powered: P=3048 lanes x C chunks x L lines, 16 host readers chase counters; flush/fence variants; launch-boundary control | "Ordering" | tpch scan_filter_pub (Oct 4), burst bench (Oct 8, sanity run showed final_bad > 0, never completed) |
| k11 pingpong | one-line handoff round trip host -> device -> host via a resident task | minimum latency of a coherent handoff each way | resident_worker ret_bench (Oct 8) |

Sizes: anything whose cache state matters stays below the 96 MiB host L3 (memory host-cache-sizes);
sizes above it are reported but flagged `state_held=0`.
