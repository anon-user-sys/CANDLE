#!/usr/bin/env bash
# CANDLE driver: the same verbs for every benchmark component.
#
#   ./candle.sh list
#   ./candle.sh build   <component|all>     host driver + MU kernel
#   ./candle.sh smoke   <component|all>     small validated runs, PASS/FAIL summary
#   ./candle.sh run     <component|all>     the paper's runs (long; see each README)
#   ./candle.sh figures <component|all>     results -> figures/data -> fig_*.eps
#
# <component> is a path such as tier1/coherence, or just its last part (coherence).
# Build as your user, then run smoke/run with sudo where a component needs root:
#   ./candle.sh build all && sudo ./candle.sh smoke all
set -uo pipefail
cd "$(dirname "$0")"
COMPONENTS=$(ls -d tier*/*/ 2>/dev/null | sed 's#/$##' | while read -r d; do [ -x "$d/build.sh" ] && echo "$d"; done)

usage() { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
resolve() {   # component name or path -> path
  [ "$1" = all ] && { echo "$COMPONENTS"; return; }
  for c in $COMPONENTS; do [ "$c" = "$1" ] || [ "${c##*/}" = "$1" ] && { echo "$c"; return; }; done
  echo "unknown component: $1 (try ./candle.sh list)" >&2; exit 2
}

verb=${1:-}; target=${2:-}
case "$verb" in
  list) for c in $COMPONENTS; do printf '%-26s %s\n' "$c" "$(sed -n '1s/^# //p' "$c/README.md")"; done ;;
  build|smoke|run|figures)
    [ -n "$target" ] || usage 2
    targets=$(resolve "$target") || exit 2
    pass=0; fail=0; failed=""
    for c in $targets; do
      echo "== $verb $c"
      case "$verb" in
        build)   (cd "$c" && ./build.sh > build.log 2>&1) && ok=1 || { ok=0; tail -5 "$c/build.log"; } ;;
        smoke)   (cd "$c" && ./smoke.sh) && ok=1 || ok=0 ;;
        run)     (cd "$c" && ./run.sh) && ok=1 || ok=0 ;;
        figures) (cd "$c" && { [ ! -f figures/make_data.py ] || python3 figures/make_data.py; } \
                   && { [ ! -x figures/render.sh ] || ./figures/render.sh; }) && ok=1 || ok=0 ;;
      esac
      if [ "$ok" = 1 ]; then pass=$((pass + 1)); else fail=$((fail + 1)); failed="$failed $c"; fi
    done
    echo; echo "$verb: $pass ok, $fail failed${failed:+ ->$failed}"
    [ "$fail" = 0 ] ;;
  ""|-h|--help) usage ;;
  *) usage 2 ;;
esac
