# tier2/scan — the hash join's filter scan, on its own

The probe-side scan of the hash join (`tier3/hash_join`), extracted so it can be
run and timed with no hash table, build phase or join:

```sql
SELECT l_orderkey FROM lineitem WHERE l_quantity <= threshold
```

For every row, read `l_quantity` (uint8). If it passes, read `l_orderkey` (uint64), which is the
key the probe would look up, then count it or (`--materialize 1`) write it to the task's output
slice. This is the same predicate, data, types and per-task split as `cxl_probe_kernel.cpp`.
`l_quantity` is uniform over 1–50, so `--threshold 0/25/50` = 0/50/100% pass, the same as the
join's `-f`.

**Cross-check with the join:** every `l_orderkey` has a matching order, so `matches` here equals
the join's output tuples at the same threshold (sf_1: 0 / 2,999,428 / 6,001,215).

## Build and run

```bash
./build.sh                                   # host driver + mu_kernel/scan_filter_kernel.mubin
./main --mode host --tbl $DATA_ROOT/sf_1/lineitem.tbl --par 32 --threshold 25 --iters 3
./run_scan.sh 100 5                          # full grid on sf_100 -> results/*.csv
```

| `--mode` | columns live in | scanned by | needs |
|---|---|---|---|
| `host` (M1) | host DRAM (bind with numactl) | host threads | — |
| `host_cxl` (M5) | CXL NUMA node `--numa-node` (default 2) | host threads | MX1 in system-ram |
| `device` (M4) | device memory (PXL `memAlloc`) | MU tasks | MX1 in devdax |

Other options:
- `--upload memcpy|dma` (device mode): `memcpy` does what the hash join does (host memcpy +
  `flushHostCache`) and needs root. `dma` uses `copyToDevice`.
- `--materialize 0|1`, `--iters N`, `--verify 0|1`, `--cache-dir DIR`.
- `--bin FILE` instead of `--tbl`: load a column file from `tools/gen_cols.sh` (no text needed).
  `run_scan.sh` uses `sf_<SF>/lineitem_cols.bin` when it exists. `MATS=0` skips materialize.

`run_scan.sh` runs whichever modes the current dax mode supports. Device runs go to 3048 tasks,
the usable MU thread count. The `memcpy` device runs use `sudo` (override with `SUDO=`).
The `dma` device runs are off by default; enable them with `RUN_DMA=1`.

## Output (CSV on stdout)

`label,mode,upload,rows,par,threshold,sel_pct,materialize,iter,copy_ms,scan_ms,read_gb,gb_per_s,matches,ok`

- `copy_ms`: per-iteration upload of both columns to the placement under test (0 for `host`).
  Single-threaded, like the join's upload.
- `scan_ms`: the scan only (kernel `execute` + `synchronize`, or the host threads).
- `read_gb` = rows × 1 B + matches × 8 B, which is what the scan actually reads. `gb_per_s` uses
  it. Output writes in materialize mode are not counted.
- `ok`: validated against a reference computed once on the host by separate code: count, sum of
  passing row indices and sum of passing keys. With `--materialize 1`, device runs also re-read
  every output slice on the device (`sum_output` kernel) and, as root, through host loads.
  The output is never read back with `copyFromDevice` (see below).

## Loading

The first run parses the `.tbl` in parallel (sf_1 ≈ 0.4 s) and writes `cache/<path>.bin`
(9 B/row, so about 5.4 GB at sf_100). Later runs read the cache. The cache is keyed on the source
file's size and mtime. The parser takes the same fields and truncation as the join's
`TPCHTableReader::read`; on sf_1 it was checked against `awk` (count and key sum at threshold 25).

## Things to keep in mind

- The loop branches on purpose. The join skips the key read for filtered rows, so the scan keeps
  that access pattern, including branch mispredicts at 50%.
- Device kernel time includes `flushCacheAll` at the start and end (the resident-worker
  coherence protocol).
- Host thread creation is inside `scan_ms`. This only matters at small
  scale factors.

## Device DMA caveats (2026-10-01)

Found while validating sf_100 materialize runs; repro programs and coredumps are in `diag/`.

- **Truncated uploads:** a single `copyToDevice` that crosses a 1 GiB-aligned device address
  silently stops there. The rest keeps its old contents, and the call still returns `Success`.
- **Zero read-back:** on buffers whose start is not 1 GiB-aligned (`memAlloc` places buffers
  top-down, so any size that isn't a whole number of GiB), data the kernel wrote above the first
  1 GiB boundary read back as zeros via `copyFromDevice`. A second kernel saw it correctly.
- **Broken launches:** after chunked DMA writes to such buffers, every kernel launch failed (PC=0
  in all coredumps) until a cold power cycle (`sudo ipmitool chassis power cycle`). A warm
  reboot left the CXL cards missing from PCIe.

What the code does about it:
- **Aligned buffers:** every device buffer is rounded up to whole GiB, and `dev_alloc` refuses a
  buffer that doesn't start on a 1 GiB boundary.
- **Split copies:** DMA copies are split so none crosses a 1 GiB boundary.
- **No output read-back:** materialized output is never read with `copyFromDevice`.
- **DMA grid off by default:** the DMA runs in the grid are opt-in.

## Key width and large scale factors

`l_orderkey` is stored as uint64 (9 B/row with `l_quantity`), because TPC-H order keys reach
about 6 million × SF and overflow uint32 above about sf_700. Results from before 2026-10-01 20:00
used uint32 keys (5 B/row) and are not directly comparable on bytes or materialize cost.

For scale factors whose text would not fit on disk, `tools/gen_cols.sh <SF> <chunks> <outdir>`
runs dbgen into FIFOs drained by `tools/tpch_cols`, so only the column file
(`lineitem_cols.bin`, 9 B/row) is written. Its output was byte-identical to the parsed sf_1
`lineitem.tbl` cache. It pins to NUMA node 1 so a node-0 DRAM budget is untouched.

DRAM budget: `dram_budget.sh apply 64 | restore | status` reserves node-0 memory as hugetlb
pages so runs bound to node 0 see about 64 GB (box-wide; tell other users, always restore).
