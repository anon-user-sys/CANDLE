# Option-3 scan figures: x = share of the platform's threads used (host cores / 128, NDP tasks / 3048),
# y = time (log). Blue ramp = host placements (DRAM light -> CXL dark), orange = NDP.
# Usage: gnuplot -e "form='count'; sel='50'" fig_o3.gp   (form: count | mat)
# Layout in cm, trimmed to the ink (measured on the rendered panels): every panel has the same plot
# area P x PH; around it only the room its labels need. L = y-tick numbers (+ y title on the
# leftmost panel, count 50%), B = x-tick numbers + x title, T/R = half a tick label.
first = (form eq "count" && sel eq "50")
# text is a true 8 pt (fontscale 0.75 undoes epscairo's default 2/3 scaling); paper body is 9 pt
R = 0.02; T = 0.04; B = 0.70
L1 = 0.77; L2 = 0.52                 # L of the first panel (y title) and of the others
PH = 2.5 - T - B                     # canvas height T+PH+B = 2.5 cm
# P: four panels + make_row.py's three 4 pt gaps fill the ACM two-column \textwidth (7 in = 17.78 cm)
P = (17.78 - 3*4/72.0*2.54 - (L1 + 3*L2) - 4*R) / 4
L = first ? L1 : L2
W = L + P + R; H = T + PH + B
set terminal epscairo noenhanced color font "Linux Libertine O,8" fontscale 0.75 size W*1.0 cm, H*1.0 cm
set output sprintf("fig_o3_%s_sel%s.eps", form, sel)
set tmargin at screen 1 - T/H; set bmargin at screen B/H; set lmargin at screen L/W; set rmargin at screen 1 - R/W
set grid xtics ytics lc rgb "#cfcfcf" lw 0.8 dt 1; set border 15 lc rgb "#333333"; set xtics nomirror; set ytics nomirror
set datafile missing "?"
c0="#86b6ef"; c25="#5598e7"; c50="#2a78d6"; c75="#1c5cab"; c100="#0d366b"; cn="#eb6834"; ink="#333333"
unset key
set logscale xy 2
set xrange [5:130]
set yrange [0.25:20]   # shared by all four panels so count and materialized compare directly
set xtics ("6" 6.25, "12" 12.5, "25" 25, "50" 50, "100" 100) font ",8" offset 0,0.55
set ytics (0.25, 1, 4, 16) font ",8" offset 0.5,0
set format y "%.2g"
set xlabel "parallelism used (%)" offset 0,0.95 font ",8"
# one shared legend (fig_o3_legend.gp) and subfigure captions replace direct labels; only the
# leftmost panel carries the y title (all four share the axis)
if (first) { set ylabel "Time (s)" offset 1.9,0 font ",8" } else { unset ylabel }
# panel tag, bottom left (empty region: few threads means long times, so the lines run high there)
tag = (form eq "count" ? (sel eq "50" ? "a." : "b.") : (sel eq "50" ? "c." : "d."))
set label 1 sprintf("%s %s, sel. %s%%", tag, (form eq "mat" ? "materialized" : "count"), sel) \
    at graph 0.04, graph 0.11 left font ",8" tc rgb "#555555" front boxed
set style textbox opaque noborder margins 0.3,0.05   # white patch: grid lines stop behind the tag
f=sprintf("data/o3_%s_sel%s_host.dat", form, sel); g=sprintf("data/o3_%s_sel%s_ndp.dat", form, sel)
plot f u ($1/1.28):($2/1000) w lp lc rgb c0 pt 7 ps .4 lw 2, \
     f u ($1/1.28):($3/1000) w lp lc rgb c25 pt 5 ps .4 lw 2, \
     f u ($1/1.28):($4/1000) w lp lc rgb c50 pt 9 ps .45 lw 2, \
     f u ($1/1.28):($5/1000) w lp lc rgb c75 pt 11 ps .45 lw 2, \
     f u ($1/1.28):($6/1000) w lp lc rgb c100 pt 13 ps .45 lw 2, \
     g u ($1/30.48):($2/1000) w lp lc rgb cn pt 7 ps .5 lw 2 dt 2
