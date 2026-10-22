# Panel (b) of the translation tax characterisation, as a standalone subfigure.
# Granule identification: cost vs span at four region spacings. Configurations
# sharing a span but differing in spacing separate here, which is what fixes
# the granule at 1 GB. Split from fig_translation3.gp; data unchanged.
set terminal epscairo enhanced color font "Linux Libertine O,10" size 3cm,2.5cm
set output "fig_translation_b.eps"

# ── plot area flush with canvas on top and right ──────────────────────
set tmargin at screen 0.99
set bmargin at screen 0.23
set rmargin at screen 0.99
set lmargin at screen 0.21

set key top left Left reverse samplen 1 spacing 0.8 font ",12" 
set grid ytics lc rgb "#dddddd"
set logscale x 2
set ylabel "ns / access" font ",12" offset 1.9,-0.5
set xlabel "Span (GB)" font ",12" offset -0,1.1
set xrange [0.8:160]
set yrange [0:1100]
set xtics (1,8,64) font ",12" offset 0,0.5
set ytics (0,200,400,600,800) font ",12" offset 0.8,0

plot \
  "data/dev_S16M.dat"   using 1:2 with linespoints lc rgb "#8e44ad" pt 11 ps 0.3 lw 1.3 title "  S=16MB", \
  "data/dev_S256M.dat"  using 1:2 with linespoints lc rgb "#c0392b" pt 5  ps 0.3 lw 1.3 title " 256MB", \
  "data/dev_S1024M.dat" using 1:2 with linespoints lc rgb "#2471a3" pt 7  ps 0.3 lw 1.3 title " 1GB", \
  "data/dev_S2048M.dat" using 1:2 with linespoints lc rgb "#1e8449" pt 9  ps 0.3 lw 1.3 title " 2GB"
