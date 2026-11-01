#!/usr/bin/env bash
# Cross-builds PS2Recomp for Windows (x86_64-w64-mingw32, llvm-mingw: Clang, libc++, UCRT) on a Linux host, and with
# --tests runs ps2x_tests.exe under Wine. Every tool comes from mise (user-level, no system packages); FFmpeg's Windows
# dev build is downloaded once into the cache, checksum verified. See docs/windows_cross.md.
#
#   tools/windows_cross/build.sh [--tests] [--runner <port dir>] [options] [-- <extra cmake args>]
#
#   --tests               run ps2x_tests.exe and the CTest suites under Wine and report the result
#   --runner <port dir>   also build that port's runner (its CMakeLists.txt, target ps2EntryRunner) and stage its DLLs
#   --runner-target <t>   the port's target to build instead of ps2EntryRunner (repeatable)
#   --target <t>          build only this target of PS2Recomp (repeatable; default: all)
#   --no-fork             skip PS2Recomp's own build (with --runner)
#   --build-dir <dir>     build trees (default: <PS2Recomp>/out/windows-cross)
#   --cache-dir <dir>     downloads and the Wine prefix (default: ${XDG_CACHE_HOME:-~/.cache}/ps2recomp-windows-cross)
#   --jobs <n>            parallel jobs (default: nproc)
#   --config <type>       CMAKE_BUILD_TYPE (default: Release)
#
# Pinned versions, each overridable from the environment:
#   LLVM_MINGW_VERSION, WINE_VERSION, CMAKE_VERSION, NINJA_VERSION, FFMPEG_URL with FFMPEG_SHA256 (both or neither)
# Exit status: 0 when everything asked for built (and the tests passed), 1 on a build failure, 2 on a test failure.
set -euo pipefail

fork=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
toolchain=$fork/tools/windows_cross/llvm-mingw.cmake

tests=0
runner=
runner_targets=()
targets=()
build_fork=1
build_dir=$fork/out/windows-cross
cache_dir=${XDG_CACHE_HOME:-$HOME/.cache}/ps2recomp-windows-cross
jobs=$(nproc 2> /dev/null || echo 4)
config=Release
cmake_extra=()

usage() { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; exit "${1:-0}"; }

while [ $# -gt 0 ]; do
    case $1 in
        --tests) tests=1 ;;
        --runner) runner=$(cd "$2" && pwd); shift ;;
        --runner-target) runner_targets+=("$2"); shift ;;
        --target) targets+=("$2"); shift ;;
        --no-fork) build_fork=0 ;;
        --build-dir) build_dir=$2; shift ;;
        --cache-dir) cache_dir=$2; shift ;;
        --jobs) jobs=$2; shift ;;
        --config) config=$2; shift ;;
        -h | --help) usage ;;
        --) shift; cmake_extra=("$@"); break ;;
        *) echo "build.sh: unknown argument $1" >&2; usage 64 >&2 ;;
    esac
    shift
done
[ "${#runner_targets[@]}" -gt 0 ] || runner_targets=(ps2EntryRunner)
if [ "$build_fork" = 0 ] && [ -z "$runner" ]; then
    echo "build.sh: --no-fork needs --runner" >&2
    exit 64
fi
if [ "$build_fork" = 0 ] && [ "$tests" = 1 ]; then
    echo "build.sh: --tests runs PS2Recomp's ps2x_tests, it cannot go with --no-fork" >&2
    exit 64
fi

LLVM_MINGW_VERSION=${LLVM_MINGW_VERSION:-20261020}
WINE_VERSION=${WINE_VERSION:-11.21}
CMAKE_VERSION=${CMAKE_VERSION:-4.4.5}
NINJA_VERSION=${NINJA_VERSION:-1.13.2}
if [ -n "${FFMPEG_URL:-}${FFMPEG_SHA256:-}" ] && { [ -z "${FFMPEG_URL:-}" ] || [ -z "${FFMPEG_SHA256:-}" ]; }; then
    echo "build.sh: FFMPEG_URL and FFMPEG_SHA256 go together" >&2
    exit 64
fi
FFMPEG_URL=${FFMPEG_URL:-https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-31-12-59/ffmpeg-n9.0.2-39-g7adfbe40dc-win64-lgpl-shared-9.0.zip}
FFMPEG_SHA256=${FFMPEG_SHA256:-e907b01fa4783913151237e37df2a10fa97d99a1598a021bf59d4ae62ffed4c0}

say() { echo "[windows-cross] $*" >&2; }

command -v mise > /dev/null || { say "mise is required (https://mise.jdx.dev), user-level install"; exit 1; }
export MISE_EXPERIMENTAL=1

case $(uname -m) in
    x86_64) host_arch=x86_64 ;;
    aarch64 | arm64) host_arch=aarch64 ;;
    *) say "no llvm-mingw build for host $(uname -m)"; exit 1 ;;
esac
llvm_spec="github:mstorsjo/llvm-mingw[asset_pattern=llvm-mingw-*-ucrt-ubuntu-22.04-$host_arch.tar.xz]@$LLVM_MINGW_VERSION"
wine_spec="github:Kron4ek/Wine-Builds[asset_pattern=wine-*-amd64-wow64.tar.xz]@$WINE_VERSION"
build_tools=("cmake@$CMAKE_VERSION" "ninja@$NINJA_VERSION")

mise_tool()
{
    mise install --quiet "$1" >&2
    mise where "$1"
}

say "tools: llvm-mingw $LLVM_MINGW_VERSION, cmake $CMAKE_VERSION, ninja $NINJA_VERSION"
LLVM_MINGW=$(mise_tool "$llvm_spec")
for tool in "${build_tools[@]}"; do mise_tool "$tool" > /dev/null; done
export LLVM_MINGW
cmake=(mise exec "${build_tools[@]}" -- cmake)

command -v pkg-config > /dev/null || command -v pkgconf > /dev/null || {
    say "pkg-config (or pkgconf) is required on the build host to find FFmpeg"; exit 1; }

fetch_ffmpeg()
{
    local dir=$cache_dir/ffmpeg-$FFMPEG_SHA256 name sum
    name=$(basename "$FFMPEG_URL")
    if [ ! -f "$dir/ready" ]; then
        rm -rf "$dir"
        mkdir -p "$dir/download" "$dir/tree"
        say "downloading $name"
        curl -fsSL --retry 3 -o "$dir/download/$name" "$FFMPEG_URL"
        sum=$(sha256sum "$dir/download/$name" | cut -d' ' -f1)
        if [ "$sum" != "$FFMPEG_SHA256" ]; then
            say "checksum mismatch for $name: got $sum, expected $FFMPEG_SHA256"
            rm -rf "$dir"
            exit 1
        fi
        (cd "$dir/tree" && "${cmake[@]}" -E tar xf "$dir/download/$name")
        rm -rf "$dir/download"
        : > "$dir/ready"
    fi
    local root
    root=$(find "$dir/tree" -mindepth 1 -maxdepth 1 -type d | head -1)
    [ -d "$root/lib/pkgconfig" ] || { say "$name has no lib/pkgconfig"; exit 1; }
    echo "$root"
}

WIN_FFMPEG=$(fetch_ffmpeg)
export WIN_FFMPEG
export PKG_CONFIG_LIBDIR=$WIN_FFMPEG/lib/pkgconfig PKG_CONFIG_PATH='' PKG_CONFIG_SYSROOT_DIR=''
say "FFmpeg: $WIN_FFMPEG"

configure_flags=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE="$config"
    -DCMAKE_C_FLAGS=-msse4.1 -DCMAKE_CXX_FLAGS=-msse4.1)
if [ "$tests" = 1 ]; then
    say "tools: wine $WINE_VERSION"
    wine=$(mise_tool "$wine_spec")/bin/wine
    configure_flags+=(-DCMAKE_CROSSCOMPILING_EMULATOR="$wine")
fi

build_tree()
{
    local name=$1 source=$2 tree=$3 log=$build_dir/$1.log
    shift 3
    say "$name: configuring $source"
    if ! "${cmake[@]}" -S "$source" -B "$tree" "${configure_flags[@]}" "${cmake_extra[@]}" > "$build_dir/$name.configure.log" 2>&1; then
        tail -40 "$build_dir/$name.configure.log" >&2
        say "$name: configure FAILED (log: $build_dir/$name.configure.log)"
        return 1
    fi
    local target_args=()
    [ $# -gt 0 ] && target_args=(--target "$@")
    say "$name: building ${*:-all} with $jobs jobs"
    local start=$SECONDS
    if ! "${cmake[@]}" --build "$tree" "${target_args[@]}" -j "$jobs" -- -k 0 > "$log" 2>&1; then
        grep -E 'error|undefined (symbol|reference)' "$log" | sed -E 's/^\[[0-9]+\/[0-9]+\] //' | sort | uniq -c | sort -rn | head -60 >&2
        say "$name: build FAILED after $((SECONDS - start)) s (log: $log)"
        return 1
    fi
    say "$name: built in $((SECONDS - start)) s"
}

dll_dirs=("$LLVM_MINGW/x86_64-w64-mingw32/bin" "$WIN_FFMPEG/bin")

stage_dlls()
{
    local todo=("$@") next dll dir file out
    out=$(dirname "$1")
    while [ "${#todo[@]}" -gt 0 ]; do
        next=()
        for file in "${todo[@]}"; do
            for dll in $("$LLVM_MINGW/bin/llvm-objdump" -p "$file" | awk '/DLL Name:/ { print $3 }'); do
                [ -f "$out/$dll" ] && continue
                for dir in "${dll_dirs[@]}"; do
                    if [ -f "$dir/$dll" ]; then
                        cp "$dir/$dll" "$out/"
                        next+=("$out/$dll")
                        break
                    fi
                done
            done
        done
        todo=("${next[@]+"${next[@]}"}")
    done
}

mkdir -p "$build_dir"
status=0
if [ "$build_fork" = 1 ]; then
    build_tree fork "$fork" "$build_dir/fork" "${targets[@]+"${targets[@]}"}" || status=1
fi
if [ -n "$runner" ]; then
    if build_tree runner "$runner" "$build_dir/runner" "${runner_targets[@]}"; then
        while IFS= read -r exe; do
            stage_dlls "$exe"
            say "runner: $exe (DLLs staged beside it)"
        done < <(find "$build_dir/runner" -name '*.exe' -newer "$build_dir/runner/CMakeCache.txt" -not -path '*/CMakeFiles/*')
    else
        status=1
    fi
fi
[ "$status" = 0 ] || exit 1

if [ "$tests" = 1 ]; then
    exe=$(find "$build_dir/fork" -name ps2x_tests.exe -not -path '*/CMakeFiles/*' | head -1)
    [ -n "$exe" ] || { say "ps2x_tests.exe was not built"; exit 1; }
    while IFS= read -r built; do
        stage_dlls "$built"
    done < <(find "$build_dir/fork" -name '*.exe' -not -path '*/CMakeFiles/*' -not -path '*/_deps/*')
    export WINEPREFIX=$cache_dir/wineprefix WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
    mkdir -p "$WINEPREFIX"
    failed=0
    say "tests: running $(basename "$exe") under Wine"
    set +e
    (cd "$fork" && timeout "${TESTS_TIMEOUT:-1800}" "$wine" "$exe") > "$build_dir/ps2x_tests.log" 2>&1
    code=$?
    set -e
    tail -6 "$build_dir/ps2x_tests.log" >&2
    if [ "$code" = 0 ]; then
        say "tests: ps2x_tests PASSED (log: $build_dir/ps2x_tests.log)"
    else
        say "tests: ps2x_tests FAILED, exit $code (log: $build_dir/ps2x_tests.log)"
        failed=1
    fi
    ctest=(mise exec "${build_tools[@]}" -- ctest --test-dir "$build_dir/fork/ps2xTest" -j "$jobs" --output-on-failure
        --timeout "${TESTS_TIMEOUT:-1800}")
    if [ "$("${ctest[@]}" -N 2> /dev/null | awk '/^Total Tests:/ { print $3 }')" != 0 ]; then
        say "tests: running the CTest suites under Wine"
        if "${ctest[@]}" > "$build_dir/ctest.log" 2>&1; then
            say "tests: CTest $(grep -E 'tests passed' "$build_dir/ctest.log" | tail -1)"
        else
            grep -E 'tests passed|\*\*\*Failed|\(Failed\)|Not Run|Timeout' "$build_dir/ctest.log" | tail -30 >&2
            say "tests: CTest FAILED (log: $build_dir/ctest.log)"
            failed=1
        fi
    fi
    [ "$failed" = 0 ] || exit 2
fi
say "done"
