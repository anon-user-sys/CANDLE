# B-link tree under updates

A YCSB-A workload (50% reads, 50% updates, Zipfian θ 0.99) on a 10 M-key B-link tree in CXL memory, processed in batches of 1 M requests. Plans: Host@CXL, Hybrid (reads on the host, updates on the device), NDP-sync (a launch per batch) and NDP@CXL (a resident worker).

**Paper:** §6.2, Fig. 13 · **Session:** devdax; root for the Host@CXL baseline

## Data
None: the tree and the requests are generated.

## Build
```bash
./build.sh            # host drivers + MU kernels
```

## Run (the paper's runs)
```bash
./run.sh
```
The four Fig. 13 sweeps (Host@CXL per operation and batched, Hybrid, NDP-sync and NDP@CXL over 16–3,048 device tasks), the Hybrid soak test and the request-combining comparison.

## Smoke test
```bash
./smoke.sh            # small, validated run; prints PASS or FAIL
```
YCSB-A on a 1 M-key tree with all four plans; each validation binary checks every result and the final tree state.

## Figures
```bash
python3 figures/make_data.py   # results -> figures/data
./figures/render.sh            # figures/data -> .eps
```
| Paper | Output |
|---|---|
| Fig. 13 | `figures/fig_blink_row.eps` |

## Outputs
- `results/fig_*.csv`: one row per run with throughput and the per-phase times (staging, dispatch, readback, invalidation, splits).

## Notes
- Binaries per plan: `blink_<plan>_val` (validated), `_tp` (throughput), `_lat` (latency).
- `scripts/run_all_phases.sh`: the full validation suite (invariants, stress, soak, YCSB sanity).
- Host dedup, replay and write grouping are excluded from the timed region for every plan.
