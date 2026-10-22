# Partitioned hash join

`orders ⋈ lineitem` on `o_orderkey = l_orderkey` with the scan's `l_quantity <= t` filter, materializing `(l_orderkey, o_rownum)`. Three plans over the same CXL-resident data: Host@CXL (build and probe on the host), Hybrid (host build, device probe) and NDP@CXL (build and probe on the device), with an owner-computes build (no atomics).

**Paper:** §6.1, Fig. 11, Fig. 12 · **Session:** devdax, root

## Data
Set `DATA_ROOT` to a folder with `sf_<SF>/orders.tbl` and `sf_<SF>/lineitem.tbl` (TPC-H `dbgen`), for SF 1, 10 and 100.

## Build
```bash
./build.sh            # host drivers + MU kernels
```

## Run (the paper's runs)
```bash
./run.sh
```
SF 100 at 50% selected on the full platform (Host@CXL and Hybrid at P = 8,192, NDP@CXL at P = 1,024), SF 1 and 10 for the cost per tuple, 0% and 100% selected, and the thread and task shares. Every run checks its output against a reference join and runs the device canary before and after.

## Smoke test
```bash
./smoke.sh            # small, validated run; prints PASS or FAIL
```
All three plans on SF 1 at 50% selected, one iteration each; passes if every run matches the reference join. Needs `$DATA_ROOT/sf_1/`.

## Figures
```bash
python3 figures/make_data.py   # results -> figures/data
./figures/render.sh            # figures/data -> .eps
```
| Paper | Output |
|---|---|
| Fig. 11 | `figures/fig_hj_row.eps` |
| Fig. 12 | `figures/fig_hj_sweep.eps` |

## Outputs
- `results/devdax_modes_<config>_<time>/mode{2,3,4}_*.json`: one line per iteration with build, probe and handoff times, the tuple count and an order-independent checksum.

## Notes
- Modes: 2 = Host@CXL, 3 = Hybrid, 4 = NDP@CXL (`hashjoin_cxl_*`). `hashjoin_host_dram` and `hashjoin_host_tiered` (Mode 5) also build; the paper does not use them.
- `scripts/run_cxl_emulator.sh`: a functional check on the XCENA emulator.
- `scripts/run_unit_test.sh`: functional tests of all modes.
