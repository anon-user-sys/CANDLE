# Figure 3: per-dispatch Map aging, and its elimination by rebuild
set terminal epscairo enhanced color font "Linux Libertine O,10" size 4cm,2cm
set output "fig_map_aging.eps"

set xlabel "Dispatches" font ",10" offset 0,1.4
set ylabel "Latency (µs)" font ",10" offset 0.8,0
set xrange [0:3000]
set yrange [0:15000]
set xtics 500 font ",10" offset 0,0.6
set ytics 2000, 2000 font ",10" offset 0.6,0
set format y "%.0s%c"   # SI prefix: 10k, 20k …
set format x "%.0s%c"   

# ── plot area flush with canvas on top and right ──────────────────────
set tmargin at screen 0.99
set bmargin at screen 0.23
set rmargin at screen 0.99
set lmargin at screen 0.16


set key top left Left reverse font ",10" samplen 1 spacing 0.9 
set grid ytics lc rgb "#dddddd"

set label "+3.27 µs/dispatch" at 995,4050 font ",10" tc rgb "#c0392b" rotate by 16
#set label "sawtooth = aging within each block" at 1300,5300 font ",8" tc rgb "#1e8449"

plot \
  "data/fig3_aging_single6000.dat" using 1:2 with lines lc rgb "#c0392b" lw 2 title "Map reused", \
  "data/fig3_aging_rebuild.dat"    using 1:2 with lines lc rgb "#1e8449" lw 2 title "Map rebuilt/220"
