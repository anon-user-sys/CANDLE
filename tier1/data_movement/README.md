# Data-movement tax (β): transfer sweep

Times explicit copies between host and device memory by path (DMA, direct memcpy, 8-byte loop, NT stores) and size, plus direct access to device memory against local DRAM.

**Paper:** §4.3, Fig. 6, Table 3 · **Session:** devdax, root

## Build
```bash
./build.sh            # host driver + MU kernel
```

## Run (the paper's runs)
```bash
./run.sh              # -> results/*_<timestamp>/
```
Runs D1, D10 and D12 with the mx_dma kthreads pinned to the device socket, then the DMA-floor diagnosis.

## Smoke test
```bash
./smoke.sh            # small, validated run; prints PASS or FAIL
```
`main d1 --verify-only`: bit-exact round trips at every size. It is also the device canary the other components use.

## Figures
```bash
python3 figures/make_data.py   # results -> figures/data   (run from this directory)
./figures/render.sh            # figures/data -> fig_*.eps
```
| Paper | Script |
|---|---|
| Fig. 6a | `figures/fig_transfer_h2d.gp` |
| Fig. 6b | `figures/fig_transfer_d2h.gp` |
| Fig. 6c | `figures/fig_access_pattern.gp` |
| Fig. 6d | `figures/fig_rw_mix.gp` |

## Outputs
- `results/pinned_*`, `results/followup_*`: the raw runs the figures use.

## Notes
- Needs root: host loads and stores on `memAlloc` memory fault without it (`notes/PERMISSIONS.md`).
- `notes/RUNBOOK.md`: preconditions and how to read a failed run.
