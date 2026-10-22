# Figure 4.3(d): cost per op vs write fraction, random offsets in device memory.
# Data: 2026-10-09 pinned config, D12, median of 4 runs, bars = min-max; make_data.py.
set terminal epscairo enhanced color font "Linux Libertine O,10" size 4cm,2cm
set output "fig_rw_mix.eps"

set xrange [-8:108]; set xtics 0,25,100 font ",10" offset 0, 0.6
set yrange [20:58]; set ytics 20,10,50 font ",10" offset 0.5,0
set grid ytics lc rgb "#dddddd"
set xlabel "Writes (% of accesses)" font ",10" offset 0.5,1.2
set ylabel "ns per operation" font ",10" offset 1.5,-0.5

set tmargin at screen 0.99
set bmargin at screen 0.23
set rmargin at screen 0.99
set lmargin at screen 0.15

unset key
set arrow from 25,35.5 to 25,27.4 lw 1 lc rgb "#c8461a" filled
set label 1 "cheapest" at 23,40 center font ",10" tc rgb "#c8461a"
plot "data/rw_mix.dat" u 1:2 w l lw 1.8 lc rgb "#1f5fa9", \
     "" u 1:2:3:4 w yerrorbars pt 7 ps 0.45 lw 1 lc rgb "#1f5fa9"
