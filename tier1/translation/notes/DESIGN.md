# translation_tax — what does address translation cost an MU core?

Tier-1 experiment for the CXL NDP paper: isolate the **translation tax** on the MX1P and
express it as a model the workload studies can apply. The MX1 SDK exposes no TLB/MMU API and no
performance counters, so device-side translation cost cannot be read directly. This example
infers it by timing, and validates the method on the host, whose translation cost is known.

Vendor input (2026-09-17): MX1P TLB coverage is capped at **8 GB per cluster** and a miss costs
about **1 µs** in a software MMU. So the question is no longer *whether* the device translates —
it does — but **what the structure is**: how many entries, how big, what a miss costs, and which
access patterns pay.

## Status

- **v1 (2026-09-15, an earlier stride-based driver, not included)** showed a tax exists:
  flat up to an 8 GB span, +443 ns/access at a 32 GB span, +297 ns/access at 64 GB. It could not
  say more, because its one knob (`stride`) moved cache demand and translation demand together.
- **v2 (2026-09-18, this driver)** separates them, adds a host validation of the method, and
  **answers the question**: the MU translation structure is **8 entries of 1 GB**, a miss costs
  **~590 ns**, and only cache-missing accesses pay. See `RESULTS.md`. The model below fits 153
  device points at R² = 1.0000.

## Model under test

The platform holds **E** translation entries covering **g** bytes each. A pattern touching **G**
distinct g-sized granules uniformly at random hits with probability E/G, so

```
ns_per_access = base + miss_cost * max(0, 1 - E/G)
```

**Measured 2026-09-18: E = 8, g = 1 GB, miss_cost ~ 590 ns, base 94 ns** (R² = 1.0000 over 153
points). The pre-registered guess from the v1 data — E = 32 entries of 256 MB — is **refuted**:
it was fit to a v1 point (16 lines over a 16 GB span, no tax) that turned out to be cache
residency, not translation coverage. The product is the same 8 GB the vendor quotes; the factors
are not. Full numbers and how each is pinned down: `RESULTS.md`.

## Method

Place **K = R x L** distinct 64-byte target lines: **R** regions **S** bytes apart, **L** lines
inside each region, then read K lines in a fixed number of random accesses (`tt_common.hpp`).

**K fixed across a sweep holds cache demand constant.** This is the v1 fix: v1 tied K to the
region count, and on the device its dense-stride baseline alone moved 43 -> 99 ns as K grew, on
top of any translation cost.

**The layout knob decides whether R is a real knob at all.**

| layout | where the L lines sit | granules touched |
|---|---|---|
| `spread` | one per slot of S/L bytes, covering the region | depends only on the span |
| `clump` | packed 64 B apart at a clump-aligned random base | tracks R |

With `spread`, the K lines end up evenly covering the span at a spacing of span/K whatever R is.
The 2026-09-18 host run confirms it: at a 1 GB span every R from 1 to 65536 costs 29 ns, and at
16 GB every R costs 45 ns. **R alone is not a knob** — a negative result worth keeping, since the
obvious "sweep the region count" experiment measures nothing. `clump` packs each region's lines
into L x 64 contiguous bytes, which fits inside any plausible granule, so the granules tracks R
while span and K stay fixed. Every sweep runs `spread` as the control.

Two access orders:
- `random` — uniform over the K lines; cost follows `miss_cost * (1 - E/G)`.
- `rr` — regions in a fixed cycle. With R above E, a cyclic pattern misses on *every* access
  under LRU or FIFO, so the added time **is** the miss cost, with no fitting. Comparing the two
  at equal R also says something about the eviction policy.

Controls, unchanged from v1 and still load-bearing:
- **Identical compute for every configuration.** One branchless expression covers both layouts;
  the timed loop has no per-access branch on layout or order, and no division (a 64-bit modulo
  in a v1 draft cost ~50 ns per access on an MU core and mimicked a translation tax).
- **No auxiliary memory traffic.** No index arrays; the only memory touched is the target lines.
- **Warm-up pass** of the identical sequence before the timed pass.
- **Correctness.** Every timed pass is checksummed against a host replay (`sum_ok`); the device
  run stops on the first mismatch. Silent wrong-result degradation is a known failure mode of
  this device, so `analyze_tlb.py` refuses to analyze a file containing any `sum_ok=0`.
- **Device coherence.** Kernels flush the host-written config before reading it and flush their
  own writes before returning, following `tier1/resident_worker`.
- **Single MU task**, so there is no contention between tasks.

Units: device `ns_per_access` = device cycles/access x ns-per-cycle from a calibration run (a
memory-free spin timed by the host). Host `ns_per_access` = `steady_clock` around the timed pass.
Absolute device and host numbers are **not** comparable (per-access compute differs ~10x);
compare within a platform, across configurations.

## Host validation (2026-09-18)

The host is the control: a platform with known translation behavior, where 4 KB pages should show
a tax that THP removes. Median ns/access, K = 65536 lines throughout:

| span | R | layout | 4 KB pages | THP |
|---|---|---|---|---|
| 1 GB | 1 | clump | 6.6 | 4.8 |
| 1 GB | 1024 | clump | 5.2 | 6.7 |
| 1 GB | 65536 | clump | **28.9** | **6.6** |
| 1 GB | any | spread | 29.2 | 6.6 |
| 16 GB | 1024 | clump | 7.1 | 6.7 |
| 16 GB | 65536 | clump | **45.2** | (over the THP cap) |
| 16 GB | any | spread | 45.3 | – |

Three things the method needed to show, all present:
1. **It detects a tax where one is known to exist** — up to 7x on 4 KB pages.
2. **A known intervention removes it** — THP cuts the 1 GB / R=65536 point from 28.9 to 6.6 ns.
3. **The fit recovers a sensible structure.** On the sweep whose region spacing brackets the
   granule (`host4k_C_miss`), the best fit is **E = 3072 entries, miss_cost = 21.5 ns,
   base = 6.2 ns, R² = 0.99** — the right order for this CPU's STLB, and the miss cost is right
   for a page walk that mostly hits cached page-table lines.

Two honest limits, both of which matter for reading the device data:
- **g is only identifiable when the sweep includes S below g.** With every region spaced at or
  above the granule, all candidate g values predict the same G, so the fit reports a tie (host
  4 KB: g = 4 KB through 128 KB fit identically). The device B sweep spans S = 16 MB to 2 GB,
  which brackets the measured 1 GB granule and is what identified it.
- **One entry-set term does not fully describe the host.** Cost keeps climbing past the point
  where `1 - E/G` saturates (22 -> 45 ns from R = 16384 to 65536), because a real page walk also
  pays for page-table footprint: more levels missing in cache as the span grows. The `spread`
  rows isolate this term — they hold G at its maximum and vary only the span. The device shows
  **no** such residual at a fixed working set, so one term is enough there.

## Running it

```
./build.sh
./scripts/run_tlb.sh host4k          # ~3 min, no root, no device
./scripts/run_tlb.sh hostthp         # ~2 min; --host-thp-cap-gb keeps the commit inside DRAM
./scripts/run_tlb.sh device          # ~40 min; needs dax0.0 in devdax mode
python3 scripts/analyze_tlb.py results/device_*_*.csv
```


The device run needs the MX1 in **devdax** mode and the box to itself (it is shared):

```
numactl -H                                             # node 2 at 0 MB = already devdax
sudo daxctl reconfigure-device --mode=devdax --force dax0.0
sudo systemctl restart pxl_resourced.service           # AFTER the flip, never before
sudo xcena_cli num-device                              # must report 1
./scripts/run_tlb.sh device results
```

Flipping the mode takes the device away from anyone using node 2 as host memory, so coordinate
first. Never `modprobe -r mx_dma`. If `memAlloc of 128 GB failed` appears, lower `BUF` in
`run_tlb.sh` (the sweep then skips spans above the buffer). Because a wedged device needs a
reboot of a shared machine, do not kill a device run mid-flight.

## What the device sweeps will answer

| sweep | question | answer (2026-09-18) |
|---|---|---|
| A `granules` | how many entries? | **8**; flat to G = 8, then exactly `1 - 8/G` |
| B `entry-size` | how big is an entry? | **1 GB**; 8 regions 2 GB apart are free where 16 regions 1 GB apart are not, at the same span |
| C `miss-cost` | what does one miss cost? | **~595 ns**, measured directly: round robin past 8 regions pays a flat 694 ns on every access |
| D `workingset` | when is it paid at all? | only on cache misses, and the miss cost itself grows with the working set (340 -> 635 ns) |

Still open: whether the 8 entries are per MU, per cluster or per device (every run here used a
single task), and what the model predicts for the hash-join probe, whose random probes into a
table above 8 GB are exactly the pattern that pays.
