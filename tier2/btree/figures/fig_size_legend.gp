# Legend for fig_size / fig_btree_ns_row: ONE row above the two panels, 7 pt, canvas as wide as one ACM
# column (8.46 cm). Entries are placed by hand (gnuplot's key gives every entry the same width and
# underestimates this font's text width, so a one-row key overlaps): sample line + marker + text,
# each entry starting where the previous one's text ends (widths from the rendered ink).
set terminal epscairo noenhanced color font "Linux Libertine O,7" fontscale 0.75 size 8.46cm,0.40cm
set output "fig_size_legend.eps"
c0="#86b6ef"; c50="#2a78d6"; c100="#0d366b"; cn="#eb6834"
unset border; unset xtics; unset ytics; unset grid; unset key
set lmargin 0; set rmargin 0; set tmargin 0; set bmargin 0
set xrange [0:8.46]; set yrange [0:1]          # x in cm
S = 0.28                                        # sample line length (cm)
G = 0.08                                        # gap: sample -> text
# text widths at 7 pt (cm, from the rendered ink) and the gap after each entry
w1 = 1.148; w2 = 1.092; w3 = 1.255; w4 = 0.471; w5 = 1.923; E = 0.14   # measured with gs -sDEVICE=bbox
x1 = (8.46 - (5*(S+G) + w1+w2+w3+w4+w5 + 4*E)) / 2
x2 = x1 + S+G+w1+E; x3 = x2 + S+G+w2+E; x4 = x3 + S+G+w3+E; x5 = x4 + S+G+w4+E
set arrow 1 from x1, 0.5 to x1+S, 0.5 nohead lc rgb c100 lw 2
set label 11 "" at x1+S/2, 0.5 point pt 13 ps .5 lc rgb c100
set label 21 "Host@CXL" at x1+S+G, 0.5 left
set arrow 2 from x2, 0.5 to x2+S, 0.5 nohead lc rgb c50 lw 2
set label 12 "" at x2+S/2, 0.5 point pt 9 ps .5 lc rgb c50
set label 22 "Tiered 50%" at x2+S+G, 0.5 left
set arrow 3 from x3, 0.5 to x3+S, 0.5 nohead lc rgb c0 lw 2
set label 13 "" at x3+S/2, 0.5 point pt 7 ps .45 lc rgb c0
set label 23 "Host-DRAM" at x3+S+G, 0.5 left
set arrow 4 from x4, 0.5 to x4+S, 0.5 nohead lc rgb cn lw 2 dt 2
set label 14 "" at x4+S/2, 0.5 point pt 7 ps .5 lc rgb cn
set label 24 "NDP" at x4+S+G, 0.5 left
set arrow 5 from x5, 0.5 to x5+S, 0.5 nohead lc rgb cn lw 1.5 dt 3
set label 15 "" at x5+S/2, 0.5 point pt 6 ps .5 lc rgb cn
set label 25 "NDP unpartitioned" at x5+S+G, 0.5 left
plot NaN notitle
