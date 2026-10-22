# Legend for panel a of fig_blink_row: the three phases (index ops, SDBI, results) in ONE row, on a canvas as wide as panel a
# and LGH tall; make_row.py stacks it on top of fig_blink_phase.eps. Placed by hand as in the hash
# join fig_hj_legend.gp, right-aligned to panel a's right plot border.
R = 0.14; LA = 1.40; LB = 0.85
P = (8.46 - 4/72.0*2.54) / 2 - LA - R   # panel a takes half the column (fig_blink_phase.gp)
W = LA + P + R; LGH = 0.56                     # must match fig_blink_phase.gp (two rows)
set terminal epscairo noenhanced color font "Linux Libertine O,7" fontscale 0.75 size W*1.0 cm, LGH*1.0 cm
set output "fig_blink_legend.eps"
# 2026-10-18: four segments in two rows (2 x 2); the former "SDBI" entry split into invalidation + staging/copy
cI="#0d366b"; cH="#eb6834"; cC="#a83a10"; cP="#2a78d6"
unset border; unset xtics; unset ytics; unset grid; unset key
set lmargin 0; set rmargin 0; set tmargin 0; set bmargin 0
set xrange [0:W]; set yrange [0:LGH]           # both in cm
S = 0.28; G = 0.08; E = 0.20                   # swatch length, swatch -> text, gap between columns
BH = 0.09                                      # half height of a swatch
Y1 = 0.39; Y2 = 0.14                           # row centres
wa = 1.30; wb = 1.12                           # widest text of column 1 (staging/copy), column 2 (invalidation)
x1 = LA + P - (2*(S+G) + wa + wb + E)          # right-aligned: text ends at panel a's right plot border
x2 = x1 + S+G+wa+E
set object 1 rect from x1, Y1-BH to x1+S, Y1+BH fc rgb cI fs solid 1.0 noborder
set label 21 "index ops" at x1+S+G, Y1 left
set object 2 rect from x2, Y1-BH to x2+S, Y1+BH fc rgb cH fs solid 1.0 noborder
set label 22 "invalidation" at x2+S+G, Y1 left
# reading order = bar order: index ops, invalidation / staging/copy, other
set object 3 rect from x1, Y2-BH to x1+S, Y2+BH fc rgb cC fs solid 1.0 noborder
set label 23 "staging/copy" at x1+S+G, Y2 left
set object 4 rect from x2, Y2-BH to x2+S, Y2+BH fc rgb cP fs solid 1.0 noborder
set label 24 "other" at x2+S+G, Y2 left
plot NaN notitle
