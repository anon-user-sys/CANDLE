#!/usr/bin/env bash
# Fig. 9  (one column: interleaved placements, uniform and Zipfian) -> fig_btree_inter_row.eps
# Fig. 10 (one column: node size, tree size)                        -> fig_btree_ns_row.eps
set -euo pipefail
cd "$(dirname "$0")"
# Fig. 9: shared panel geometry in cm, as in the paper (one 8.46 cm column, two panels)
B=0.62; PH=1.76; GAP=0.1411; LA=0.80; LB=0.52; RA=0.02; RB=0.14
P=$(python3 -c "print((8.46 - $GAP - ($LA+$LB) - ($RA+$RB)) / 2)")
common="CB=$B; CPH=$PH; CXO=1.2; CP=$P"
gnuplot fig_inter_legend.gp
gnuplot -e "pl='interleave'; dist='uniform'; $common; CL=$LA; CR=$RA; COUT='fig_inter_a.eps'; CTAG='a. uniform'" fig_bto.gp
gnuplot -e "pl='interleave'; dist='zipf'; $common; CL=$LB; CR=$RB; COUT='fig_inter_b.eps'; CTAG='b. Zipfian'" fig_bto.gp
python3 make_row_inter.py > /dev/null && echo "  fig_btree_inter_row.eps"
# Fig. 10
gnuplot fig_size_legend.gp
gnuplot -e "panel='time'" fig_node.gp
gnuplot -e "panel='time'" fig_size.gp
python3 make_row_ns.py > /dev/null && echo "  fig_btree_ns_row.eps"
