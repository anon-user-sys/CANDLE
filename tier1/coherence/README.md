# Coherency tax (γ): visibility suite

Measures the software work that makes writes visible across host and device without hardware back-invalidation: flush cost, which handoff actions are needed, per-object invalidation and in-kernel publication.

**Paper:** §4.4, Fig. 7, Table 4 · **Session:** devdax, root

## Build
```bash
./build.sh            # host driver + MU kernel
```

## Run (the paper's runs)
```bash
./run.sh              # -> results/*_<timestamp>/
```
Runs k1, k2, k4, k5, k6, k7, k8, k10 and k13 with the device canary before and after each test.

## Smoke test
```bash
./smoke.sh            # small, validated run; prints PASS or FAIL
```
k6, k7 and k8 (the Table 4 visibility tests) in quick mode.

## Figures
```bash
python3 figures/make_data.py   # results -> figures/data   (run from this directory)
./figures/render.sh            # figures/data -> fig_*.eps
```
| Paper | Script |
|---|---|
| Fig. 7a | `figures/fig_coh_flush.gp` |
| Fig. 7b | `figures/fig_coh_dirty.gp` |
| Fig. 7c | `figures/fig_coh_object.gp` |
| Fig. 7d | `figures/fig_coh_publish.gp` |

## Outputs
- `results/suite_<timestamp>/`: one file per test; every word is classed correct, stale, poison or corrupt.

## Notes
- Needs root and `tier1/data_movement` built (its canary).
- `notes/PLAN.md`: what each test asks; `notes/RESULTS.md`: the measured answers.
