#!/usr/bin/env bash
# Full validation of the shared C++ core, as run for every change:
#   1. GCC + Clang builds with -Werror
#   2. unit tests (33 cases: bit-exact gapless, seek, recovery, stress …)
#   3. AddressSanitizer + UndefinedBehaviorSanitizer + LeakSanitizer
#   4. ThreadSanitizer (includes the multi-thread command-hammering test)
#   5. Valgrind memcheck (no errors, every allocation freed)
#   6. libFuzzer on the metadata parser and the decoders (FUZZ_SECONDS each, default 60)
#   7. Android JNI/AAudio compile check against the real headers
# Needs: cmake, ninja, clang (+ libclang-rt), gcc, valgrind, pkg-config, libflac-dev, libsndfile1-dev, libsqlite3-dev.
# Linux only for steps 5–6 (Valgrind/libFuzzer); on macOS run 1–4 with Apple clang (no LSan/Valgrind there).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
core="$root/core"
b="${BUILD_DIR:-$root/build/validate}"
FUZZ_SECONDS="${FUZZ_SECONDS:-60}"
mkdir -p "$b"
step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

step "GCC -Werror + unit tests"
cmake -S "$core" -B "$b/gcc" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER=gcc -DGROOVE_TESTS=ON -DGROOVE_TOOLS=ON -DGROOVE_WERROR=ON >/dev/null
ninja -C "$b/gcc" >/dev/null
"$b/gcc/groove_tests"

step "Clang ASan + UBSan + LSan"
cmake -S "$core" -B "$b/asan" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DGROOVE_TESTS=ON -DGROOVE_TOOLS=ON -DGROOVE_WERROR=ON -DGROOVE_SANITIZE=address,undefined >/dev/null
ninja -C "$b/asan" >/dev/null
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:detect_stack_use_after_return=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 "$b/asan/groove_tests"

step "Clang ThreadSanitizer"
cmake -S "$core" -B "$b/tsan" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DGROOVE_TESTS=ON -DGROOVE_WERROR=ON -DGROOVE_SANITIZE=thread >/dev/null
ninja -C "$b/tsan" >/dev/null
TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 "$b/tsan/groove_tests"

if command -v valgrind >/dev/null; then
  step "Valgrind memcheck"
  valgrind --tool=memcheck --leak-check=full --errors-for-leak-kinds=definite,indirect,possible --track-origins=yes \
           --error-exitcode=99 --fair-sched=yes "$b/gcc/groove_tests" 2>&1 | grep -E "ERROR SUMMARY|in use at exit|All heap blocks|test cases"
fi

step "libFuzzer (${FUZZ_SECONDS}s each)"
cmake -S "$core" -B "$b/fuzz" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DGROOVE_FUZZ=ON -DGROOVE_SANITIZE=address,undefined,fuzzer-no-link >/dev/null
ninja -C "$b/fuzz" fuzz_metadata fuzz_decoders >/dev/null
mkdir -p "$b/corpus-meta" "$b/corpus-dec"
"$b/fuzz/fuzz_metadata" -max_total_time="$FUZZ_SECONDS" -max_len=262144 -artifact_prefix="$b/" "$b/corpus-meta" "$core/tests/fixtures" 2>&1 | grep -E "^Done|ERROR|SUMMARY" || true
# The system libsndfile still has the Vorbis open leak that third_party/patches fixes for mobile builds.
LSAN_OPTIONS=suppressions="$core/fuzz/lsan-system-libsndfile.supp" \
"$b/fuzz/fuzz_decoders" -max_total_time="$FUZZ_SECONDS" -max_len=131072 -timeout=20 -artifact_prefix="$b/" "$b/corpus-dec" "$core/tests/fixtures" 2>&1 | grep -E "^Done|ERROR|SUMMARY" || true
if ls "$b"/crash-* "$b"/leak-* "$b"/timeout-* >/dev/null 2>&1; then echo "fuzzer artifacts found in $b"; exit 1; fi

step "Android JNI/AAudio compile check"
"$root/scripts/android-check/check.sh"

step "All checks passed"
