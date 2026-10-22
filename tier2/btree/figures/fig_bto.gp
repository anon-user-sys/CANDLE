# B+tree lookup figures, OPTIMIZED plans (compact layout; NDP partitions the probes per task),
# same format as tier2/scan fig_o3.gp. x = share of the platform's threads used (host cores / 128, NDP tasks / 3048),
# y = time (log). Blue ramp = host placements (DRAM light -> CXL dark), orange = NDP.
# Usage: gnuplot -e "pl='interleave'; dist='uniform'" fig_bto.gp   (pl: interleave | block, dist: uniform | zipf)
# Layout in cm, trimmed to the ink (measured on the rendered panels): every panel has the same plot
# area P x PH; around it only the room its labels need. L = y-tick numbers (+ y title on the
# leftmost panel), B = x-tick numbers + x title, T/R = half a tick label.
first = (pl eq "interleave" && dist eq "uniform")
# text is a true 8 pt (fontscale 0.75 undoes epscairo's default 2/3 scaling); paper body is 9 pt
R = 0.02; T = 0.04; B = 0.70
L1 = 0.77; L2 = 0.52                 # L of the first panel (y title) and of the others
PH = 2.5 - T - B                     # canvas height T+PH+B = 2.5 cm
# P: four panels + make_row.py's three 4 pt gaps fill the ACM two-column \textwidth (7 in = 17.78 cm)
P = (17.78 - 3*4/72.0*2.54 - (L1 + 3*L2) - 4*R) / 4
L = first ? L1 : L2
W = L + P + R; H = T + PH + B
# combined-row overrides, used only by render_combo.sh (plot width, margins, plot height in cm)
if (exists("CP")) { P = CP; L = CL; R = CR; B = CB; PH = CPH; W = L + P + R; H = T + PH + B }
set terminal epscairo noenhanced color font "Linux Libertine O,8" fontscale 0.75 size W*1.0 cm, H*1.0 cm
set output sprintf("fig_bto_%s_%s.eps", pl, dist)
if (exists("COUT")) { set output COUT }
set tmargin at screen 1 - T/H; set bmargin at screen B/H; set lmargin at screen L/W; set rmargin at screen 1 - R/W
set grid xtics ytics lc rgb "#cfcfcf" lw 0.8 dt 1; set border 15 lc rgb "#333333"; set xtics nomirror; set ytics nomirror
set datafile missing "?"
c0="#86b6ef"; c25="#5598e7"; c50="#2a78d6"; c75="#1c5cab"; c100="#0d366b"; cn="#eb6834"; ink="#333333"
unset key
set logscale xy 2
set xrange [5:130]
set yrange [0.25:32]   # shared by all four panels
set xtics ("6" 6.25, "12" 12.5, "25" 25, "50" 50, "100" 100) font ",8" offset 0,0.55
set ytics (0.25, 1, 4, 16) font ",8" offset 0.5,0
set format y "%.2g"
set xlabel "parallelism used (%)" offset 0,0.95 font ",8"
if (exists("CXO")) { set xlabel offset 0,CXO }
# one shared legend (fig_bto_legend.gp) and subfigure captions replace direct labels; only the
# leftmost panel carries the y title (all four share the axis)
if (first) { set ylabel "Time (s)" offset 1.9,0 font ",8" } else { unset ylabel }
# panel tag, bottom left (empty region: few threads means long times, so the lines run high there)
tag = (pl eq "interleave" ? (dist eq "uniform" ? "a." : "b.") : (dist eq "uniform" ? "c." : "d."))
set label 1 sprintf("%s %s, %s", tag, (pl eq "interleave" ? "interleaved" : "levels first"), dist) \
    at graph 0.04, graph 0.11 left font ",8" tc rgb "#555555" front boxed
set style textbox opaque noborder margins 0.3,0.05   # white patch: grid lines stop behind the tag
if (exists("CTAG")) { set label 1 CTAG }
f=sprintf("data/bto_%s_%s_host.dat", pl, dist); g=sprintf("data/bto_%s_%s_ndp.dat", pl, dist)
plot f u ($1/1.28):($2/1000) w lp lc rgb c0 pt 7 ps .4 lw 2, \
     f u ($1/1.28):($3/1000) w lp lc rgb c25 pt 5 ps .4 lw 2, \
     f u ($1/1.28):($4/1000) w lp lc rgb c50 pt 9 ps .45 lw 2, \
     f u ($1/1.28):($5/1000) w lp lc rgb c75 pt 11 ps .45 lw 2, \
     f u ($1/1.28):($6/1000) w lp lc rgb c100 pt 13 ps .45 lw 2, \
     g u ($1/30.48):($2/1000) w lp lc rgb cn pt 7 ps .5 lw 2 dt 2
