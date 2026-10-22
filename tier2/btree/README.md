# B+-tree point lookups

Batches of point lookups in a B+-tree on TPC-H `orders.o_orderkey` at SF 1500 (2.25 B keys, 41 GB), on the host with the index in DRAM, mixed or on CXL, and on the device; probes range-partitioned per worker.

**Paper:** §5.2, Fig. 9, Fig. 10 · **Session:** devdax (NDP, Host-DRAM) and system-ram (Host@CXL, Tiered)

## Data
None: keys come from dbgen's `o_orderkey` formula, so any scale factor works without input files.

## Build
```bash
./build.sh            # host driver + MU kernel
```

## Run (the paper's runs)
```bash
./run.sh              # runs the half that the current session (devdax or system-ram) supports
```
Device session: the NDP sweeps (Fig. 9), node size and tree size (Fig. 10) and the partitioning ablation. System-ram session: the host at 0–100% of the index on CXL (Fig. 9), levels-first placement, tree size and node size.

## Smoke test
```bash
./smoke.sh            # small, validated run; prints PASS or FAIL
```
1 M device lookups in an SF 1 tree; passes if the found count and sums match.

## Figures
```bash
python3 figures/make_data.py   # results -> figures/data
./figures/render.sh            # figures/data -> .eps
```
| Paper | Output |
|---|---|
| Fig. 9 | `figures/fig_btree_inter_row.eps` |
| Fig. 10 | `figures/fig_btree_ns_row.eps` |

## Outputs
- `results/*.csv`: one row per configuration and iteration, with `ok` = 1 when validated and `part_ms` for the partitioning step.

## Notes
- `notes/DESIGN.md`: layouts (classic, compact), placements (interleave, block) and options.
- `scripts/confirm_taxes.sh`: checks Tier 1 costs inside this workload (not in the paper).
