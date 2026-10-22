# Sub count: fixed cost vs handle aging (T6 paired sweep 2026-09-14, 64 tasks,
# median + range of 3 runs). Replaces the old aged-only "alpha_launch IS per-Sub
# init" plot, which TODO E3/E4 disproved.
# Fresh DeviceInit (2-7 us) is not plotted: it lies on the x-axis; state it in
# the caption instead.
set terminal epscairo enhanced color font "Linux Libertine O,10" size 4cm,2cm
set output "fig_subcount.eps"

set logscale x 2
set xlabel "Subs allocated" font ",10" offset 0,1.2
set ylabel "Latency (µs)" font ",10" offset 0.4,0
set xrange [0.8:30]
set yrange [0:4000]
set xtics (1,2,4,8,16,24) font ",10" offset 0,0.6
set ytics 0,1000,3000 font ",10" offset 0.6,0
set format y "%.0s%c"

set tmargin at screen 0.99
set bmargin at screen 0.23
set rmargin at screen 0.97
set lmargin at screen 0.15

set key at graph 0.02,0.97 left top vertical maxrows 2 Left reverse font ",10" samplen 0.8 spacing 0.9 width -10
set grid ytics lc rgb "#dddddd"
set bars small

# cols: 1 numSub | 2-4 fresh med/min/max | 5-7 aged med/min/max | 8 DevInit fresh | 9 DevInit aged
plot \
  NaN with linespoints lc rgb "#c0392b" pt 7 ps 0.3 lw 1.5 title "reused", \
  NaN with linespoints lc rgb "#2471a3" pt 5 ps 0.3 lw 1.5 title "fresh", \
  "data/fig_subcount_fresh_aged.dat" using 1:5:6:7 with yerrorlines lc rgb "#c0392b" pt 7 ps 0.3 lw 1.5 notitle, \
  "" using 1:9 with linespoints lc rgb "#c0392b" dt 2 pt 6 ps 0.3 lw 1.2 title "reused [DevInit]", \
  "" using 1:2:3:4 with yerrorlines lc rgb "#2471a3" pt 5 ps 0.3 lw 1.5 notitle
