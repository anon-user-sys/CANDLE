#!/usr/bin/env python3
"""results -> figures/data for Fig. 9 (placements x cores) and Fig. 10 (node size, tree size)."""
import os, runpy
D = os.path.dirname(os.path.abspath(__file__))
os.makedirs(os.path.join(D, "data"), exist_ok=True)
for s in ("make_data_opt.py", "make_data_node.py", "make_data_size.py"):
    runpy.run_path(os.path.join(D, s), run_name="__main__")
