# Multi-iteration dispatch: orchestration tax per iteration (T15 fix 2026-09-14,
# 64 tasks). Fresh handle per timed dispatch (median + range of 3 runs) vs the
# same sweep on one shared handle (control run). Replaces the old single-handle
# "async polling kernel, 76x" plot.
set terminal epscairo enhanced color font "Linux Libertine O,10" size 4cm,2cm
set output "fig_amortization.eps"

set logscale x 2
set logscale y 10
set xlabel "Iterations per dispatch (k)" font ",10" offset 0,1.2
set ylabel "µs/iteration" font ",10" offset 1.2,0
set xrange [0.7:1500]
set yrange [0.8:5000]
set xtics (1,4,16,64,256,1024) font ",10" offset 0,0.6
set ytics (1,10,100,1000) font ",10" offset 0.6,0
set format y "%.0s%c"

set tmargin at screen 0.99
set bmargin at screen 0.23
set rmargin at screen 0.97
set lmargin at screen 0.15

set key bottom left Left reverse font ",10" samplen 1 spacing 0.9 
set grid ytics lc rgb "#dddddd"

# ideal: one fresh dispatch (k=1 median) paid once per dispatch
ideal(x) = 1264.665/x

plot \
  "data/fig_amortization_fresh_aged.dat" using 1:5 with linespoints lc rgb "#c0392b" pt 7 ps 0.3 lw 1.5 title "reused Map", \
  "" using 1:2:3:4 with yerrorbars lc rgb "#2471a3" pt 5 ps 0.3 lw 1.5 notitle, \
  "" using 1:2 with lines lc rgb "#2471a3" lw 1.5 title "fresh Map"
