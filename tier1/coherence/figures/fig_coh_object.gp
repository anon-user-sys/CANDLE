# Figure 4.4(c): ns per lookup, object read plainly or after invalidating it (flushHostCache),
# hot (in host cache) or cold. Data: coherence_tax suite_20261010_135135 k5, 1 thread, median.
set terminal epscairo enhanced color font "Linux Libertine O,10" size 4cm,2cm
set output "fig_coh_object.eps"
set tmargin at screen 0.99
set bmargin at screen 0.23
set rmargin at screen 0.99
set lmargin at screen 0.15
set grid ytics lc rgb "#dddddd"
set logscale x 2; set logscale y 10
set xrange [48:5500]; set yrange [8:60000]
set xtics ("64" 64, "256" 256, "1K" 1024, "4K" 4096) font ",10" offset 0,0.6
set ytics (10,100,1000,10000) font ",10" offset 0.5,0
set format y "10^{%T}"
set xlabel "Object size (bytes)" font ",10" offset 0,1.35
set ylabel "ns per lookup" font ",10" offset 1.5,0
set key at graph 0.02,0.97 left top vertical maxrows 2 Left reverse font ",10" samplen 0.8 spacing 0.9 
plot "data/coh_object.dat" u 1:3 w lp lw 1.8 pt 7 ps 0.35 lc rgb "#1f5fa9" t "hot, inval.", \
     "" u 1:2 w lp lw 1.3 pt 6 ps 0.35 lc rgb "#1f5fa9" dt 2 t "hot", \
     "" u 1:5 w lp lw 1.8 pt 5 ps 0.35 lc rgb "#c8461a" t "cold, inval.", \
     "" u 1:4 w lp lw 1.3 pt 4 ps 0.35 lc rgb "#c8461a" dt 2 t "cold"
