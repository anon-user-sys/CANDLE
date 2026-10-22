#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
STAMP=$(date +%Y%m%d_%H%M%S)
export DATA_ROOT=${DATA_ROOT:-$HOME/tpch}       # TPC-H tables: $DATA_ROOT/sf_<SF>/{orders,lineitem}.tbl
# The paper's runs (Fig. 11, Fig. 12). Device session, root. Each step writes results/devdax_modes_*/.
HOST8=(PARTITIONS=8192 PREFETCH=32 HOST_DIRECT=1 OWNER_BUILD=1)   # Host@CXL and Hybrid: best partition count
OWN=(PARTITIONS=1024 PREFETCH=32 HOST_DIRECT=1 OWNER_BUILD=1)     # NDP@CXL: best partition count
env MODES="2 3" "${HOST8[@]}" scripts/run_devdax_modes.sh 100 50  # Fig. 11a, full platform
env MODES=4 "${OWN[@]}" scripts/run_devdax_modes.sh 100 50
for sf in 1 10; do env MODES="2 3 4" "${OWN[@]}" scripts/run_devdax_modes.sh "$sf" 50; done   # Fig. 11b, cost per tuple
for sel in 0 100; do env MODES=4 "${OWN[@]}" scripts/run_devdax_modes.sh 100 "$sel"; done     # Fig. 12b, NDP@CXL
scripts/run_quick4.sh                                              # Fig. 12a host shares, Fig. 12b Host@CXL and Hybrid
scripts/run_quick5.sh                                              # Fig. 12a device shares
echo "Done. Point figures/make_data.py at the new results/devdax_modes_* folders, then ./figures/render.sh"
