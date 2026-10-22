#!/bin/bash
# gen_cols.sh <SF> <chunks> <outdir> [cpu_node]
#
# Generates TPC-H lineitem at scale factor SF and keeps only the scan's two columns, as one
# column file for `main --bin`: header (TPCHSF02), all l_orderkey as uint64, all l_quantity
# as uint8 - the same layout as the scan's cache. dbgen runs in <chunks> parallel steps; each
# step writes into a FIFO drained by tpch_cols, so the text (~0.8 TB per sf_1000) never
# reaches disk. Chunks are joined in step order, which gives the same row order as a single
# dbgen run (verified on sf_1 against the parsed lineitem.tbl).
#
# Runs on cpu_node (default 1) with its memory there too, so a DRAM budget on node 0 is not
# touched. Writes <outdir>/lineitem_cols.bin (9 B/row) and gen_cols.log.
set -uo pipefail

SF=${1:?scale factor}; CHUNKS=${2:?chunks}; OUT=${3:?outdir}; NODE=${4:-1}
DBGEN_DIR=$HOME/backup/tpch-dbgen
TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PIN="numactl --cpunodebind=$NODE --membind=$NODE"

[ -x "$TOOLS/tpch_cols" ] || g++ -O3 -march=native -o "$TOOLS/tpch_cols" "$TOOLS/tpch_cols.cpp" || exit 1
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
WORK="$OUT/.work"; rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK" || exit 1
cp "$DBGEN_DIR/dists.dss" .
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$OUT/gen_cols.log"; }

log "lineitem SF=$SF, $CHUNKS chunks, cpu/mem node $NODE"
readers=(); writers=()
for k in $(seq 1 "$CHUNKS"); do
    rm -f "lineitem.tbl.$k"; mkfifo "lineitem.tbl.$k"
    $PIN "$TOOLS/tpch_cols" "keys.$k" "qty.$k" "lineitem.tbl.$k" > "stats.$k" 2>>cols.log &
    readers+=($!)
done
for k in $(seq 1 "$CHUNKS"); do
    $PIN "$DBGEN_DIR/dbgen" -q -f -s "$SF" -T L -C "$CHUNKS" -S "$k" -b dists.dss >>dbgen.log 2>&1 &
    writers+=($!)
done
fail=0
for p in "${writers[@]}" "${readers[@]}"; do wait "$p" || fail=1; done
if [ $fail -ne 0 ]; then log "FAILED: see $WORK/dbgen.log and cols.log"; exit 1; fi
log "generated; assembling"

rows=0; max_key=0
for k in $(seq 1 "$CHUNKS"); do
    read -r r m < "stats.$k"
    rows=$((rows + r)); [ "$m" -gt "$max_key" ] && max_key=$m
done

# Header: 8-byte magic, rows, src_size=0, src_mtime=0 (no source file).
python3 -c "import struct,sys; sys.stdout.buffer.write(b'TPCHSF02' + struct.pack('<QQQ', $rows, 0, 0))" > "$OUT/lineitem_cols.bin.tmp"
for k in $(seq 1 "$CHUNKS"); do cat "keys.$k" >> "$OUT/lineitem_cols.bin.tmp" && rm -f "keys.$k"; done
for k in $(seq 1 "$CHUNKS"); do cat "qty.$k"  >> "$OUT/lineitem_cols.bin.tmp" && rm -f "qty.$k"; done

expect=$(( 32 + rows * 9 ))
got=$(stat -c %s "$OUT/lineitem_cols.bin.tmp")
if [ "$got" -ne "$expect" ]; then log "FAILED: file is $got bytes, expected $expect"; exit 1; fi
mv "$OUT/lineitem_cols.bin.tmp" "$OUT/lineitem_cols.bin"
cd "$OUT" && rm -rf "$WORK"
log "DONE: $OUT/lineitem_cols.bin  rows=$rows  max_l_orderkey=$max_key  bytes=$got"
