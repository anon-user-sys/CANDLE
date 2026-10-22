# Selective scan

The hash join's probe-side filter on its own: `SELECT l_orderkey FROM lineitem WHERE l_quantity <= t` on TPC-H SF 1500 (81 GB in two columns), counting matches or materializing their keys, on the host with its input in DRAM, mixed or on CXL, and on the device.

**Paper:** §5.1, Fig. 8, Table 5 · **Session:** devdax (NDP, Host-DRAM) and system-ram (Host@CXL, Tiered); root for the return path

## Data
Set `DATA_ROOT` to a folder holding `sf_<SF>/lineitem.tbl` (TPC-H `dbgen`). For SF 1500 the runs read a two-column file instead:
```bash
tools/gen_cols.sh 1500 64 $DATA_ROOT/sf_1500    # -> lineitem_cols.bin (81 GB)
```

## Build
```bash
./build.sh            # host driver + MU kernel
```

## Run (the paper's runs)
```bash
./run.sh              # runs the half that the current session (devdax or system-ram) supports
```
Device session: the NDP task sweep with prefetch and the all-DRAM host (Fig. 8), serial vs. 32-launch return (Table 5) and its task sweep, and the device read rate. System-ram session: the host at 0–100% of the input on CXL (Fig. 8).

## Smoke test
```bash
./smoke.sh            # small, validated run; prints PASS or FAIL
```
The device scan on SF 1 at 50% selected; passes if every row matches the reference. Needs `$DATA_ROOT/sf_1/lineitem.tbl`.

## Figures
```bash
python3 figures/make_data.py   # results -> figures/data
./figures/render.sh            # figures/data -> .eps
```
| Paper | Output |
|---|---|
| Fig. 8 | `figures/fig_o3_row.eps` |

## Outputs
- `results/tpch_scan_*.csv`: one row per configuration and iteration, with `ok` = 1 when the result matches the reference.
- `results/return_*.csv`: the return runs, with `ret_ms` and `e2e_ms`.

## Notes
- `--return pipelined` was removed: its in-kernel publish raced. The paper's overlapped return is `--return chunked` (32 launches).
- `notes/DESIGN.md`: modes, options and device caveats.
