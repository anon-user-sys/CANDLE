# data_movement_tax — how to run

## Unprivileged (DMA + host baseline only)
```bash
./build.sh
./scripts/run_sweep.sh --no-direct
```
Skips every direct host-pointer arm on device memory. Safe, no root, but yields
only ~24/3/10/16 rows for d1/d2/d4/d10 instead of the full 84/3/35/16.

## Root (full sweep, includes direct host access to memAlloc memory)
Direct load/store on `memAlloc` memory needs root — without it the mapping
silently fails and the first touch SIGBUSes.

```bash
# 1. preconditions
daxctl list | grep -q '"mode":"devdax"' || \
  sudo daxctl reconfigure-device --mode=devdax --force dax0.0
sudo systemctl restart pxl_resourced.service      # only AFTER any mode flip
sudo xcena_cli num-device                          # expect: 1, answers instantly
sudo xcena_cli device-info 0 | grep Computable     # expect: True

# 2. confirm nobody else is on the device (SHARED BOX)
who
ps -eo user,pid,etime,cmd | grep -iE 'pxl|xcena|mubin|[.]/main' | grep -v grep
sudo fuser -v /dev/dax*

# 3. sweep
sudo ./scripts/run_sweep.sh

# 4. post-sweep canary — the device can degrade mid-sweep
sudo xcena_cli num-device
```

Knobs: `TESTS="d1 d2"`, `REPS=5`, `TIMEOUT=3600`, `OUT=/path`.

## Reading the output
Per-run status is `ok` / `NO_DATA` / `TIMEOUT` / `CRASH(rc=N)` / `INVALID`.

`INVALID` means a **bit-exact round-trip mismatch** — the bytes came back wrong
while `PxlResult` said Success. The script then writes
`rN_test.attribution.txt` capturing `xcena_cli`, `fuser`, `who`, process list
and `dmesg`. **That capture is the whole point**: a mismatch clears within
minutes and afterwards cannot be attributed to the device vs. a concurrent user.


Do not report numbers from a run whose status was not `ok`.

## Gotchas that cost real time
- **96 MiB L3 per socket.** Any pointer-walk buffer under ~100 MiB measures
  cache, not memory, and CXL then looks as fast as DRAM. D10/D12 use 256 MiB
  and 1 GiB for this reason; do not shrink them.
- **TLB-resident and cache-defeating are mutually exclusive here** (needs
  <16 MiB and >96 MiB). Translation cost is measured separately, not in D10.
- `flushHostCache(opt=true)` skips clean ranges, so dirty the buffer first or
  flush cost is undermeasured.
- Never `modprobe -r mx_dma` (decommits CXL regions → reboot).
- Aborting mid-dispatch can wedge the device; the harness counts failures
  instead of aborting and registers a signal handler.
