# Hash join, panel a (top row of fig_hj_row): sf_100, 50%, time per plan split into build, probe and the
# host<->device hand-offs. Owner-computes build in every plan, each plan at its best partition count
# (data/phase100.dat from make_data_sf100.py). Provisional bars (column 8 = 1) are labelled with *.
# Previous sf_10 version: _sf10_backup/ and fig_hj_phase.gp.
R = 0.05; T = 0.04; B = 0.62          # left column: thin right margin, plot stretches toward panel b/d (was 0.14)
LA = 1.40; LB = 0.85   # left margins of this panel (a, plan names) and the scaling panel (b)
L = LA
LGH = 0.56                           # height of the phase legend above this panel (fig_hj_legend.gp)
PH = 1.76 - LGH                      # shorter than panel b so legend + panel a = panel b
P = (8.46 - 4/72.0*2.54) / 2 - L - R   # each panel (labels included) takes half the column
W = L + P + R; H = T + PH + B
set terminal epscairo noenhanced color font "Linux Libertine O,8" fontscale 0.75 size W*1.0 cm, H*1.0 cm
set output "fig_hj_phase100.eps"
set tmargin at screen 1 - T/H; set bmargin at screen B/H; set lmargin at screen L/W; set rmargin at screen 1 - R/W
set grid xtics lc rgb "#cfcfcf" lw 0.8 dt 1; set border 15 lc rgb "#333333"; set xtics nomirror; set ytics nomirror scale 0
# B+tree palette: phases in blue (build c100 navy, probe c50), host<->device hand-offs in the NDP
# oranges (cn, NDP-opt) -- same colours as the plan lines of panel b
cB="#0d366b"; cBg="#eb6834"; cP="#2a78d6"; cPg="#a83a10"; ink="#333333"
unset key
set xrange [0:1500]; set yrange [-0.6:2.6]
set xtics 0,500,1000 font ",8" offset 0,0.55
set mxtics 2
set ytics ("Host@CXL" 2, "Hybrid" 1, "NDP@CXL" 0) font ",8" offset 0.5,0
set xlabel "Time (ms)" offset 0,1.2 font ",8"
# panel label "a. phases": in the legend strip above (fig_hj_legend.gp)
set style textbox opaque noborder margins 0.3,0.05
set style fill solid 1.0 noborder
f = "data/phase100.dat"; h = 0.30
# data rows: 0 = Host@CXL, 1 = Hybrid, 2 = NDP@CXL
y(i) = (i == 0 ? 2 : i == 1 ? 1 : i == 2 ? 0 : NaN)
# white numbers: build and probe time (ms) centred in their segments
# TEMPORARY: rows 0-1 are composites (host owner build of 12:23 + probes of the atomic runs) until the
# C31/C32 rerun -- marked with *. Owner-computes build only (user, 2026-10-18).
plot f u (0):(y($0)):(0):($2):(y($0)-h):(y($0)+h) w boxxyerror lc rgb cB, \
     f u (0):(y($0)):($2):($2+$3):(y($0)-h):(y($0)+h) w boxxyerror lc rgb cBg, \
     f u (0):(y($0)):($2+$3):($2+$3+$4):(y($0)-h):(y($0)+h) w boxxyerror lc rgb cP, \
     f u (0):(y($0)):($2+$3+$4):($2+$3+$4+$5):(y($0)-h):(y($0)+h) w boxxyerror lc rgb cPg, \
     f u ($2/2):(y($0)):(sprintf("%.0f", $2)) w labels center font ",6" tc rgb "#ffffff", \
     f u ($2+$3+$4/2):(y($0)):(sprintf("%.0f", $4)) w labels center font ",6" tc rgb "#ffffff", \
     f u ($6+40):(y($0)):(sprintf(int($8) ? "%.0f*" : "%.0f", $6)) w labels left font ",7" tc rgb ink
