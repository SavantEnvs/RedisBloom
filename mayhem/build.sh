#!/usr/bin/env bash
#
# mayhem/build.sh — build this repo's fuzz harness(es). EDIT per repo.
#
# Runs inside the commit image (mayhem/Dockerfile) as `mayhem` in /mayhem. The base image
# (ghcr.io/savantenvs/base) already exports the build contract — use these, don't redefine:
#   CC, CXX             stock clang / clang++
#   LIB_FUZZING_ENGINE  -fsanitize=fuzzer   (link into each harness that has a LLVMFuzzer entry)
#   SANITIZER_FLAGS     -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
#                       (ASan + UBSan, both set to HALT — so Mayhem catches memory AND UB defects)
#   DEBUG_FLAGS         -g -gdwarf-3   (DWARF debug info — always on for fuzz/standalone builds,
#                       independent of the sanitizer off-switch; DWARF version must be < 4)
#   RUST_DEBUG_FLAGS    -C debuginfo=2 -C force-frame-pointers=yes -C llvm-args=-gdwarf-3
#                       (thread through RUSTFLAGS on every cargo-fuzz build)
#   GO_DEBUG_FLAGS      -gcflags=all=-N -l
#                       (thread through go build / go-fuzz-build so the linked ELF keeps symbols)
#   SRC                 /mayhem (the repo source)
#
# Contract: build EVERYTHING here — one runnable binary per fuzz harness, AND the project's test
# suite (so mayhem/test.sh only has to RUN it, never compile). Keep it ADDITIVE (build upstream as
# upstream documents; don't edit upstream files). IMPORTANT: build the PROJECT ITSELF with
# $SANITIZER_FLAGS and $DEBUG_FLAGS (not just the harness) so the fuzzed code is instrumented
# AND carries DWARF < 4 symbols — otherwise ASan/UBSan only see the harness, not the library
# you're trying to find bugs in, and backtraces won't resolve project source lines. Build the TEST
# suite with the project's NORMAL flags (a clean, independent build) so test.sh stays an honest
# functional oracle and won't false-fail on benign UB. Leave the test binary/runner where test.sh
# expects it.
set -euo pipefail

# clang rejects SOURCE_DATE_EPOCH='' (empty) — it must be unset or a valid integer.
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

# Build knobs come from the ENVIRONMENT (overridable), with sane defaults — no if-statements,
# just parameter-expansion fallbacks. Default sanitizers are the base's ASan+UBSan (halting); override
# per build via the Dockerfile's `--build-arg SANITIZER_FLAGS="..."`.
# NB: SANITIZER_FLAGS uses `=` (no colon) on purpose — `=` only fills when the var is UNSET, so an
# explicit EMPTY value (`--build-arg SANITIZER_FLAGS=`) is honored and builds with NO sanitizers
# (useful when you want the program's natural crash / full backtrace, not an ASan report). The other
# knobs use `:=` (default on empty too). MAYHEM_JOBS sets build parallelism (falls back to nproc).
: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
# DEBUG_FLAGS carries DWARF debug info INDEPENDENTLY of the sanitizer off-switch (so an empty
# SANITIZER_FLAGS still yields DWARF symbols). DWARF MUST be < 4 (Mayhem triage can't read >=4); clang-19's
# plain `-g` emits DWARF-5, so `-gdwarf-3` is explicit. Apply $DEBUG_FLAGS to the fuzz/harness/standalone
# builds (NOT the test/oracle build). Rust/Go carry the same intent via their language flags below.
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${RUST_DEBUG_FLAGS:=-C debuginfo=2 -C force-frame-pointers=yes -C llvm-args=-gdwarf-3}"
: "${GO_DEBUG_FLAGS:=-gcflags=all=-N -l}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
# COVERAGE_FLAGS: empty by default → no effect on the normal oracle build. Set it via the Dockerfile's
# `--build-arg COVERAGE_FLAGS="-fprofile-instr-generate -fcoverage-mapping"` to instrument the TEST
# build for source-coverage measurement (how much of the project the test suite actually exercises —
# a quality signal for the oracle; complements the anti-reward-hack sabotage check). APPEND it to the
# test build's compile+link flags in step 3 (NOT the fuzz build); after `test.sh` runs, merge with
# `llvm-profdata` and report with `llvm-cov`. Empty value is honored (`=`, not `:=`).
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS RUST_DEBUG_FLAGS GO_DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"

# Dependencies are read from the in-image cache (Dockerfile); nothing here touches the network.
SDK=$(echo /opt/toolchains/deps/RedisModulesSDK-*/src)
TDG=$(echo /opt/toolchains/deps/t-digest-c-*/src)/src
RDY=$(echo /opt/toolchains/deps/readies-*/include)
for d in "$SDK" "$TDG" "$RDY"; do [ -d "$d" ] || { echo "missing dependency cache: $d" >&2; exit 1; }; done

OUT=/mayhem/bin
GEN=$OUT/gen
rm -rf "$OUT"
mkdir -p "$GEN/inc/a" "$GEN/inc/deps" "$GEN/fuzz" "$GEN/oracle" "$GEN/h"
# src/td_redismodule_malloc.h includes "../deps/RedisModulesSDK/redismodule.h"; resolve it via -I
# even when the deps/ submodule directories are empty in this checkout.
ln -sfn "$SDK" "$GEN/inc/deps/RedisModulesSDK"

DEFS="-D_GNU_SOURCE -DREDIS_MODULE_TARGET -DREDISMODULE_SDK_RLEC"
INCS="-I$SRC/mayhem -I$SRC/src -I$SDK -I$SRC/deps -I$SRC/deps/murmur2 -I$TDG -I$RDY -I$GEN/inc/a"
TDG_DEFS="-DTD_MALLOC_INCLUDE=\"$SRC/src/td_redismodule_malloc.h\""
CFLAGS_COMMON="-std=gnu99 -fblocks -fPIC -w $DEFS $INCS"

# Library sources, shared by the sanitized fuzz build and the plain oracle build. rebloom.c and
# rm_topk.c are compiled through mayhem/rb_static.c (their RDB callbacks are static).
LIB_SRCS="deps/bloom/bloom.c deps/murmur2/MurmurHash2.c deps/rmutil/util.c
  src/cmd_info/cf_info.c src/cmd_info/bf_info.c src/cmd_info/cms_info.c src/cmd_info/tdigest_info.c
  src/cmd_info/topk_info.c src/sb.c src/cf.c src/topk.c src/rm_cms.c src/rm_tdigest.c src/cms.c
  src/config.c"

# Blocks runtime (clang `defer`): prefer the static archive.
BLOCKS_LIB=-lBlocksRuntime
for d in /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu; do
  [ -f "$d/libBlocksRuntime.a" ] && BLOCKS_LIB="$d/libBlocksRuntime.a"
done

# 1) Sanitized fuzz build of the library: SanCov (fuzzer-no-link) + ASan/UBSan on the fuzzed code.
FUZZ_CFLAGS="$SANITIZER_FLAGS $DEBUG_FLAGS -O1 -fsanitize=fuzzer-no-link $CFLAGS_COMMON"
compile_fuzz() { # <src> <obj-stem>
  "$CC" $FUZZ_CFLAGS -c "$SRC/$1" -o "$GEN/fuzz/$2.o"
}
jobs_pids=()
for f in $LIB_SRCS mayhem/rb_static.c mayhem/mock.c; do
  compile_fuzz "$f" "$(echo "$f" | tr / _)" &
  jobs_pids+=($!)
  while [ "$(jobs -rp | wc -l)" -ge "$MAYHEM_JOBS" ]; do sleep 0.1; done
done
"$CC" $FUZZ_CFLAGS $TDG_DEFS -c "$TDG/tdigest.c" -o "$GEN/fuzz/tdigest.o" &
jobs_pids+=($!)
"$CXX" $SANITIZER_FLAGS $DEBUG_FLAGS -c "$SRC/mayhem/lsan_off.cc" -o "$GEN/fuzz/lsan_off.o" &
jobs_pids+=($!)
for p in "${jobs_pids[@]}"; do wait "$p"; done
FUZZ_OBJS=$(ls "$GEN"/fuzz/*.o)

# 2) One libFuzzer binary + one standalone reproducer per harness.
TARGETS="fuzz_bf_rdb_load fuzz_cf_rdb_load fuzz_cms_rdb_load fuzz_topk_rdb_load fuzz_tdigest_rdb_load
  fuzz_bf_loadchunk fuzz_cf_loadchunk"
"$CC" $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$GEN/h/standalone_main.o"
for t in $TARGETS; do
  "$CC" $SANITIZER_FLAGS $DEBUG_FLAGS -O1 $CFLAGS_COMMON -c "$SRC/mayhem/$t.c" -o "$GEN/h/$t.o" &
done
wait
for t in $TARGETS; do
  "$CC" $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "$GEN/h/$t.o" $FUZZ_OBJS \
      $BLOCKS_LIB -lm -o "$OUT/$t"
  "$CC" $SANITIZER_FLAGS $DEBUG_FLAGS "$GEN/h/standalone_main.o" "$GEN/h/$t.o" $FUZZ_OBJS \
      $BLOCKS_LIB -lm -o "$OUT/$t-standalone"
done

# Seed generator (runs the real serializers; used to author mayhem/*/testsuite).
"$CC" $SANITIZER_FLAGS $DEBUG_FLAGS -O1 $CFLAGS_COMMON -c "$SRC/mayhem/seedgen.c" -o "$GEN/h/seedgen.o"
"$CC" $SANITIZER_FLAGS $DEBUG_FLAGS "$GEN/h/seedgen.o" $FUZZ_OBJS $BLOCKS_LIB -lm -o "$OUT/seedgen"

# 3) Test oracle: the module as a shared object with the project's NORMAL flags (no sanitizers),
#    the upstream unit tests, and mayhem/kat.c (known-answer probes), all linked dynamically so the
#    sabotage check's LD_PRELOAD shim takes effect.
OR_CFLAGS="-O2 $COVERAGE_FLAGS $CFLAGS_COMMON"
OR_OBJS=""
for f in $LIB_SRCS; do
  o="$GEN/oracle/$(echo "$f" | tr / _).o"
  "$CC" $OR_CFLAGS -c "$SRC/$f" -o "$o"; OR_OBJS="$OR_OBJS $o"
done
"$CC" $OR_CFLAGS $TDG_DEFS -c "$TDG/tdigest.c" -o "$GEN/oracle/tdigest.o"
"$CC" $OR_CFLAGS -c "$SRC/src/rebloom.c" -o "$GEN/oracle/rebloom.o"
"$CC" $OR_CFLAGS -c "$SRC/src/rm_topk.c" -o "$GEN/oracle/rm_topk.o"
"$CC" -shared $COVERAGE_FLAGS -o "$OUT/redisbloom.so" $OR_OBJS "$GEN/oracle/tdigest.o" \
    "$GEN/oracle/rebloom.o" "$GEN/oracle/rm_topk.o" $BLOCKS_LIB -lm
UT_CFLAGS="-O2 -std=gnu99 -fblocks -w $COVERAGE_FLAGS -I$SRC/tests/unit -I$SRC/deps -I$SDK -I$SRC/src -I$TDG -I$GEN/inc/a $DEFS"
for t in test-basic test-cuckoo; do
  "$CC" $UT_CFLAGS "$SRC/tests/unit/$t.c" -o "$OUT/$t" -Wl,-rpath,"$OUT" "$OUT/redisbloom.so" -lm
done
"$CC" $UT_CFLAGS -I"$SRC/mayhem" "$SRC/mayhem/kat.c" "$SRC/mayhem/mock.c" -o "$OUT/kat" \
    -Wl,-rpath,"$OUT" "$OUT/redisbloom.so" -lm
