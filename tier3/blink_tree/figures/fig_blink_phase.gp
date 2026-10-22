# B-link tree, panel a: where each plan spends its timed share per 1M ops, YCSB A (10M keys,
# scrambled Zipf 0.99), horizontal stacked bars from data/phase.dat (median run; whisker = min-max).
# Each plan at its BEST configuration per workload (data/phase.dat last column).
# Host@CXL: batched (same pipeline and timing rule as NDP@CXL): index ops = parallel locate + writes, results = reporting.
# NDP@CXL = resident device worker, NDP-sync = one kernel launch per job (batch 1M, combining on), timed from
# staging to the end of each batch:
# index ops = execute->synchronize (dispatch + device kernels), invalidation = result-readback invalidation,
# staging/copy = request staging + result readback, other = host split fallback + reporting. Pre-execute prep is
# not counted. Hybrid: index ops = host locate + device write kernel, invalidation = post-locate leaf invalidation
# (SDBI) + result invalidation. (2026-10-18: split from the former "SDBI" segment; previous: *_before_split.gp) Layout as the hash join fig_hj_phase.gp (one ACM column, legend on top).
R = 0.14; T = 0.04; B = 0.62
LA = 1.40; LB = 0.85
L = LA
LGH = 0.56                           # two-row phase legend above this panel (fig_blink_legend.gp)
PH = 1.76 - LGH                      # legend + panel a = panel b (fig_blink_scale.gp PH; hash join height)
P = (8.46 - 4/72.0*2.54) / 2 - L - R
W = L + P + R; H = T + PH + B
set terminal epscairo noenhanced color font "Linux Libertine O,8" fontscale 0.75 size W*1.0 cm, H*1.0 cm
set output "fig_blink_phase.eps"
set tmargin at screen 1 - T/H; set bmargin at screen B/H; set lmargin at screen L/W; set rmargin at screen 1 - R/W
set grid xtics lc rgb "#cfcfcf" lw 0.8 dt 1; set border 15 lc rgb "#333333"; set xtics nomirror; set ytics nomirror scale 0
cI="#0d366b"; cH="#eb6834"; cC="#a83a10"; cP="#2a78d6"; ink="#333333"
unset key
load "data/phase_range.gp"                         # XMAX, XSTEP from make_data.py (fits the data)
set xrange [0:XMAX]; set yrange [-0.6:3.6]
set xtics 0,XSTEP,XMAX-XSTEP font ",8" offset 0,0.55
set mxtics 2
set ytics ("Host@CXL" 3, "Hybrid" 2, "NDP-sync" 1, "NDP@CXL" 0) font ",8" offset 0.5,0
set xlabel "Time (ms)" offset 0,1.2 font ",8"   # per 1M ops (caption)
# on the NDP@CXL row, right-aligned: clear of the x axis and of that row's short bar and total
set label 1 "a. phases" at graph 0.98, first 0.5 right font ",8" tc rgb "#555555" front boxed
set style textbox opaque noborder margins 0.3,0.05
set style fill solid 1.0 noborder
f = "data/phase.dat"; h = 0.30
num(v) = (v >= XMAX*0.12 ? sprintf("%.0f", v) : "")   # white numbers only where the segment fits them
# columns: 1 plan, 2 wl (A|B), 3 y, 4 index ops, 5 hand-off, 6 results, 7 total, 8 min, 9 max, 10 n, 11 best config
# columns: 4 index ops, 5 invalidation, 6 staging/copy, 7 other, 8 total, 9 min, 10 max
plot f u (0):3:(0):4:($3-h):($3+h) w boxxyerror lc rgb cI, \
     f u (0):3:4:($4+$5):($3-h):($3+h) w boxxyerror lc rgb cH, \
     f u (0):3:($4+$5):($4+$5+$6):($3-h):($3+h) w boxxyerror lc rgb cC, \
     f u (0):3:($4+$5+$6):($4+$5+$6+$7):($3-h):($3+h) w boxxyerror lc rgb cP, \
     f u ($4/2):3:(num($4)) w labels center font ",6" tc rgb "#ffffff", \
     f u ($4+$5/2):3:(num($5)) w labels center font ",6" tc rgb "#ffffff", \
     f u ($4+$5+$6+$7/2):3:(num($7)) w labels center font ",6" tc rgb "#ffffff", \
     f u 9:3:($10-$9):(0) w vectors nohead lc rgb ink lw 0.8, \
     f u 9:($3-0.12):(0):(0.24) w vectors nohead lc rgb ink lw 0.8, \
     f u 10:($3-0.12):(0):(0.24) w vectors nohead lc rgb ink lw 0.8, \
     f u ($10+XMAX*0.035):3:(sprintf("%.0f", $8)) w labels left font ",7" tc rgb ink
