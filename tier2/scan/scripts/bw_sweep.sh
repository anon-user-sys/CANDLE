#!/bin/bash
# bw_sweep.sh [gb] [iters]
#
# Read-bandwidth curves for the three paths the scan uses (devdax session):
#   host     host threads on host DRAM (node 0)                  par 1..128
#   device   MU tasks on device memory, prefetch window 0 and 2   tasks 64..3048
#   hostdev  host threads on device memory over CXL.mem (root)    par 1..128
#   flush    host threads flushHostCache device memory (root), CPU cache state absent/clean/dirty
# PATHS selects which to run (default all); hostdev runs under sudo.
set -uo pipefail

GB=${1:-16}; ITERS=${2:-3}
cd "$(dirname "$0")/.." || exit 1
PATHS=${PATHS:-"host device hostdev flush"}
HOST_PAR=${HOST_PAR:-"1 2 4 8 16 32 64 128"}
DEV_PAR=${DEV_PAR:-"64 128 256 512 1024 2048 3048"}
WINDOWS=${WINDOWS:-"0 2"}
SUDO=${SUDO:-sudo}
mkdir -p results
CSV=results/bw_$(date +%Y%m%d_%H%M%S).csv
ERR=${CSV%.csv}.err

[ "$(daxctl list 2>/dev/null | awk -F'"' '/"mode"/{print $4}')" = devdax ] || { echo "needs devdax mode"; exit 1; }
echo "# bw sweep gb=$GB iters=$ITERS paths=$PATHS load=$(cut -d' ' -f1 /proc/loadavg)" | tee "$CSV"
./bw --header >> "$CSV"

for path in $PATHS; do
  case $path in
    host)    for p in $HOST_PAR; do echo "  host par=$p" >&2
               numactl --cpunodebind=0 --membind=0 ./bw --path host --gb "$GB" --par "$p" --iters "$ITERS" >> "$CSV" 2>>"$ERR"; done ;;
    device)  for w in $WINDOWS; do for p in $DEV_PAR; do echo "  device tasks=$p window=$w" >&2
               ./bw --path device --gb "$GB" --par "$p" --prefetch "$w" --iters "$ITERS" >> "$CSV" 2>>"$ERR"; done; done ;;
    hostdev) for p in $HOST_PAR; do echo "  hostdev par=$p" >&2
               $SUDO numactl --cpunodebind=0 --membind=0 ./bw --path hostdev --gb "$GB" --par "$p" --iters "$ITERS" >> "$CSV" 2>>"$ERR"; done ;;
    flush)   for st in ${FLUSH_STATES:-absent clean dirty}; do for p in $HOST_PAR; do echo "  flush state=$st par=$p" >&2
               $SUDO numactl --cpunodebind=0 --membind=0 ./bw --path flush --state "$st" --gb "$GB" --par "$p" --iters "$ITERS" >> "$CSV" 2>>"$ERR"; done; done ;;
  esac
done

python3 - "$CSV" <<'EOF'
import csv, sys, statistics as st
rows = [r for r in csv.DictReader(l for l in open(sys.argv[1]) if not l.startswith('#')) if r.get('ok') in ('0', '1')]
g = {}
for r in rows:
    if int(r['iter']) >= 1:
        g.setdefault((r['path'], int(r['prefetch'])), {}).setdefault(int(r['par']), []).append(float(r['gb_per_s']))
for (path, w), d in sorted(g.items()):
    label = f"{path}" + (f" w{w}" if path == 'device' else "")
    print(f"{label:10} " + "  ".join(f"{p}:{st.median(v):.1f}" for p, v in sorted(d.items())) + "  GB/s")
print(f"rows {len(rows)}, bad {sum(r['ok'] != '1' for r in rows)}")
EOF
echo "results: $CSV"
