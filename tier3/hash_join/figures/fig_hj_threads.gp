# Hash join, fig_hj_sweep panel a: sf_100, 50%, total time against the share of the
# platform's threads (host cores / 128, device tasks / 3,048), owner build. Data: data/threads100_*.dat.
R = 0.05; T = 0.04; B = 0.62          # left column: thin right margin, plot stretches toward panel b/d (was 0.14)
LA = 1.40; LB = 0.85
L = LB
PH = 1.76
P = (8.46 - 4/72.0*2.54) / 2 - L - R   # each panel (labels included) takes half the column
W = L + P + R; H = T + PH + B
set terminal epscairo noenhanced color font "Linux Libertine O,8" fontscale 0.75 size W*1.0 cm, H*1.0 cm
set output "fig_hj_threads.eps"
set tmargin at screen 1 - T/H; set bmargin at screen B/H; set lmargin at screen L/W; set rmargin at screen 1 - R/W
set grid xtics ytics lc rgb "#cfcfcf" lw 0.8 dt 1; set border 15 lc rgb "#333333"; set xtics nomirror; set ytics nomirror
c100="#0d366b"; cn="#eb6834"; cy="#a83a10"
unset key
# key inside the plot, top right, placed by hand (gnuplot's key underestimates this font's text width):
# sample + marker + text, rows KS cm apart, text measured with gs -sDEVICE=bbox (widest: Host@CXL 1.148 cm)
S = 0.28; G = 0.08; KS = 0.22; KW = 1.148
kx(cm) = (cm / P); ky(cm) = 1 - (cm / PH)          # cm from the plot's left / top edge -> graph
K0 = P - 0.08 - (S + G + KW)                       # sample start, text ends 0.08 cm inside the border
Y1 = ky(0.16); Y2 = ky(0.16 + KS); Y3 = ky(0.16 + 2*KS)
set arrow 11 from graph kx(K0), graph Y1 to graph kx(K0+S), graph Y1 nohead lc rgb c100 lw 2 front
set label 21 "" at graph kx(K0+S/2), graph Y1 point pt 13 ps .45 lc rgb c100 front
set label 31 "Host@CXL" at graph kx(K0+S+G), graph Y1 left font ",7" front
set arrow 12 from graph kx(K0), graph Y2 to graph kx(K0+S), graph Y2 nohead lc rgb cy lw 2 front
set label 22 "" at graph kx(K0+S/2), graph Y2 point pt 5 ps .45 lc rgb cy front
set label 32 "Hybrid" at graph kx(K0+S+G), graph Y2 left font ",7" front
set arrow 13 from graph kx(K0), graph Y3 to graph kx(K0+S), graph Y3 nohead lc rgb cn lw 2 dt 2 front
set label 23 "" at graph kx(K0+S/2), graph Y3 point pt 7 ps .5 lc rgb cn front
set label 33 "NDP@CXL" at graph kx(K0+S+G), graph Y3 left font ",7" front
set logscale xy 2
set xrange [10:130]
set yrange [700:9000]
set xtics ("12" 12.5, "25" 25, "50" 50, "100" 100) font ",8" offset 0,0.55
set ytics ("1k" 1000, "2k" 2000, "4k" 4000) font ",8" offset 0.5,0
set xlabel "parallelism used (%)" offset -1.0,1.2 font ",8"
set ylabel "Time (ms)" offset 0.5,0 font ",8"
set label 1 "a. threads" at graph 0.04, graph 0.12 left font ",8" tc rgb "#555555" front boxed
set style textbox opaque noborder margins 0.3,0.05
h = "data/threads100_host.dat"; d = "data/threads100_dev.dat"
plot h u ($1/1.28):2 w lp lc rgb c100 pt 13 ps .45 lw 2 notitle, \
     d u ($1/30.48):3 w lp lc rgb cn lw 2 dt 2 pt 7 ps .5 notitle, \
     d u ($1/30.48):2 w lp lc rgb cy lw 2 pt 5 ps .45 notitle
