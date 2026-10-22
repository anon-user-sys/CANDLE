#!/usr/bin/env python3
"""fig_btree_inter_row.eps: the legend on top, the four fig_bto panels side by side below it, in ONE vector EPS
(standard EPSF inclusion: each file is wrapped in save/restore and translated). GAP pt between
panels; the legend canvas is as wide as the panel row. No TeX needed. Run after the panels are
rendered (watch.sh does it). Also redraws preview/fig_btree_inter_row.png from the EPS, replacing the
side-by-side PNG watch.sh builds."""
import os, re, subprocess
D = os.path.dirname(os.path.abspath(__file__))
PANELS = ["fig_inter_a", "fig_inter_b"]
LEGEND = "fig_inter_legend"
GAP = 4.0                                                  # pt between panels (nominal; see below)

def bbox(txt):
    m = re.search(r"^%%HiResBoundingBox:\s*([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)", txt, re.M) or \
        re.search(r"^%%BoundingBox:\s*([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)", txt, re.M)
    return tuple(map(float, m.groups()))

def load(name):
    txt = open(os.path.join(D, name + ".eps"), encoding="latin-1").read()
    return name, txt, bbox(txt)

def place(name, txt, box, x, y):
    """EPS `name` with its lower-left corner at (x, y) of the row."""
    x0, y0 = box[0], box[1]
    return f"""/b4_inc_state save def
/dict_count countdictstack def /op_count count 1 sub def
userdict begin /showpage {{}} def
0 setgray 0 setlinecap 1 setlinewidth 0 setlinejoin 10 setmiterlimit [] 0 setdash newpath
{x - x0:.3f} {y - y0:.3f} translate
%%BeginDocument: {name}.eps
{txt}
%%EndDocument
count op_count sub {{pop}} repeat countdictstack dict_count sub {{end}} repeat
b4_inc_state restore
"""

panels = [load(p) for p in PANELS]
legend = load(LEGEND)
PH = max(b[3] - b[1] for _, _, b in panels)                # panel row height
LW, LH = legend[2][2] - legend[2][0], legend[2][3] - legend[2][1]
# gnuplot's EPS bounding boxes are whole points, so the panels come out a little narrower than
# fig_bto.gp sized them; stretch the gaps so the row is exactly as wide as the legend (= \textwidth)
PW = sum(b[2] - b[0] for _, _, b in panels)
GAP = max(GAP, (LW - PW) / (len(panels) - 1))
W = PW + GAP * (len(panels) - 1)
H = LH + PH
out = [f"%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 {int(-(-W // 1))} {int(-(-H // 1))}\n"
       f"%%HiResBoundingBox: 0 0 {W:.3f} {H:.3f}\n%%Title: fig_btree_inter_row\n%%Creator: make_row.py\n%%EndComments\n"]
out.append(place(*legend, (W - LW) / 2, PH))               # legend on top, centred over the row
x = 0.0
for name, txt, b in panels:                                # panels top-aligned under the legend
    out.append(place(name, txt, b, x, PH - (b[3] - b[1])))
    x += (b[2] - b[0]) + GAP
out.append("showpage\n%%EOF\n")
open(os.path.join(D, "fig_btree_inter_row.eps"), "w", encoding="latin-1").write("".join(out))
print(f"fig_btree_inter_row.eps: {W:.1f} x {H:.1f} pt ({W/72*2.54:.2f} x {H/72*2.54:.2f} cm)")
# PDF twin for pdflatex/Overleaf (same content, cropped to the bounding box)
subprocess.run(["ps2pdf", "-dEPSCrop", os.path.join(D, "fig_btree_inter_row.eps"), os.path.join(D, "fig_btree_inter_row.pdf")], check=True)
# preview PNG of the real layout (legend above the panels)
os.makedirs(os.path.join(D, "preview"), exist_ok=True)
subprocess.run(["gs", "-q", "-dSAFER", "-dBATCH", "-dNOPAUSE", "-dEPSCrop", "-sDEVICE=png16m", "-r200",
                "-dTextAlphaBits=4", "-dGraphicsAlphaBits=4",
                "-sOutputFile=" + os.path.join(D, "preview", "fig_btree_inter_row.png"),
                os.path.join(D, "fig_btree_inter_row.eps")], check=True)
