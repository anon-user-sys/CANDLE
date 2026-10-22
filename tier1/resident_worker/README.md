# Resident worker

A kernel launched once that keeps polling a queue in device memory, so the host can publish work without a new launch. Measures device dequeue and host publication costs.

**Paper:** §4.1.4 (Fig. 4); the protocol used by NDP@CXL in Tier 3 · **Session:** devdax, root

## Build
```bash
./build.sh            # host driver + MU kernel
```

## Run (the paper's runs)
```bash
./run.sh              # -> results/*_<timestamp>/
```
Runs `drain_bench` at 1M and 10M items, three processes each, with host enqueue timing.

## Smoke test
```bash
./smoke.sh            # small, validated run; prints PASS or FAIL
```
One resident kernel that sees host-enqueued work and hands back coherent results.

## Figures
None: the paper reports its numbers in the text.


## Outputs
- `results/enqueue_<timestamp>/`: per-run dequeue and publication costs.

## Notes
- Needs root.
- `scripts/run_burst_bench.sh`, `scripts/run_return_bench.sh`: the in-kernel publish and result-return experiments that led to the coherence rules (not in the paper).
- `notes/RESULTS.md`: detailed results.
