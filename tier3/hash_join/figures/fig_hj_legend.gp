# Legend for panel a of fig_hj_row: the four phases in TWO rows (2 x 2), on a canvas as wide as panel a
# and LGH tall; make_row.py stacks it on top of fig_hj_phase.eps. Panel b's key is inside its plot
# (fig_hj_scale.gp). Entries placed by hand as in btree_lookup fig_size_legend.gp (text widths from
# the rendered ink, gs -sDEVICE=bbox), right-aligned to panel a's right plot border.
R = 0.05; LA = 1.40; LB = 0.85          # R must match fig_hj_phase100.gp
P = (8.46 - 4/72.0*2.54) / 2 - LA - R   # panel a takes half the column (fig_hj_phase.gp)
W = LA + P + R; LGH = 0.56                     # must match fig_hj_phase.gp
set terminal epscairo noenhanced color font "Linux Libertine O,7" fontscale 0.75 size W*1.0 cm, LGH*1.0 cm
set output "fig_hj_legend.eps"
# B+tree palette: phases in blue (build c100 navy, probe c50), host<->device hand-offs in the NDP
# oranges (cn, NDP-opt) -- same colours as the plan lines of panel b
cB="#0d366b"; cBg="#eb6834"; cP="#2a78d6"; cPg="#a83a10"
unset border; unset xtics; unset ytics; unset grid; unset key
set lmargin 0; set rmargin 0; set tmargin 0; set bmargin 0
set xrange [0:W]; set yrange [0:LGH]           # both in cm
S = 0.28; G = 0.08; E = 0.20                   # swatch length, swatch -> text, gap between columns
BH = 0.09                                      # half height of a swatch
Y1 = 0.39; Y2 = 0.14                           # row centres
wa = 0.568; wb = 1.513                         # widest text of column 1 (probe), column 2 (probe hand-off)
x1 = W - 0.02 - (2*(S+G) + wa + wb + E)        # text ends 0.02 cm before the canvas edge; leaves room for "a. phases"
x2 = x1 + S+G+wa+E
set object 1 rect from x1, Y1-BH to x1+S, Y1+BH fc rgb cB fs solid 1.0 noborder
set label 21 "build" at x1+S+G, Y1 left
set object 2 rect from x2, Y1-BH to x2+S, Y1+BH fc rgb cBg fs solid 1.0 noborder
set label 22 "build hand-off" at x2+S+G, Y1 left
set object 3 rect from x1, Y2-BH to x1+S, Y2+BH fc rgb cP fs solid 1.0 noborder
set label 23 "probe" at x1+S+G, Y2 left
set object 4 rect from x2, Y2-BH to x2+S, Y2+BH fc rgb cPg fs solid 1.0 noborder
set label 24 "probe hand-off" at x2+S+G, Y2 left
# panel a's label in the empty top-left corner (left of the phase legend, above the plan names);
# fig_hj_phase100.gp has no in-plot label
set label 1 "a. phases" at 0.02, (Y1 + Y2) / 2 left font ",8" tc rgb "#555555"
plot NaN notitle
