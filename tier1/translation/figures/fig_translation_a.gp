# Panel (a) of the translation tax characterisation, as a standalone subfigure.
# Cost per access vs the number of 1 GB granules touched, random and round
# robin, against the model. Split from fig_translation3.gp; data unchanged.
set terminal epscairo enhanced color font "Linux Libertine O,10" size 3cm,2.5cm
set output "fig_translation_a.eps"

# ── plot area flush with canvas on top and right ──────────────────────
set tmargin at screen 0.99
set bmargin at screen 0.23
set rmargin at screen 0.99
set lmargin at screen 0.21

set key top center Left reverse samplen 1 spacing 0.8 font ",12"
set grid ytics lc rgb "#dddddd"
set logscale x 2
set ylabel "ns / access" font ",12" offset 1.9,0
set xlabel "1 GB granules touched" font ",12" offset -1,1.1
set xrange [0.7:200]
set yrange [0:1100]
set xtics (1,8,32,128) font ",12" offset -0.1,0.6
set ytics (0,200,400,600,800) font ",12" offset 0.8,0
set arrow from 8,0 to 8,780 nohead dt 3 lc rgb "#999999"
#set label 1 "E=8" at 2,600 font ",12" tc rgb "#666666"

plot \
  "data/dev_rr.dat"     using 1:2 with linespoints lc rgb "#c0392b" pt 5 ps 0.3 lw 1.5 title " round robin", \
  "data/dev_random.dat" using 1:2 with linespoints lc rgb "#2471a3" pt 7 ps 0.3 lw 1.5 title " random"

#plot \
#  "data/dev_model.dat"  using 1:2 with lines lc rgb "#999999" lw 1.5 dt 2 title " 94+587(1-8/G) ", \
#  "data/dev_rr.dat"     using 1:2 with linespoints lc rgb "#c0392b" pt 5 ps 0.3 lw 1.5 title " round robin ", \
#  "data/dev_random.dat" using 1:2 with linespoints lc rgb "#2471a3" pt 7 ps 0.3 lw 1.5 title " random "
