# Figure 4.4(d): lines a host reader saw before they were written back, per device publish
# protocol (of 12.5 M per run). Bars = median of 3 runs, whiskers = min-max.
# Data: coherence_tax suite_20261010_135135 k10; make_data.py.
set terminal epscairo enhanced color font "Linux Libertine O,10" size 4cm,2cm
set output "fig_coh_publish.eps"
set tmargin at screen 0.99
set bmargin at screen 0.23
set rmargin at screen 0.99
set lmargin at screen 0.15
set grid ytics lc rgb "#dddddd"
set style fill solid 1.0 noborder
set boxwidth 0.7
set xrange [-0.6:5.6]; set yrange [0:95]
set ytics 0,20,80 font ",10" offset 0.5,0
# Two-line tick labels with a tighter line gap: gnuplot advances a line by the TIC font size, so
# the tic font is small (7) and each line is drawn at 10 pt with {/=10 ...}.
set xtics font ",7" nomirror norotate center offset 0,0.45 scale 0
tl(s) = (strstrt(s, "\n") ? sprintf("{/=10 %s}\n{/=10 %s}", s[1:strstrt(s, "\n")-1], s[strstrt(s, "\n")+1:]) : sprintf("{/=10 %s}", s))
set ylabel "Lines seen early (k)" font ",10" offset 1.5,-0.7
unset key
plot "data/coh_publish.dat" u 1:($3):($0 < 3 ? 0xc8461a : 0x1f5fa9):xtic(tl(strcol(2))) w boxes lc rgb variable, \
     "" u 1:($3 > 0 ? $3 : 1/0):4:5 w yerrorbars pt 0 ps 0 lw 1.2 lc rgb "#000000", \
     "" u 1:($3 == 0 ? 5 : 1/0):("0") w labels font ",10" tc rgb "#1f5fa9"
