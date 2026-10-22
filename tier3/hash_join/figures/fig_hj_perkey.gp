# Hash join, panel b (top row of fig_hj_row): cost per input tuple against TPC-H scale factor -- owner
# build per orders key and probe per lineitem row, host vs device (data/perkey100.dat from make_data_sf100.py). Log-log, key inside top right.
R = 0.14; T = 0.04; B = 0.62
LA = 1.40; LB = 0.85
L = 0.66                             # tight: 3-char ticks + axis title (was LB = 0.85)
PH = 1.76
P = (8.46 - 4/72.0*2.54) / 2 - L - R
W = L + P + R; H = T + PH + B
set terminal epscairo noenhanced color font "Linux Libertine O,8" fontscale 0.75 size W*1.0 cm, H*1.0 cm
set output "fig_hj_perkey.eps"
set tmargin at screen 1 - T/H; set bmargin at screen B/H; set lmargin at screen L/W; set rmargin at screen 1 - R/W
set grid xtics ytics lc rgb "#cfcfcf" lw 0.8 dt 1; set border 15 lc rgb "#333333"; set xtics nomirror; set ytics nomirror
c100="#0d366b"; cn="#eb6834"; cy="#a83a10"; cs="#8c8c8c"
unset key
# colour = where it runs (host navy, device orange), line = phase (build solid/filled, probe dashed/open).
# hand-offs (Hybrid; grey, dotted): build hand-off per key (filled), probe hand-off per row (open).
# Key, top right: column 1 = colour (host/device), column 2 = line style (build/probe/hand-off, grey)
S = 0.28; G = 0.08; KS = 0.26; cg = "#777777"   # row spacing 0.26 (0.22 let probe/hand-off touch)
kx(cm) = (cm / P); ky(cm) = 1 - (cm / PH)
C2 = P - 0.08 - (S + G + 0.86)            # column 2 (build / probe), text ends 0.08 cm inside the border
C1 = C2 - 0.10 - (S + G + 0.62)           # column 1 (host / device)
Y1 = ky(0.16); Y2 = ky(0.16 + KS); Y3 = ky(0.16 + 2*KS)
set arrow 11 from graph kx(C1), graph Y1 to graph kx(C1+S), graph Y1 nohead lc rgb c100 lw 2 front
set label 31 "host" at graph kx(C1+S+G), graph Y1 left font ",7" front
set arrow 12 from graph kx(C1), graph Y2 to graph kx(C1+S), graph Y2 nohead lc rgb cn lw 2 front
set label 32 "device" at graph kx(C1+S+G), graph Y2 left font ",7" front
set arrow 13 from graph kx(C2), graph Y1 to graph kx(C2+S), graph Y1 nohead lc rgb cg lw 2 front
set label 23 "" at graph kx(C2+S/2), graph Y1 point pt 7 ps .45 lc rgb cg front
set label 33 "build" at graph kx(C2+S+G), graph Y1 left font ",7" front
set arrow 14 from graph kx(C2), graph Y2 to graph kx(C2+S), graph Y2 nohead lc rgb cg lw 2 dt 2 front
set label 24 "" at graph kx(C2+S/2), graph Y2 point pt 6 ps .45 lc rgb cg front
set label 34 "probe" at graph kx(C2+S+G), graph Y2 left font ",7" front
set arrow 15 from graph kx(C2), graph Y3 to graph kx(C2+S), graph Y3 nohead lc rgb cg lw 2 dt 3 front
set label 35 "hand-off" at graph kx(C2+S+G), graph Y3 left font ",7" front
set logscale xy 10
set xrange [0.7:140]
set yrange [0.04:400]
set xtics ("1" 1, "10" 10, "100" 100) font ",8" offset 0,0.55
set ytics ("0.1" 0.1, "1" 1, "10" 10, "100" 100) font ",8" offset 0.5,0
set xlabel "TPC-H scale factor" offset 0,1.2 font ",8"
set ylabel "ns / tuple" offset 1.4,0 font ",8"
set label 1 "b. cost" at graph 0.02, graph 0.90 left font ",8" tc rgb "#555555" front boxed
set style textbox opaque noborder margins 0.3,0.05
f = "data/perkey100.dat"
set datafile missing "NaN"
# build: ns per orders key (col 3 host, 5 device); probe: ns per lineitem row (col 6 host, 7 device)
plot f u 1:5 w lp lc rgb cn lw 2 pt 7 ps .5 notitle, \
     f u 1:7 w lp lc rgb cn lw 2 dt 2 pt 6 ps .5 notitle, \
     f u 1:3 w lp lc rgb c100 lw 2 pt 13 ps .45 notitle, \
     f u 1:6 w lp lc rgb c100 lw 2 dt 2 pt 12 ps .5 notitle, \
     f u 1:8 w lp lc rgb cg lw 2 dt 3 pt 7 ps .45 notitle, \
     f u 1:9 w lp lc rgb cg lw 2 dt 3 pt 6 ps .45 notitle
