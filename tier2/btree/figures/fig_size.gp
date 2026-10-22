# B+tree tree-size figure, same style as fig_bto.gp / tier2/scan fig_o3.gp (8 pt text), sized for
# ONE ACM column (3.33 in = 8.46 cm): two panels side by side, two-row legend on top
# (fig_size_legend.gp, joined by make_row_size.py). x = tree size (GB, log).
# Usage: gnuplot -e "panel='time'" fig_size.gp    (panel: time | gain)
R = 0.14; T = 0.04; B = 0.62   # R: room for the "100" tick label
LA = 0.60; LB = 0.85   # left margins of the node panel (a) and this panel (b, wider tick labels)
L = LB
PH = 1.76                            # plot height kept; the canvas shrinks with B
P = (8.46 - 4/72.0*2.54 - LA - LB - 2*R) / 2   # both panels get the same plot width
W = L + P + R; H = T + PH + B
# combined-row overrides, used only by render_combo.sh (plot width, margins, plot height in cm)
if (exists("CP")) { P = CP; L = CL; R = CR; B = CB; PH = CPH; W = L + P + R; H = T + PH + B }
set terminal epscairo noenhanced color font "Linux Libertine O,8" fontscale 0.75 size W*1.0 cm, H*1.0 cm
set output sprintf("fig_size_%s.eps", panel)
if (exists("COUT")) { set output COUT }
set tmargin at screen 1 - T/H; set bmargin at screen B/H; set lmargin at screen L/W; set rmargin at screen 1 - R/W
set grid xtics ytics lc rgb "#cfcfcf" lw 0.8 dt 1; set border 15 lc rgb "#333333"; set xtics nomirror; set ytics nomirror
c0="#86b6ef"; c25="#5598e7"; c50="#2a78d6"; c75="#1c5cab"; c100="#0d366b"; cn="#eb6834"; mark="#888888"
unset key
set logscale x 10
set xrange [0.08:130]
set xtics ("0.1" 0.1, "1" 1, "10" 10, "100" 100) font ",8" offset 0,0.55
set xlabel "tree size (GB)" offset 0,1.2 font ",8"
if (exists("CXO")) { set xlabel offset 0,CXO }
# device TLB reach (8 x 1 GB)
set arrow 2 from 8.59, graph 0 to 8.59, graph 1 nohead lc rgb mark dt 2 lw 1 back
set label 2 "TLB reach" at 8.59, graph 0.18 left offset 0.3,0 font ",7" tc rgb mark front
set style textbox opaque noborder margins 0.3,0.05
if (panel eq "time") {
    set logscale y 2
    set yrange [0.08:5]
    set ytics (0.125, 0.25, 0.5, 1, 2, 4) font ",8" offset 0.5,0
    set format y "%.3g"
    set ylabel "Time (s)" offset 2.0,0 font ",8"
    set label 1 "b. tree size" at graph 0.04, graph 0.89 left font ",8" tc rgb "#555555" front boxed
    if (exists("CTAG")) { set label 1 CTAG }
    f = "data/size_time.dat"
    plot f u 1:($2/1000) w lp lc rgb c0 pt 7 ps .4 lw 2, \
         f u 1:($4/1000) w lp lc rgb c50 pt 9 ps .45 lw 2, \
         f u 1:($5/1000) w lp lc rgb c100 pt 13 ps .45 lw 2, \
         f u 1:($6/1000) w lp lc rgb cn pt 7 ps .5 lw 2 dt 2, \
         f u 1:($7/1000) w lp lc rgb cn pt 6 ps .5 lw 1.5 dt 3
} else {
    set yrange [0.8:5.5]
    set ytics 1,1,5 font ",8" offset 0.5,0
    set ylabel "Speedup (x)" offset 1.9,0 font ",8"
    set label 1 "b. partitioning gain" at graph 0.04, graph 0.80 left font ",8" tc rgb "#555555" front boxed
    f = "data/size_gain.dat"
    plot f u 1:2 w lp lc rgb cn pt 7 ps .5 lw 2 dt 2, \
         f u 1:3 w lp lc rgb c0 pt 7 ps .4 lw 2, \
         f u 1:4 w lp lc rgb c100 pt 13 ps .45 lw 2
}
