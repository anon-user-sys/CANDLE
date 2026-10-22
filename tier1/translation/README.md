# Translation tax (τ): pointer-chase kernel

Infers the device's address-translation structure by timing dependent reads over K target lines spread across R regions, holding cache demand fixed while the number of 1 GB granules touched varies.

**Paper:** §4.2, Fig. 5 · **Session:** devdax

## Build
```bash
./build.sh            # host driver + MU kernel
```

## Run (the paper's runs)
```bash
./run.sh              # -> results/*_<timestamp>/
```
Runs the device sweeps (granules, spacing, round-robin, target lines) and the host 4 KB control.

## Smoke test
```bash
./smoke.sh            # small, validated run; prints PASS or FAIL
```
One 16-granule configuration; passes if every pass's checksum matches the host replay.

## Figures
```bash
./figures/render.sh            # figures/data -> fig_*.eps
```
| Paper | Script |
|---|---|
| Fig. 5a | `figures/fig_translation_a.gp` |
| Fig. 5b | `figures/fig_translation_b.gp` |
| Fig. 5c | `figures/fig_translation_c.gp` |

## Outputs
- `results/<timestamp>/device_*.csv`, `host4k_*.csv`: one row per configuration with `ns_per_access` and `sum_ok`.
- `figures/data/`: the plotted points (no `make_data.py`; fit with `scripts/analyze_tlb.py`).

## Notes
- `notes/DESIGN.md`: the method, controls and host validation.
- `notes/RESULTS.md`: the fitted model `t = 94.2 + 587.2·max(0, 1 − 8/G)` ns.
