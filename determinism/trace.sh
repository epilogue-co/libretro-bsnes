#!/usr/bin/env bash
# Write a per-frame determinism hash trace via the vendored libretro-hashtrace tool. The tool hashes
# canonical RAM (SYSTEM_RAM + SAVE_RAM) over the ROM with the bsnes determinism pins; traces from every
# OS/arch/toolchain leg must be byte-identical (the cross-platform gate).
#
# Usage: trace.sh <out-file> <core> <rom> [frames]
# Env: HASHTRACE_TOOL overrides the tool path (default: the standalone build dir next to this script).
set -u -o pipefail

here="$(cd "$(dirname "$0")" && pwd)"
out="${1:?usage: trace.sh <out-file> <core> <rom> [frames]}"
core="${2:?usage: trace.sh <out-file> <core> <rom> [frames]}"
rom="${3:?usage: trace.sh <out-file> <core> <rom> [frames]}"
frames="${4:-1800}"
tool="${HASHTRACE_TOOL:-$here/hashtrace/build/libretro-hashtrace}"

[ -x "$tool" ] || { echo "FAIL: hashtrace tool not built at $tool"; exit 1; }
[ -f "$core" ] || { echo "FAIL: core not found at $core"; exit 1; }
[ -f "$rom" ]  || { echo "FAIL: rom not found at $rom"; exit 1; }

"$tool" "$rom" --core "$core" --out "$out" --frames "$frames"
lines=$(wc -l < "$out" | tr -d ' ')
[ "$lines" -eq "$frames" ] || { echo "FAIL: trace has $lines lines, expected $frames"; exit 1; }
echo "trace written: $out ($lines frames, core=$core)"
