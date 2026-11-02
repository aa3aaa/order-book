#!/usr/bin/env bash
# Profile-guided build of the benchmark (and everything else) in build/pgo.
#
#   scripts/pgo_build.sh            # then: ./build/pgo/bench
#
# 1. Build with instrumentation (OB_PGO=GENERATE), plus OB_NATIVE and OB_LTO.
# 2. Run a training workload: the benchmark on a *different* seed from the one
#    it measures, so the profile learns the shape of the work, not the exact ops.
# 3. Clang only: merge the raw profiles with llvm-profdata.
# 4. Rebuild in the same directory with OB_PGO=USE. (GCC names its profile files
#    after each object's path, so both phases must use the same build directory.)
set -euo pipefail
cd "$(dirname "$0")/.."
B=build/pgo
PROFILES="$PWD/$B/pgo"

rm -rf "$PROFILES"
cmake -S . -B "$B" -DCMAKE_BUILD_TYPE=Release -DOB_NATIVE=ON -DOB_LTO=ON -DOB_PGO=GENERATE -DOB_PGO_DIR="$PROFILES" >/dev/null
cmake --build "$B" -j --target bench
echo "training run..."
OB_BENCH_SEED=3 "$B/bench" 1000000 1 >/dev/null

if compgen -G "$PROFILES/*.profraw" >/dev/null; then
    if command -v xcrun >/dev/null && xcrun -f llvm-profdata >/dev/null 2>&1; then
        PROFDATA="xcrun llvm-profdata"
    else
        PROFDATA="$(command -v llvm-profdata || ls /usr/bin/llvm-profdata-* 2>/dev/null | sort -V | tail -1)"
    fi
    $PROFDATA merge -o "$PROFILES/merged.profdata" "$PROFILES"/*.profraw
fi

cmake -S . -B "$B" -DOB_PGO=USE >/dev/null
cmake --build "$B" -j --target bench
echo "done: run ./$B/bench"
