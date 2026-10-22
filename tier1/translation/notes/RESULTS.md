# Translation tax on the MX1P — results (2026-09-18)

Device runs: `results/device_{A_span,B_entry,C_miss}_20260918_143111.csv`,
`results/device_B2_entry_20260918_144404.csv`, `device_D_workingset_*`, `device_E_smallK_*`.
Host validation of the method: `results/host4k_*_20260918_141855.csv`,
`results/hostthp_*_20260918_142259.csv`. Every timed pass is checksummed (`sum_ok=1` throughout;
the analyzer refuses files with any mismatch). Single MU task, MX1 in devdax.

## Headline

The MU translation structure is **8 entries of 1 GB each** — the vendor's 8 GB of coverage, now
resolved into its two factors — and a miss costs **~590 ns** at a working set of 4096 lines.

```
ns_per_access = 94.2 + 587.2 * max(0, 1 - 8/G)        R2 = 1.0000 over 153 points
```

where **G is the number of distinct 1 GB granules the access pattern touches**. Nothing else
matters: region size, region count, span and layout all drop out once G is fixed.

| G (1 GB granules) | random | round robin |
|---:|---:|---:|
| 1 | 94.4 | 99.0 |
| 2 | 94.4 | 99.0 |
| 4 | 94.4 | 99.0 |
| 8 | 94.4 | 99.0 |
| 16 | 384.9 | 689.3 |
| 32 | 535.0 | 691.4 |
| 64 | 609.9 | 693.6 |
| 128 | 646.8 | 694.5 |

## How each number is pinned down

**8 entries.** Cost is flat to G = 8 and rises from G = 16 on. The rise follows `1 - 8/G` to
four decimal places: 0.5 of the miss cost at G = 16, 0.75 at 32, 0.875 at 64, 0.9375 at 128.

**1 GB granules.** Identified without assuming it, by configurations that share a span but
differ in granule count. At a **16 GB span**: 8 regions spaced 2 GB apart cost **94.4 ns**,
while 16 regions spaced 1 GB apart cost **384.6 ns**. At a **32 GB span**: 16 regions of 2 GB
cost 385.1 ns, 32 regions of 1 GB cost 535.0 ns. Region spacings below 1 GB (512 MB, 256 MB,
64 MB, 16 MB) never add cost beyond what the span's 1 GB granule count already predicts — lines
closer than 1 GB share an entry. Fitting g as a free parameter over all 153 points gives
g = 1 GB at R² = 1.0000, with the next candidate (2 GB, E = 4) at 0.9511 and 512 MB at 0.9032.

**~590 ns per miss, measured twice.** The least-squares fit gives 587.2 ns. Independently, the
round-robin order pays a flat **~694 ns** for every G from 16 to 128 — a cyclic pattern over more
entries than exist misses on every access, so the tax is the miss cost itself, with no fitting:
694.5 - 99.0 = **595 ns**. The two agree within 1.5%.

**The replacement policy is LRU/FIFO-like.** At G = 16 with 8 entries, round robin misses 100%
of the time while random misses 50%. Random replacement would have let the cyclic pattern hit
sometimes; it never does.

## Two effects that gate when the tax is paid at all

**1. Cache-resident working sets pay nothing.** At G = 16 (twice the entries), a working set of
16 or 32 lines costs 62-65 ns — no tax at all — while 64 lines costs 228 ns and 4096 lines
385 ns. So translation is only paid when an access actually reaches memory. This retires the
open question from the 2026-09-15 v1 run, whose "16 lines over a 16 GB span, no tax" point
looked like evidence for wider coverage: those 16 lines were simply cache-resident.

**2. The miss cost itself grows with the working set.** At a fixed G = 64:

| lines (K) | 64 | 128 | 256 | 512 | 1024 | 4096 | 16384 | 65536 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| ns/access | 362 | 483 | 538 | 563 | 576 | 609 | 630 | 635 |

Backing out the miss cost at K = 64 from three different granule counts gives the same answer
each time (G = 16: 330 ns, G = 32: 341 ns, G = 64: 343 ns), so this is a property of the miss,
not of the miss rate. It is what a **software** MMU should do: a small working set keeps the
walk's own metadata in cache, a large one does not. The miss cost therefore runs from ~340 ns
(small working set) to ~635 ns (large), with ~590 ns at K = 4096.

A fuller model, then:

```
tax = P(cache miss) * max(0, 1 - 8/G) * miss_cost(working set),   miss_cost in [340, 640] ns
```

## Method validation on the host

The same code, same patterns, on a platform whose translation behavior is known:
- It finds a tax where one must exist: up to 7x on 4 KB pages (28.9 ns vs 4.0 ns baseline).
- A known intervention removes it: THP takes the 1 GB span / R = 65536 point from 28.9 to 6.6 ns.
- The fit recovers a sensible structure: **E = 3072 entries at 4 KB, 21.5 ns per miss,
  R² = 0.99** — the right order for this CPU's STLB, and the right cost for a hardware walk.

One caveat carried over from the host: there, cost keeps climbing after `1 - E/G` saturates
(22 -> 45 ns from R = 16384 to 65536), because a hardware page walk also pays for page-table
footprint. **The device shows no such residual** — at a fixed working set, everything collapses
onto G alone — so the one-term model is enough for the MX1P.

## Negative result worth keeping

**Sweeping the region count alone measures nothing.** With target lines spread evenly through
each region, the lines end up covering the span at a spacing of span/K whatever the region count
is, so the granules touched depend only on the span. Host: 29 ns at a 1 GB span and 45 ns at
16 GB, identical for every R from 1 to 65536. Device: 386 ns at 16 GB and 610 ns at 64 GB, flat
across all 13 region counts. The `spread` rows in every sweep are this control; only the `clump`
layout makes the granule count an independent variable.

## What this means for the paper

- Tier 1 can state the translation tax as a **two-parameter hardware fact** (8 x 1 GB, ~590 ns)
  plus a **rate term the workload controls**: `1 - 8/G`.
- The tax is **not a per-byte or per-span cost** — it is set by how many 1 GB granules a kernel's
  random accesses touch. A sequential scan of 64 GB touches 64 granules but in order, so it pays
  once per granule, not per access. A random probe into a table above 8 GB pays on most accesses.
- The device ceiling is **8 GB of concurrent random reach per MU task**. The hash-join probe at
  SF1000 and above is exactly the pattern that exceeds it, so the model predicts a ~590 ns adder
  on the majority of probe accesses there — worth checking against the measured probe times.

## Open

- **Per MU, per cluster, or per device?** Every run here used a single task. Whether 8 entries
  are shared by a cluster's 8 MUs (32 threads) decides whether the tax worsens under the task
  counts real kernels use. Needs either task-placement control in PXL or an indirect sweep over
  task count.
- **Does the round-robin 694 ns hold at larger working sets?** C used K = 4096 throughout.
- **Entry granularity vs. allocation.** All spans here came from one 128 GB `memAlloc`; whether
  entry boundaries follow that allocation or the device's physical layout is untested.
