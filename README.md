# CANDLE

**When Is CXL Near-Data Execution Worth It? A Real-Silicon Study of Computational Memory Expanders for Database Systems**
<br>Artifact for the SIGMOD 2027 Experiments & Analysis submission.

<p align="center">
  <a href="https://anon-user-sys.github.io/CANDLE/">
    <img src="docs/badge.svg" alt="Interactive Documentation" width="460">
  </a>
</p>

CANDLE (**C**XL-**A**ccelerated **N**ear-**D**ata **L**ogic **E**valuation) measures when running a database
fragment on a computational CXL memory expander, the XCENA MX1P, beats running it on the host over the same
CXL-resident data, once preparation, launch and result handoff are counted. This repository holds every
benchmark, the raw measurements behind the paper's figures, and the scripts that turn one into the other.

## Benchmarks

| Tier | Component | Measures | Paper |
|---|---|---|---|
| 1 · Taxes | [`tier1/orchestration`](tier1/orchestration) | launch and dispatch cost (α) | §4.1 · Fig. 3 · Table 2 |
| | [`tier1/translation`](tier1/translation) | device address translation (τ) | §4.2 · Fig. 5 |
| | [`tier1/data_movement`](tier1/data_movement) | explicit copies, DMA vs. direct (β) | §4.3 · Fig. 6 · Table 3 |
| | [`tier1/coherence`](tier1/coherence) | software visibility handoffs (γ) | §4.4 · Fig. 7 · Table 4 |
| | [`tier1/resident_worker`](tier1/resident_worker) | a kernel launched once, fed through a queue | §4.1.4 |
| 2 · DB operations | [`tier2/scan`](tier2/scan) | selective scan, count vs. materialize | §5.1 · Fig. 8 · Table 5 |
| | [`tier2/btree`](tier2/btree) | B+-tree point lookups | §5.2 · Fig. 9 · Fig. 10 |
| 3 · Composite workloads | [`tier3/hash_join`](tier3/hash_join) | partitioned hash join | §6.1 · Fig. 11 · Fig. 12 |
| | [`tier3/blink_tree`](tier3/blink_tree) | B-link tree under YCSB-A updates | §6.2 · Fig. 13 |

Every component has the same layout:

```
README.md   what it measures, how to run it
build.sh    host driver + MU kernel
run.sh      the paper's runs  -> results/
smoke.sh    a small, validated run -> PASS / FAIL
scripts/    the underlying sweeps
results/    the raw measurements behind the paper's figures
figures/    make_data.py (results -> data) · render.sh (data -> .eps) · fig_*.gp
```

## Quick start

`candle.sh` runs the same verb on one component or on all of them:

```bash
./candle.sh list                     # the components
./candle.sh figures all              # regenerate the paper's figures from results/ (no MX1P needed)
./candle.sh build all                # build, as your user
sudo ./candle.sh smoke all           # small validated runs on the MX1P: PASS / FAIL per component
./candle.sh run <component>          # the paper's runs for one component (long)
```

A component is named by its path (`tier1/coherence`) or its last part (`coherence`).

## Without an MX1P

- **Figures:** `./candle.sh figures all` needs only Python 3, gnuplot and Ghostscript. Every paper figure is rebuilt from the
  shipped raw measurements.
- **Code:** the XCENA SDK emulator runs the same binaries for a functional check (timings are not meaningful
  there). See the Reproduce tab of the [documentation](https://anon-user-sys.github.io/CANDLE/#repro).

## Requirements for measuring

- An XCENA MX1P on an x86-64 Linux host, with PXL SDK 2.0.0, the MU LLVM 21.1.2 toolchain, device firmware 1.0.4
  and the `mx_dma` driver. The paper's host: 2 × Intel Xeon 6756E, Ubuntu 24.04, Linux 6.8.
- TPC-H `dbgen` tables for the scan and the hash join, in `$DATA_ROOT/sf_<SF>/`.
- Two machine sessions: **devdax** (device memory for PXL: NDP, Host@CXL and Hybrid runs) and **system-ram**
  (device memory as a NUMA node: the Tiered runs). Root wherever the host loads or stores device memory.

The [documentation](https://anon-user-sys.github.io/CANDLE/) explains the device, the cost models, every result
and the session setup, and lists which run produces each figure.
