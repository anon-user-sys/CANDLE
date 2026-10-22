# Panel (c) of the translation tax characterisation, as a standalone subfigure.
# Working set: only accesses that miss the MU cache pay, and the miss cost
# itself grows with the working set. Split from fig_translation3.gp; data
# unchanged.
set terminal epscairo enhanced color font "Linux Libertine O,10" size 3cm,2.5cm
set output "fig_translation_c.eps"

# ── plot area flush with canvas on top and right ──────────────────────
set tmargin at screen 0.99
set bmargin at screen 0.23
set rmargin at screen 0.99
set lmargin at screen 0.21

set key top left Left reverse samplen 1 spacing 0.8 font ",12" 
set grid ytics lc rgb "#dddddd"
set logscale x 2
set ylabel "ns / access" font ",12" offset 1.9,0
set xlabel "Target lines (K)" font ",12" offset 0,1.1
set xrange [12:100000]
set yrange [0:1100]
set xtics (16,256,"4k" 4096,"65k" 65536) font ",12" offset -0.50,0.6
set ytics (200,400,600,800) font ",12" offset 0.8,0

#set label 3 "cache-resident" at 100,200 font ",12" tc rgb "#666666"

plot \
  "data/dev_ws_G8.dat"  using 1:2 with linespoints lc rgb "#1e8449" pt 9 ps 0.3 lw 1.5 title " G=8",\
  "data/dev_ws_G16.dat" using 1:2 with linespoints lc rgb "#2471a3" pt 7 ps 0.3 lw 1.5 title " 16", \
  "data/dev_ws_G64.dat" using 1:2 with linespoints lc rgb "#c0392b" pt 5 ps 0.3 lw 1.5 title " 64"
