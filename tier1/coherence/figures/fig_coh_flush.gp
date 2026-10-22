# Figure 4.4(a): one host flush (flushHostCache, clflushopt) vs range size, by host cache state.
# Data: coherence_tax suite_20261010_135135 k1, medians; make_data.py.
# Shaded: ranges above 4 MiB, where one thread no longer holds the prepared state.
set terminal epscairo enhanced color font "Linux Libertine O,10" size 4cm,2cm
set output "fig_coh_flush.eps"
set tmargin at screen 0.99
set bmargin at screen 0.23
set rmargin at screen 0.99
set lmargin at screen 0.15
set grid ytics lc rgb "#dddddd"
set logscale x 2; set logscale y 10
set xrange [40:2e9]; set yrange [0.05:2e5]
set xtics ("64" 64, "4K" 4096, "256K" 262144, "16M" 16777216, "1G" 1073741824) font ",10" offset 0,0.6
set ytics (1,100,10000) font ",10" offset 0.5,0
set format y "10^{%T}"
set xlabel "Range size (bytes)" font ",10" offset 0,1.35
set ylabel "Flush time (µs)" font ",10" offset 1.5,0
set object 1 rect from 4194304,0.05 to 2e9,2e5 fc rgb "#eeeeee" fs solid noborder behind
set key top left Left reverse font ",10" samplen 1.0 spacing 0.9 
plot "data/coh_flush.dat" u 1:2 w lp lw 1.8 pt 7 ps 0.35 lc rgb "#c8461a" t "dirty", \
     "" u 1:3 w lp lw 1.8 pt 9 ps 0.35 lc rgb "#1f5fa9" t "clean", \
     "" u 1:4 w lp lw 1.3 pt 4 ps 0.3  lc rgb "#7a7a7a" dt 2 t "absent"
