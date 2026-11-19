#!/usr/bin/env bash
# mayhem/build.sh — build USearch's fuzz harnesses (sanitized) and its C test suite (normal flags).
# Runs in the commit image as `mayhem` in /mayhem; re-runnable and offline (yyjson/{fmt} come from
# /opt/toolchains/deps, pre-fetched by mayhem/Dockerfile).
set -euo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
: "${SRC:=/mayhem}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"
DEPS=/opt/toolchains/deps
OFFLINE=(-DFETCHCONTENT_FULLY_DISCONNECTED=ON
         -DFETCHCONTENT_SOURCE_DIR_YYJSON="$DEPS/yyjson-0.13.0/src"
         -DFETCHCONTENT_SOURCE_DIR_FMT="$DEPS/fmt-12.2.0/src")
# One portable CPU class (no runtime dispatch) keeps the build short and the code paths deterministic.
COMMON=(-DUSEARCH_BUILD_SHARED=ON -DUSEARCH_MULTIVERSIONED=OFF -DUSEARCH_BUILD_TOOLS=OFF)

# 1) The library, instrumented (ASan+UBSan halting + SanCov). RelWithDebInfo with its per-config flags
#    replaced by -O1, so neither -DNDEBUG (asserts stay on) nor a DWARF-5 `-g` sneaks in.
FUZZ_FLAGS="$SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link"
cmake -S . -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_FLAGS="$FUZZ_FLAGS" -DCMAKE_CXX_FLAGS="$FUZZ_FLAGS" \
    -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O1" -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O1" \
    "${COMMON[@]}" "${OFFLINE[@]}"
cmake --build build-fuzz --target usearch_static -j"$MAYHEM_JOBS"
LIB="$SRC/build-fuzz/libusearch_static.a"
LIBS=("$LIB" -lstdc++ -lpthread -lm -ldl)

# 2) Harnesses: libFuzzer binary + standalone reproducer each; LeakSanitizer off at build time.
OBJ="$SRC/build-fuzz/mayhem"
mkdir -p "$OBJ"
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c mayhem/lsan_off.cc -o "$OBJ/lsan_off.o"
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$OBJ/standalone_main.o"
build_harness() { # <name> <source> [defines...]
    local name="$1" src="$2"; shift 2
    $CC $SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link -O1 -Ic "$@" -c "$src" -o "$OBJ/$name.o"
    $CXX $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "$OBJ/$name.o" "$OBJ/lsan_off.o" "${LIBS[@]}" -o "$SRC/$name"
    $CXX $SANITIZER_FLAGS $DEBUG_FLAGS "$OBJ/standalone_main.o" "$OBJ/$name.o" "$OBJ/lsan_off.o" "${LIBS[@]}" \
        -o "$SRC/$name-standalone"
}
build_harness fuzz_dense_load      mayhem/harnesses/fuzz_load.c -DUSEARCH_FUZZ_DENSE
build_harness fuzz_sequence_load   mayhem/harnesses/fuzz_load.c -DUSEARCH_FUZZ_SEQUENCE
build_harness fuzz_shapes_load     mayhem/harnesses/fuzz_load.c -DUSEARCH_FUZZ_SHAPES
build_harness fuzz_geospatial_load mayhem/harnesses/fuzz_load.c -DUSEARCH_FUZZ_GEOSPATIAL
build_harness fuzz_dense_ops       mayhem/harnesses/fuzz_dense_ops.c
build_harness fuzz_dense_view      mayhem/harnesses/fuzz_load.c -DUSEARCH_FUZZ_DENSE -DUSEARCH_FUZZ_VIEW
build_harness fuzz_sequence_view   mayhem/harnesses/fuzz_load.c -DUSEARCH_FUZZ_SEQUENCE -DUSEARCH_FUZZ_VIEW
build_harness fuzz_shapes_view     mayhem/harnesses/fuzz_load.c -DUSEARCH_FUZZ_SHAPES -DUSEARCH_FUZZ_VIEW
build_harness fuzz_geospatial_view mayhem/harnesses/fuzz_load.c -DUSEARCH_FUZZ_GEOSPATIAL -DUSEARCH_FUZZ_VIEW
# The header-only Arrow IPC codec behind `usearch import`/`recall` (tools/arrow_ipc.hpp): C++20, no library.
$CXX -std=c++20 $SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link -O1 -Itools -Iinclude \
    -c mayhem/harnesses/fuzz_arrow_ipc.cpp -o "$OBJ/fuzz_arrow_ipc.o"
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "$OBJ/fuzz_arrow_ipc.o" "$OBJ/lsan_off.o" -o "$SRC/fuzz_arrow_ipc"
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS "$OBJ/standalone_main.o" "$OBJ/fuzz_arrow_ipc.o" "$OBJ/lsan_off.o" \
    -o "$SRC/fuzz_arrow_ipc-standalone"

# 3) Upstream's C test suite (c/test.c) with the project's normal flags, for mayhem/test.sh.
cmake -S . -B build-test -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$COVERAGE_FLAGS" -DCMAKE_CXX_FLAGS="$COVERAGE_FLAGS" \
    -DUSEARCH_BUILD_TEST=ON "${COMMON[@]}" "${OFFLINE[@]}"
cmake --build build-test --target usearch_test_c -j"$MAYHEM_JOBS"
