# Figure 4.3(c): cost per op, direct access to device memory vs local DRAM, by pattern.
# Data: 2026-10-09 pinned config, D10 on a 256 MiB buffer, median of 3; make_data.py.
set terminal epscairo enhanced color font "Linux Libertine O,10" size 4cm,2cm
set output "fig_access_pattern.eps"
# identical margins in all four panels so the plot boxes align in the row

set style data histograms
set style histogram clustered gap 1
set style fill solid 1.0 border lc rgb "#ffffff"
set boxwidth 0.9
set yrange [0:95]; set ytics 0,20,80 font ",10" offset 0.5,0
set ylabel "ns per operation" font ",10" offset 1.5,0
xoff = 0.6     # vertical position of the x labels (both lines)
lsep = 0.95    # gap between label lines (1.0 = normal line)
set xtics font ",10" nomirror norotate center offset 0,xoff
set grid ytics lc rgb "#dddddd"

set tmargin at screen 0.99
set bmargin at screen 0.23
set rmargin at screen 0.99
set lmargin at screen 0.15

set key top left horizontal maxrows 1 Left reverse font ",10" samplen 0.8 spacing 0.9 nobox width 0 offset 0,0.15
#set object 1 rect from graph 0.03,0.79 to graph 0.66,0.97 front fc rgb "black" fs empty border lw 0.5
plot "data/access_pattern.dat" u 3:xtic(substr(strcol(2),1,strstrt(strcol(2),"\n")-1)) lc rgb "#1f5fa9" t "CXL", \
     "" u 4 lc rgb "#c84a4a" t "DRAM", \
     "" u ($0-0.17):($3+5):(sprintf("%.1f×",$5)) w labels font ",10" tc rgb "#333333" offset 0,0.1 notitle, \
     "" u 0:(0):(substr(strcol(2),strstrt(strcol(2),"\n")+1,99)) w labels font ",10" offset 0,xoff-0.8-lsep notitle
