#!/usr/bin/env bash
# Render every figure of this component: gnuplot fig_*.gp -> fig_*.eps (next to the scripts).
# Run figures/make_data.py first; a figure whose data is missing is skipped.
set -euo pipefail
cd "$(dirname "$0")"
for g in fig_*.gp; do
  missing=$(grep -o '"data/[^"]*"' "$g" | tr -d '"' | sort -u | while read -r f; do [ -f "$f" ] || echo "$f"; done | head -1)
  if [ -n "$missing" ]; then echo "  skip ${g%.gp}.eps: no $missing (run ./run.sh, then figures/make_data.py)"; continue; fi
  gnuplot "$g" && echo "  ${g%.gp}.eps"
done
