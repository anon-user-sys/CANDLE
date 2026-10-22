#!/usr/bin/env bash
# Fig. 11 (phases, cost per tuple) -> fig_hj_row.eps; Fig. 12 (share of threads, selectivity) -> fig_hj_sweep.eps
set -euo pipefail
cd "$(dirname "$0")"
gnuplot fig_hj_legend.gp
for g in fig_hj_phase100 fig_hj_perkey fig_hj_threads fig_hj_sel; do gnuplot $g.gp; done
python3 make_row.py fig_hj_row fig_hj_phase100 fig_hj_perkey fig_hj_legend > /dev/null && echo "  fig_hj_row.eps"
python3 make_row.py fig_hj_sweep fig_hj_threads fig_hj_sel > /dev/null && echo "  fig_hj_sweep.eps"
