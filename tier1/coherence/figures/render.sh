#!/usr/bin/env bash
# Render every figure of this component: gnuplot fig_*.gp -> fig_*.eps (next to the scripts).
set -euo pipefail
cd "$(dirname "$0")"
for g in fig_*.gp; do gnuplot "$g" && echo "  ${g%.gp}.eps"; done
