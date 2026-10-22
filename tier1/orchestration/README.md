# Orchestration tax (α): null-control kernel

Measures what it costs to admit, dispatch and complete device work, with a kernel that does nothing: launch cost against task count, handle reuse, resource allocation and amortization.

**Paper:** §4.1, Fig. 3, Table 2 · **Session:** devdax

## Build
```bash
./build.sh            # host driver + MU kernel
```

## Run (the paper's runs)
```bash
./run.sh              # -> results/*_<timestamp>/
```
Runs the paired aged/fresh sweep, the 15-test sweep, the T15 amortization runs and the 6,000-dispatch handle-aging run.

## Smoke test
```bash
./smoke.sh            # small, validated run; prints PASS or FAIL
```
One fresh-handle T1 sweep (1 to 131,072 tasks); passes if every dispatch completes.

## Figures
```bash
python3 figures/make_data.py   # results -> figures/data   (run from this directory)
./figures/render.sh            # figures/data -> fig_*.eps
```
| Paper | Script |
|---|---|
| Fig. 3a | `figures/fig_cost_model.gp` |
| Fig. 3b | `figures/fig_map_aging.gp` |
| Fig. 3c | `figures/fig_subcount.gp` |
| Fig. 3d | `figures/fig_amortization.gp` |

## Outputs
- `results/data_fresh/`, `results/data_t15fix/`: the raw runs behind Fig. 3a, 3c, 3d and Table 2.
- `results/aging/`: written by `scripts/aging_run.sh`. Fig. 3b renders once this exists (the paper's original raw run was not kept).

## Notes
- `main_profiler` (XPTI profiler, device stages) is the paper driver; `main` is the same tests without the profiler.
- `notes/FRESH_RESULTS.md`, `notes/DATA_TABLES.md`: tables of the measured runs.
