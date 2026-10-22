# Figure 4.4(b): time to flush a 16 MiB range vs fraction of its lines dirty (rest absent).
# Data: coherence_tax suite_20261010_135135 k2, medians of 30 reps; make_data.py.
set terminal epscairo enhanced color font "Linux Libertine O,10" size 4cm,2cm
set output "fig_coh_dirty.eps"
set tmargin at screen 0.99
set bmargin at screen 0.23
set rmargin at screen 0.99
set lmargin at screen 0.15

set grid ytics lc rgb "#dddddd"
set xrange [-5:105]; set xtics 0,25,100 font ",10" offset 0,0.6
set yrange [0:1.6]; set ytics 0,0.5,1.5 font ",10" offset 0.5,0
set format y "%.1f"
set xlabel "Dirty lines (%)" font ",10" offset 0,1.35
set ylabel "Flush time (ms)" font ",10" offset 1.5,0
set key bottom right Left reverse font ",10" samplen 1.0 spacing 0.9 
plot "data/coh_dirty.dat" u 1:2 w lp lw 1.8 pt 7 ps 0.35 lc rgb "#1f5fa9" t "uniform", \
     "" u 1:3 w lp lw 1.8 pt 5 ps 0.35 lc rgb "#c8461a" t "clustered"
