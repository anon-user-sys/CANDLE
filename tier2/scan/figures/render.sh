#!/usr/bin/env bash
# Fig. 8: four panels (count / materialize x 50% / 100% selected) + legend -> fig_o3_row.eps
set -euo pipefail
cd "$(dirname "$0")"
gnuplot fig_o3_legend.gp
for form in count mat; do for sel in 50 100; do gnuplot -e "form='$form'; sel='$sel'" fig_o3.gp; done; done
python3 make_row.py > /dev/null && echo "  fig_o3_row.eps"
