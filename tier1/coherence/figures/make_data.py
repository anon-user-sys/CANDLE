#!/usr/bin/env python3
"""Build figure data for Sec. 4.4 (coherency) from the full suite run of 2026-10-10.

results/suite_20261010_135135: k1 (flush vs size), k2 (dirty fraction), k5 (per-object),
k10 (publish race). Values are medians as printed by the suite (k1/k2/k5 already median over
repetitions); k10 has one row per run, so it is the median with min/max across the 3 runs.

Usage: python3 make_data.py   (run from figures/)
"""
import collections
import statistics as st
from pathlib import Path

RUN = Path(__file__).resolve().parent.parent / "results" / "suite_20261010_135135"
OUT = Path(__file__).resolve().parent / "data"
OUT.mkdir(exist_ok=True)


def rows(test):
    return [l.strip().split(",") for l in open(RUN / f"{test}.csv") if l.startswith(test + ",")]


def k1():   # bytes, then p50 us for dirty / clean / absent (lib_clflushopt)
    d = {(int(r[1]), r[2]): float(r[5]) / 1e3 for r in rows("k1") if r[3] == "lib_clflushopt"}
    with open(OUT / "coh_flush.dat", "w") as f:
        f.write("# bytes dirty_us clean_us absent_us   (one host flush, lib_clflushopt, median)\n")
        for b in sorted({k[0] for k in d}):
            f.write(f"{b} {d[(b, 'dirty')]:.4f} {d[(b, 'clean')]:.4f} {d[(b, 'absent')]:.4f}\n")


def k2():   # dirty %, then ms for uniform / clustered with the rest absent
    d = {(float(r[2]), r[3], r[4]): float(r[7]) / 1e6 for r in rows("k2")}
    with open(OUT / "coh_dirty.dat", "w") as f:
        f.write("# dirty_pct uniform_ms clustered_ms   (16 MiB range, other lines absent, median)\n")
        for p in sorted({k[0] for k in d}):
            f.write(f"{p} {d[(p, 'uniform', 'absent')]:.4f} {d[(p, 'clustered', 'absent')]:.4f}\n")


def k5():   # object bytes, then ns/lookup for hot_plain hot_sdbi cold_plain cold_sdbi (1 thread)
    d = {(int(r[1]), r[3]): float(r[6]) for r in rows("k5") if r[2] == "1"}
    arms = ["hot_plain", "hot_sdbi", "cold_plain", "cold_sdbi"]
    with open(OUT / "coh_object.dat", "w") as f:
        f.write("# obj_bytes " + " ".join(a + "_ns" for a in arms) + "   (1 thread, median)\n")
        for o in sorted({k[0] for k in d}):
            f.write(f"{o} " + " ".join(f"{d[(o, a)]:.1f}" for a in arms) + "\n")


def k10():  # protocol label, median / min / max early-bad lines (thousands) over 3 runs
    d = collections.defaultdict(list)
    for r in rows("k10"):
        d[(r[1], r[2])].append(int(r[8]) / 1e3)
    order = [(("flush_fence", "chase"), "flush\\n+fence"), (("flush", "chase"), "flush"),
             (("noflush", "chase"), "none"), (("flushall_fence", "chase"), "all\\n+fence"),
             (("flushall", "chase"), "all"), (("flush_fence", "after_sync"), "after\\nend")]
    with open(OUT / "coh_publish.dat", "w") as f:
        f.write("# idx label median_k min_k max_k   (lines read before written back, of 12.5 M)\n")
        for i, (k, lab) in enumerate(order):
            v = d[k]
            f.write(f'{i} "{lab}" {st.median(v):.2f} {min(v):.2f} {max(v):.2f}\n')


if __name__ == "__main__":
    OUT.mkdir(exist_ok=True)
    k1(); k2(); k5(); k10()
    print("wrote", *sorted(p.name for p in OUT.glob("*.dat")))
