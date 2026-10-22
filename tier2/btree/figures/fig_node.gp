# NDP vs node size, same style as fig_size.gp (8 pt text, one ACM column of 8.46 cm, two panels).
# Usage: gnuplot -e "panel='time'" fig_node.gp    (panel: time | lines)
R = 0.14; T = 0.04; B = 0.62
LA = 0.60; LB = 0.85   # left margins of the node panel (a) and the tree-size panel (b, wider tick labels)
L = LA
PH = 1.76                            # plot height kept; the canvas shrinks with B
P = (8.46 - 4/72.0*2.54 - LA - LB - 2*R) / 2   # both panels get the same plot width
W = L + P + R; H = T + PH + B
# combined-row overrides, used only by render_combo.sh (plot width, margins, plot height in cm)
if (exists("CP")) { P = CP; L = CL; R = CR; B = CB; PH = CPH; W = L + P + R; H = T + PH + B }
set terminal epscairo noenhanced color font "Linux Libertine O,8" fontscale 0.75 size W*1.0 cm, H*1.0 cm
set output sprintf("fig_node_%s.eps", panel)
if (exists("COUT")) { set output COUT }
set tmargin at screen 1 - T/H; set bmargin at screen B/H; set lmargin at screen L/W; set rmargin at screen 1 - R/W
set grid xtics ytics lc rgb "#cfcfcf" lw 0.8 dt 1; set border 15 lc rgb "#333333"; set xtics nomirror; set ytics nomirror
cn="#eb6834"; ink="#555555"; c100="#0d366b"
unset key
set logscale x 2
set xrange [50:5200]
set xtics ("64" 64, "256" 256, "1K" 1024, "4K" 4096) font ",8" offset 0,0.55
set mxtics 2
set xlabel "node size (B)" offset 0,1.2 font ",8"
if (exists("CXO")) { set xlabel offset 0,CXO }
set style textbox opaque noborder margins 0.3,0.05
set datafile missing "?"
f = "data/node_ndp.dat"
if (panel eq "time") {
    set yrange [0:7.4]
    set ytics 0,2,6 font ",8" offset 0.5,0
    set ylabel "Time (s)" offset 1.8,0 font ",8"
    set label 1 "a. node size" at graph 0.04, graph 0.89 left font ",8" tc rgb ink front boxed
    if (exists("CTAG")) { set label 1 CTAG }
    plot f u 1:($8/1000) w lp lc rgb "#2a78d6" pt 9 ps .45 lw 2, \
         f u 1:($7/1000) w lp lc rgb "#0d366b" pt 13 ps .45 lw 2, \
         f u 1:($6/1000) w lp lc rgb "#86b6ef" pt 7 ps .4 lw 2, \
         f u 1:($3/1000) w lp lc rgb cn pt 7 ps .5 lw 2 dt 2, \
         f u 1:($2/1000) w lp lc rgb cn pt 6 ps .5 lw 1.5 dt 3
} else {
    set yrange [0:34]
    set ytics 0,10,30 font ",8" offset 0.5,0
    set ylabel "Per lookup" offset 1.9,0 font ",8"
    set label 1 "b. lines read, levels" at graph 0.96, graph 0.55 right font ",8" tc rgb ink front boxed
    plot f u 1:5 w lp lc rgb c100 pt 13 ps .45 lw 2, \
         f u 1:4 w lp lc rgb c100 pt 12 ps .45 lw 1.5 dt 2
}
