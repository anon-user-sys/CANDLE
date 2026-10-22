#!/usr/bin/env python3
"""fig_hj_row.eps: panel a (phases) with its 2-row legend on top, beside panel b (scaling, key inside), in ONE vector EPS
(standard EPSF inclusion: each file is wrapped in save/restore and translated). GAP pt between
panels; the legend canvas is as wide as the panel row. No TeX needed. Run after the panels are
rendered (watch.sh does it). Also redraws preview/fig_hj_row.png from the EPS, replacing the
side-by-side PNG watch.sh builds."""
import os, re, subprocess, sys
D = os.path.dirname(os.path.abspath(__file__))
# usage: make_row.py [OUT PANEL_A PANEL_B [LEGEND]]  (default: the phase/scaling row with its legend)
OUT, PANELS, LEGEND = "fig_hj_row", ["fig_hj_phase", "fig_hj_scale"], "fig_hj_legend"
if len(sys.argv) > 1:
    OUT, PANELS, LEGEND = sys.argv[1], sys.argv[2:4], (sys.argv[4] if len(sys.argv) > 4 else None)
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
legend = load(LEGEND) if LEGEND else None
# left column: the phase legend stacked on panel a (fig_hj_phase.gp shortens panel a by the legend's
# height); right column: panel b (its key is inside the plot). Columns bottom-aligned.
ba, bb = panels[0][2], panels[1][2]
AW, AH = ba[2] - ba[0], ba[3] - ba[1]
LW, LH = (legend[2][2] - legend[2][0], legend[2][3] - legend[2][1]) if legend else (0.0, 0.0)
BW, BH = bb[2] - bb[0], bb[3] - bb[1]
H = max(LH + AH, BH)
# gnuplot's EPS bounding boxes are whole points; stretch the gap so the row is one ACM column wide
COLW = 8.46 / 2.54 * 72
GAP = max(GAP, COLW - max(AW, LW) - BW)
W = max(AW, LW) + GAP + BW
out = [f"%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 {int(-(-W // 1))} {int(-(-H // 1))}\n"
       f"%%HiResBoundingBox: 0 0 {W:.3f} {H:.3f}\n%%Title: {OUT}\n%%Creator: make_row.py\n%%EndComments\n"]
# columns BOTTOM-aligned: both panels have the same bottom margin, so their x axes line up (the
# whole-point bounding boxes leave the left column ~1 pt shorter; the slack goes above the legend)
out.append(place(*panels[0], 0.0, 0.0))                    # panel a at the bottom
if legend: out.append(place(*legend, 0.0, AH))            # legend on top of it
out.append(place(*panels[1], max(AW, LW) + GAP, 0.0))      # panel b, bottom-aligned
out.append("showpage\n%%EOF\n")
open(os.path.join(D, OUT + ".eps"), "w", encoding="latin-1").write("".join(out))
print(f"{OUT}.eps: {W:.1f} x {H:.1f} pt ({W/72*2.54:.2f} x {H/72*2.54:.2f} cm)")
# PDF twin for pdflatex/Overleaf (same content, cropped to the bounding box)
subprocess.run(["ps2pdf", "-dEPSCrop", os.path.join(D, OUT + ".eps"), os.path.join(D, OUT + ".pdf")], check=True)
# preview PNG of the real layout (legend above the panels)
os.makedirs(os.path.join(D, "preview"), exist_ok=True)
subprocess.run(["gs", "-q", "-dSAFER", "-dBATCH", "-dNOPAUSE", "-dEPSCrop", "-sDEVICE=png16m", "-r200",
                "-dTextAlphaBits=4", "-dGraphicsAlphaBits=4",
                "-sOutputFile=" + os.path.join(D, "preview", OUT + ".png"),
                os.path.join(D, OUT + ".eps")], check=True)
