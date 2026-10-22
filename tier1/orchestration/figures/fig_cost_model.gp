# Figure 1: dispatch cost, aged vs fresh handle (T1, paired sweep, median + range of 3 runs)
# Regime boundary moved 1024 -> 4096 (2026-10-08): fresh curve peaks at 1024 and dips
# through 4096; alpha_task refit from 4096 = 0.097 us/task, R2=0.995.
set terminal epscairo enhanced color font "Linux Libertine O,10" size 4cm,2cm
set output "fig_cost_model.eps"

set logscale x 2
set logscale y 10
set xlabel "Task count" font ",10" offset 0,1.2
set ylabel "Latency (µs)" font ",10" offset 0.8,0
set xrange [0.7:200000]
set yrange [800:25000]
set xtics (1,16,256,4096,65536) font ",10" offset 0,0.6
set ytics (1000,2000,5000,10000,20000) font ",10" offset 0.6,0
set format y "%.0s%c"

# ── plot area flush with canvas on top and right ──────────────────────
set tmargin at screen 0.99
set bmargin at screen 0.23
set rmargin at screen 0.99
set lmargin at screen 0.16

set key top left Left reverse font ",10" samplen 1 spacing 0.9
set grid ytics lc rgb "#dddddd"

# regime boundaries
set arrow from 64,800 to 64,30000 nohead dt 3 lc rgb "#999999"
set arrow from 4096,800 to 4096,30000 nohead dt 3 lc rgb "#999999"
# regime labels along the bottom, clear of both series
set label "flat"    at 3,1700    font ",10" tc rgb "#666666"
set label "fan-out" at 150,1700  font ",10" tc rgb "#666666"
set label "linear"  at 9000,1700 font ",10" tc rgb "#666666"

plot \
  "data/fig1_aged.dat"  using 1:2:3:4 lc rgb "#c0392b" pt 7 ps 0.25 lw 1.5 notitle, \
  "data/fig1_aged.dat"  using 1:2:3:4 with linespoints lc rgb "#c0392b" pt 7 ps 0.25 lw 1.5 title " reused map ", \
  "data/fig1_fresh.dat" using 1:2:3:4 lc rgb "#2471a3" pt 5 ps 0.25 lw 1.5 notitle, \
  "data/fig1_fresh.dat" using 1:2:3:4 with linespoints lc rgb "#2471a3" pt 5 ps 0.25 lw 1.5 title " fresh map "
