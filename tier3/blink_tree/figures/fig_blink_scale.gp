# B-link tree, panel b: time per 1M ops against the share of the platform's threads used, YCSB A, same axis as the hash join fig_hj_scale.gp: host threads / 128,
# NDP tasks / 3,048. y = time (log). Blue = Host@CXL (batched, same rule as NDP), orange dashed = NDP@CXL
# (batch 1M, combining on, timed from execute() to batch end); dark orange = Hybrid (128 host threads,
# batch 1M, x = device tasks / 3,048).
# Whiskers = min-max over runs.
R = 0.14; T = 0.04; B = 0.62
LA = 1.40; LB = 0.85
L = LB
PH = 1.76                              # same height as the hash join fig_hj_scale.gp
P = (8.46 - 4/72.0*2.54) / 2 - L - R   # each panel (labels included) takes half the column
W = L + P + R; H = T + PH + B
set terminal epscairo noenhanced color font "Linux Libertine O,8" fontscale 0.75 size W*1.0 cm, H*1.0 cm
set output "fig_blink_scale.eps"
set tmargin at screen 1 - T/H; set bmargin at screen B/H; set lmargin at screen L/W; set rmargin at screen 1 - R/W
set grid xtics ytics lc rgb "#cfcfcf" lw 0.8 dt 1; set border 15 lc rgb "#333333"; set xtics nomirror; set ytics nomirror
c100="#0d366b"; cn="#eb6834"; cy="#a83a10"
unset key
# key inside the plot, top right, placed by hand like fig_hj_scale.gp
S = 0.28; G = 0.08; KS = 0.22; KW = 1.148
kx(cm) = (cm / P); ky(cm) = 1 - (cm / PH)
K0 = P - 0.08 - (S + G + KW)
Y1 = ky(0.16); Y2 = ky(0.16 + KS); Y3 = ky(0.16 + 2*KS); Y4 = ky(0.16 + 3*KS)
set arrow 11 from graph kx(K0), graph Y1 to graph kx(K0+S), graph Y1 nohead lc rgb c100 lw 2 front
set label 21 "" at graph kx(K0+S/2), graph Y1 point pt 13 ps .45 lc rgb c100 front
set label 31 "Host@CXL" at graph kx(K0+S+G), graph Y1 left font ",7" front
set arrow 12 from graph kx(K0), graph Y2 to graph kx(K0+S), graph Y2 nohead lc rgb cy lw 2 front
set label 22 "" at graph kx(K0+S/2), graph Y2 point pt 5 ps .45 lc rgb cy front
set label 32 "Hybrid" at graph kx(K0+S+G), graph Y2 left font ",7" front
set arrow 13 from graph kx(K0), graph Y3 to graph kx(K0+S), graph Y3 nohead lc rgb cn lw 1 dt 3 front
set label 23 "" at graph kx(K0+S/2), graph Y3 point pt 6 ps .5 lc rgb cn front
set label 33 "NDP-sync" at graph kx(K0+S+G), graph Y3 left font ",7" front
set arrow 14 from graph kx(K0), graph Y4 to graph kx(K0+S), graph Y4 nohead lc rgb cn lw 2 dt 2 front
set label 24 "" at graph kx(K0+S/2), graph Y4 point pt 7 ps .5 lc rgb cn front
set label 34 "NDP@CXL" at graph kx(K0+S+G), graph Y4 left font ",7" front
set logscale xy 2
set xrange [0.45:130]   # NDP@CXL from 16 tasks (0.52%), Host@CXL from 1 thread (0.78%)
set yrange [16:4096]   # headroom above Host@CXL's 1-thread point for the top-left label
set xtics ("0.8" 0.78125, "3" 3.125, "12" 12.5, "50" 50, "100" 100) font ",8" offset 0,0.55
set ytics (16, 64, 256, 1024) font ",8" offset 0.5,0
set xlabel "parallelism used (%)" offset -1.0,1.2 font ",8"
set ylabel "Time (ms)" offset 2.0,0 font ",8"   # per 1M ops (caption)
set label 1 "b. scaling" at graph 0.04, graph 0.90 left font ",8" tc rgb "#555555" front boxed   # top left, as in fig_hj_scale.gp
set style textbox opaque noborder margins 0.3,0.05
# columns: 1 threads|tasks, 2 median, 3 min, 4 max
plot "data/scale_host_a.dat" u ($1/1.28):2 w lp lc rgb c100 pt 13 ps .45 lw 2 notitle, \
     "data/scale_host_a.dat" u ($1/1.28):3:(0):($4-$3) w vectors nohead lc rgb c100 lw 0.8 notitle, \
     "data/scale_hyb_a.dat" u ($1/30.48):2 w lp lc rgb cy pt 5 ps .45 lw 2 notitle, \
     "data/scale_hyb_a.dat" u ($1/30.48):3:(0):($4-$3) w vectors nohead lc rgb cy lw 0.8 notitle, \
     "data/scale_dev_a.dat" u ($1/30.48):2 w lp lc rgb cn lw 1 dt 3 pt 6 ps .5 notitle, \
     "data/scale_dev_a.dat" u ($1/30.48):3:(0):($4-$3) w vectors nohead lc rgb cn lw 0.8 notitle, \
     "data/scale_res_a.dat" u ($1/30.48):2 w lp lc rgb cn lw 2 dt 2 pt 7 ps .5 notitle, \
     "data/scale_res_a.dat" u ($1/30.48):3:(0):($4-$3) w vectors nohead lc rgb cn lw 0.8 notitle
