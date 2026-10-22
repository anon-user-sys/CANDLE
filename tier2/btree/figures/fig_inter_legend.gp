# Legend for the one-column placement figure (fig_btree_inter_row): two rows of three entries,
# 8 pt, columns aligned; canvas as wide as one ACM column (8.46 cm). Entries placed by hand from
# the rendered text widths (gs -sDEVICE=bbox), as in fig_combo_legend.gp.
set terminal epscairo noenhanced color font "Linux Libertine O,8" fontscale 0.75 size 8.46cm,0.78cm
set output "fig_inter_legend.eps"
c0="#86b6ef"; c25="#5598e7"; c50="#2a78d6"; c75="#1c5cab"; c100="#0d366b"; cn="#eb6834"
unset border; unset xtics; unset ytics; unset grid; unset key
set lmargin 0; set rmargin 0; set tmargin 0; set bmargin 0
set xrange [0:8.46]; set yrange [0:1]
S = 0.42; G = 0.14; E = 0.45
array t[6] = ["Host@CXL", "Tiered 75%", "Tiered 50%", "Tiered 25%", "Host-DRAM", "NDP"]
array c[6] = [c100, c75, c50, c25, c0, cn]
array p[6] = [13, 11, 9, 5, 7, 7]
array d[6] = [1, 1, 1, 1, 1, 2]
array cw[3] = [1.312, 1.434, 1.247]        # column text width = widest entry in the column (cm)
tot = 3*(S+G) + cw[1] + cw[2] + cw[3] + 2*E
x0 = (8.46 - tot) / 2
do for [i=1:6] {
    col = (i-1) % 3 + 1; y = (i <= 3) ? 0.74 : 0.26
    x = x0; do for [k=1:col-1] { x = x + S + G + cw[k] + E }
    set arrow i from x, y to x+S, y nohead lc rgb c[i] lw 2 dt d[i]
    set label 10+i "" at x+S/2, y point pt p[i] ps .5 lc rgb c[i]
    set label 20+i t[i] at x+S+G, y left
}
plot NaN notitle
