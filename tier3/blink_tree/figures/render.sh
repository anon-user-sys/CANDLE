#!/usr/bin/env bash
# Fig. 13 (phases, scaling) -> fig_blink_row.eps
set -euo pipefail
cd "$(dirname "$0")"
gnuplot fig_blink_legend.gp
gnuplot fig_blink_phase.gp
gnuplot fig_blink_scale.gp
python3 make_row.py > /dev/null && echo "  fig_blink_row.eps"
