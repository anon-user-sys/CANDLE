# Shared legend for the four fig_o3 panels: one line, no box, ABOVE the row; canvas as wide as the
# row (ACM \textwidth, 7 in = 17.78 cm), 0.4 cm tall.
set terminal epscairo noenhanced color font "Linux Libertine O,8" fontscale 0.75 size 17.78cm,0.4cm
set output "fig_o3_legend.eps"
c0="#86b6ef"; c25="#5598e7"; c50="#2a78d6"; c75="#1c5cab"; c100="#0d366b"; cn="#eb6834"
unset border; unset xtics; unset ytics; unset grid
set lmargin 0; set rmargin 0; set tmargin 0; set bmargin 0
set xrange [0:1]; set yrange [0:1]
# centred by MEASURED ink (gnuplot underestimates this font's text width, so its own centring is off):
# ink is 322.8 pt wide on the 504 pt canvas and starts 4.2 pt right of the anchor -> anchor at 86.4 pt.
# Text is a true 8 pt (fontscale 0.75 undoes epscairo's default 2/3 scaling).
set key at screen 0.171, screen 0.5 left center horizontal maxrows 1 reverse Left nobox samplen 1.8 width 1 font ",8"
plot NaN w lp lc rgb c100 pt 13 ps .55 lw 2 t "Host@CXL", \
     NaN w lp lc rgb c75 pt 11 ps .55 lw 2 t "Tiered 75%", \
     NaN w lp lc rgb c50 pt 9 ps .55 lw 2 t "Tiered 50%", \
     NaN w lp lc rgb c25 pt 5 ps .5 lw 2 t "Tiered 25%", \
     NaN w lp lc rgb c0 pt 7 ps .5 lw 2 t "Host-DRAM", \
     NaN w lp lc rgb cn pt 7 ps .55 lw 2 dt 2 t "NDP"
