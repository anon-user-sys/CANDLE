#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

g++ -O2 -std=c++17 -pthread -I"$SCRIPT_DIR" "$SCRIPT_DIR/main_host.cpp" -o "$SCRIPT_DIR/main_host"

echo "Built: $SCRIPT_DIR/main_host"
