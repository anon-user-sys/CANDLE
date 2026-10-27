# btree_lookup — B+tree point lookups, host vs. CXL NDP

The index counterpart of `tier2/scan`. Same layout and conventions (shared
host/device header, `--mode host|host_cxl|host_split|device`, validated CSV rows, grid script);
a different access pattern. The scan streams columns and is bandwidth-bound. Here every lookup
is a chain of dependent node fetches:

```sql
SELECT o_rownum FROM orders WHERE o_orderkey = ?        -- one probe per lookup key
```

- **Index:** a B+tree on TPC-H `o_orderkey`, N = 1.5M × SF keys, value = order row number.
  Keys come from dbgen's sparse formula (`mk_sparse`: 1..7, 32..39, 64..71, ...), checked
  against `sf_1/orders.tbl`, so no input file is needed at any scale factor.
- **Probes:** `--lookups` keys, uniform or scrambled Zipfian (`--dist zipf --theta 0.99`, YCSB's
  scrambling) over the existing keys. `--miss-pct P` makes P% of them keys that can't exist.
- **Work split:** one contiguous slice of the probe array per thread (host) or MU task (device).
  Slices are multiples of 8 lookups, so no output line is shared by two tasks.

## Build and run

```bash
./build.sh                                              # host driver + mu_kernel/btree_kernel.mubin
./main --mode host   --sf 100 --par 128  --lookups 100M --iters 3
./main --mode device --sf 100 --par 3048 --lookups 100M --iters 3
./run_btree.sh 100 3                                    # grid -> results/btree_sf100_*.csv
```

| `--mode` | tree + probes live in | looked up by | needs |
|---|---|---|---|
| `host` (M1) | host DRAM (bind with numactl) | host threads | — |
| `host_cxl` (M5) | CXL node `--numa-node` (default 2) | host threads | system-ram |
| `host_split` (M1_split) | `--dram-frac F` in DRAM node 0, rest on CXL, mbind per 2 MB | host threads | system-ram |
| `device` (M4) | device memory (`memAlloc`) | MU tasks | devdax (no root needed) |

`host_split --placement`: `interleave` mixes DRAM and CXL chunks evenly (F/(1-F) pattern, as in
the scan). `block` puts the first F of the array in DRAM. Nodes are laid out top-down (root =
node 0, then each level, leaves last), so `block` = all upper levels + the first leaves in DRAM.
That is the placement a tiering system would converge to.

Other options:
- `--node-bytes B` (multiple of 64; fanout = (B-8)/16: 256 → 15, 512 → 31, 4096 → 255),
  `--fill f` (fraction of each node used by the bulk load, default 1.0), `--keys N` (instead of `--sf`).
- `--group G`: walk G lookups level by level in lockstep, prefetching each one's next node
  (group prefetching). G = 1 is the plain loop. The host uses `__builtin_prefetch`, the device
  `mu::prefetch`. Nodes ≤ 512 B are prefetched whole; bigger nodes only the header line and
  the first binary-search probe.
- `--materialize 1`: write each lookup's value (0 = miss) to `out[i]`. **Unreliable on the
  device, see below.**
- `--thp 0|1` (host buffers, default 0 = MADV_NOHUGEPAGE, 4 KB pages as in the scan; stderr reports AnonHugePages), `--seed`,
  `--iters`, `--verify`.

## Output (CSV on stdout)

`label,mode,sf,keys,node_bytes,fanout,fill,height,tree_gb,lookups,dist,theta,miss_pct,par,group,materialize,iter,upload_ms,lookup_ms,mops,found,ok`

- `lookup_ms`: the lookups only: kernel `execute` + `synchronize`, or the host threads
  (thread creation included, as in the scan).
- `upload_ms` (device only): the one-time copy of tree + probes into device memory (16-thread
  memcpy + `flushHostCache`). It's the same on every row of a run. The tree stays resident across
  iterations, like an index would.
- `mops` = lookups / lookup_ms / 1000.
- `ok`: validated against a reference computed from the probe keys alone with the key formula,
  never through the tree. It checks the found count, the sum of found positions and the sum of
  values. With `--materialize 1` the output is re-summed as well (device: on the device and by
  host loads). A deliberately broken tree (off-by-one routing) fails it.

## First numbers (sf 100, 150M keys, devdax, 2026-10-11, 50M uniform lookups)

| node | height | host 128 thr (DRAM) | device 3048 tasks |
|---|---|---|---|
| 256 B | 7 | 158 Mops | 67 Mops |
| 512 B | 6 | 141 | 65 |
| 1024 B | 5 | 124 | 45 |
| 4096 B | 4 | 76 | 25 |

- **Correction (2026-10-12):** an earlier version said the device was at its bandwidth limit.
  The search code reads exactly 3 lines per 256 B node and 7 per 4 KB node (24 / 28 lines per
  lookup at SF 1500, 21 / 28 at SF 100). At 3048 tasks the device reads 59 GB/s (256 B) and only
  27 GB/s (4 KB) of tree lines at SF 1500, far below its 124 GB/s *sequential* ceiling. Its
  ceiling for scattered 64 B reads hasn't been measured. Group prefetching still didn't help at SF 100.
- **The host is the unrestricted DRAM reference.** The upper levels sit in its 96 MiB L3. The
  paper's comparison is M4 vs the restricted host (M1_split), which needs system-ram mode.

## Device caveat: lost MU writes in `--materialize 1` (2026-10-11)

With 4 KiB nodes and `--materialize 1`, about 10–30% of fresh processes failed validation on
iteration 0. The device's counts were always right, but some 64 B output lines in device memory
still held the **previous process's** values (48–3528 lines; different lines every time).
On a mismatch the diagnosis in `run_device` re-reads the output. Facts from those runs:
- It's unchanged after 200 ms, after a 3048-task `flushCacheAll` kernel, and after the host reads
  400 MB of other device memory. So no write-back is pending, and it isn't the host cache.
- At first the device's own re-read (`sum_output`) saw the new values. After an MU kernel read
  160 MB of other memory, the device's re-read was **wrong too**. So the stores sat only in an
  MU cache and were dropped on eviction instead of written back.
- Both the poison fill and the lookup's writes to those lines were lost. That's why the stale
  values come from the previous process and why later iterations pass (they write identical
  values).
- Host-side poisoning (stores + `flushHostCache` from 16 threads) had its own failure: poison
  lines reached device memory *after* the kernel wrote them, at the 16-thread chunk boundaries.
  The poison now comes from a device kernel (`fill_output`), as in the scan's return path.

Count mode (the default) writes one stats line per task and was correct in every run. The tree
is never written on the device. Bad-line offsets of the failures: `diag/badlines_20261011.txt`.
