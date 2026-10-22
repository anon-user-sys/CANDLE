# Figure 4.3(a): host-to-device transfer latency vs size, DMA vs direct paths.
# Data: 2026-10-09 pinned sweep (mx_dma kthreads on CPUs 8,9, node 0, default C-states),
# median of 3 runs; built by make_data.py. Panel size matches Sec 4.1 (4 x 3.5 cm).
set terminal epscairo enhanced color font "Linux Libertine O,10" size 4cm,2cm
set output "fig_transfer_h2d.eps"
# 2026-10-10: NT store = no flush (col 14, the correct path, coherence_tax k7/k13); the
# flushed NT arm stays as a dotted line (col 11). Crossover band moved to 16-64 MiB.

set logscale x 2; set logscale y 10
set xrange [40:400000000]; set yrange [0.3:10000000]
set xtics ("64" 64, "4K" 4096, "256K" 262144, "16M" 16777216) font ",10" offset 0,0.6
set ytics (1,100,10000,1000000) font ",10" offset 0.7,0
set format y "10^{%T}"
set grid ytics lc rgb "#dddddd"
set xlabel "Transfer size (bytes)" font ",10" offset 0,1.3
set ylabel "Latency (µs)" font ",10" offset 1.5,0

set tmargin at screen 0.99
set bmargin at screen 0.23
set rmargin at screen 0.99
set lmargin at screen 0.15

set key at graph 0.02,0.97 left top horizontal maxcols 3 Left reverse font ",10" samplen 0.8 spacing 0.9 
#set label 1 "H{/Symbol \256}D" at graph 0.97,0.07 right font ",10"
set object 1 rect from 16777216,0.3 to 67108864,20000000 fc rgb "#eeeeee" fs solid noborder behind
plot "data/transfer_h2d.dat" u 1:2  w lp lw 1.8 pt 7 ps 0.35 lc rgb "#1f5fa9" t "DMA", \
     "" u 1:14 w lp lw 1.8 pt 9 ps 0.35 lc rgb "#18855c" t "NT store", \
     "" u 1:11 w l  lw 1   lc rgb "#18855c" dt 3 t "NT + flush", \
     "" u 1:5  w lp lw 1.3 pt 5 ps 0.3  lc rgb "#c8461a" t "memcpy", \
     "" u 1:8  w lp lw 1   pt 4 ps 0.25 lc rgb "#7a7a7a" dt 2 t "word"
