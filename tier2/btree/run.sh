#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export DATA_ROOT=${DATA_ROOT:-$HOME/tpch}    # TPC-H data: $DATA_ROOT/sf_<SF>/
SESSION=$(daxctl list 2>/dev/null | grep -o '"mode":"[a-z-]*"' | head -1 | cut -d'"' -f4)
SF=${SF:-1500}
# The paper's runs (Fig. 9, Fig. 10, §5.2 text). Each session runs its half:
if [ "$SESSION" = devdax ]; then                       # NDP, and the all-DRAM host reference
  scripts/ndp_opt_sweep.sh "$SF"                       # Fig. 9 NDP
  scripts/node_sweep.sh                                # Fig. 10a NDP
  scripts/node_sweep_host.sh                           # Fig. 10a Host-DRAM
  scripts/size_sweep.sh device                         # Fig. 10b NDP
  scripts/ablation.sh "$SF"                            # §5.2.6 partition ranges and layout
else                                                   # system-ram: Host@CXL and the Tiered placements
  MODES=M1_split LAYOUT=compact HOST_PARTITION=1 CORE_BIND=1 DRAM_FRACS="1 0.75 0.5 0.25 0" PLACEMENTS=interleave scripts/run_btree.sh "$SF"   # Fig. 9
  MODES=M1_split LAYOUT=compact CORE_BIND=1 DRAM_FRACS="0.75 0.5 0.25 0.15" PLACEMENTS=block scripts/run_btree.sh "$SF"                    # §5.2.4 levels first
  scripts/size_sweep.sh host                                                                  # Fig. 10b host
  scripts/node_sweep_host.sh M1_dram0_interleave 0 interleave                                 # Fig. 10a Host@CXL
  scripts/node_sweep_host.sh M1_dram50_interleave 0.5 interleave                              # Fig. 10a Tiered 50%
fi
echo "Done ($SESSION). Point figures/make_data*.py at the new results/*.csv, then ./figures/render.sh"
