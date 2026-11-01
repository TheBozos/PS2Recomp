# Windows cross build from Linux

`tools/windows_cross/build.sh` builds PS2Recomp for Windows (x86_64-w64-mingw32) on a Linux machine with
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (Clang, libc++, UCRT) and, with `--tests`, runs the tests under
Wine. It is a check, not a release build: it catches what only a Windows toolchain sees (libc++ headers that GCC's
libstdc++ pulls in transitively, POSIX-only calls, MinGW link differences) without a Windows machine. The first break
it caught was `std::getenv` used without `<cstdlib>`, which libstdc++ forgives and libc++ does not.

## What it needs

- [mise](https://mise.jdx.dev), installed for the user. Everything else comes from mise, user-level, nothing from the
  system's package manager: llvm-mingw, CMake, Ninja and, for `--tests`, a Wine build
  ([Kron4ek/Wine-Builds](https://github.com/Kron4ek/Wine-Builds)).
- `curl`, `sha256sum`, `pkg-config` (or `pkgconf`) and `git` on the host, which build machines already have.
- Network access to GitHub: the fork's CMake fetches its dependencies (raylib, imgui, Granite, ...) at configure time.

FFmpeg comes as [BtbN's](https://github.com/BtbN/FFmpeg-Builds) LGPL shared Windows build, pinned to a release that
BtbN keeps (a month-end autobuild) and checked against its SHA-256 before it is unpacked into its own directory of
the cache. CMake finds it with pkg-config, as on Linux and MSYS2.

## Use

```bash
tools/windows_cross/build.sh                 # configure and build everything (the default `all` target)
tools/windows_cross/build.sh --tests         # and run ps2x_tests.exe and the CTest suites under Wine
tools/windows_cross/build.sh --runner ../MyPort --no-fork   # build a port's runner instead
```

| Option | Effect |
| --- | --- |
| `--tests` | run `ps2x_tests.exe` and the CTest suites (the GS cache suites) under Wine |
| `--runner <port dir>` | also build that port's CMake project (target `ps2EntryRunner`) and copy the DLLs it needs beside it |
| `--runner-target <t>` | build that port target instead of `ps2EntryRunner` (repeatable) |
| `--target <t>` | build only that PS2Recomp target (repeatable; default: all) |
| `--no-fork` | skip PS2Recomp's own build (with `--runner`) |
| `--build-dir <dir>` | build trees and logs (default `out/windows-cross`) |
| `--cache-dir <dir>` | FFmpeg and the Wine prefix (default `${XDG_CACHE_HOME:-~/.cache}/ps2recomp-windows-cross`) |
| `--jobs <n>`, `--config <type>` | parallel jobs (default: all cores), `CMAKE_BUILD_TYPE` (default Release) |
| `-- <args>` | passed to CMake's configure step, e.g. `-- -DPS2X_ENABLE_PARALLEL_GS=OFF` |

The port given to `--runner` must be ready to configure: generated code in place, and any step the port runs before
CMake already done. Its CMake project normally adds PS2Recomp with `add_subdirectory`, so it builds against the port's
own copy of PS2Recomp, not necessarily this checkout.

The exit status is 0 when everything built and the tests passed, 1 when a configure or build failed (the errors are
printed grouped, the full logs are in the build directory) and 2 when a test failed.

Versions are pinned in the script and can be overridden from the environment: `LLVM_MINGW_VERSION`, `WINE_VERSION`,
`CMAKE_VERSION`, `NINJA_VERSION`, and `FFMPEG_URL` with `FFMPEG_SHA256` (both or neither). `TESTS_TIMEOUT` (seconds,
default 1800) bounds each test run.

## On a remote build machine

The script builds where it runs. To use a remote Linux box, copy the tree there and run it over ssh; on a box shared
by several sessions, hold its lock while it builds (`docs/benchlock.md`):

```bash
rsync -a --delete --exclude=.git --exclude=/out/ ./ buildbox:ps2recomp/
benchlock run -t MYPORT -m comp --host buildbox -w "Windows cross check" -e 20 -- \
    ssh buildbox 'cd ps2recomp && tools/windows_cross/build.sh --tests'
```

On a 40-core machine a build from scratch takes about a minute once the dependencies are fetched (PS2Recomp's
`all`, 52 s; a port's runner, 32 s more) and the tests another minute under Wine. Disk: about 2 GB for PS2Recomp's
build tree and 1 GB for a runner's, 0.8 GB of cache (FFmpeg, Wine prefix) and 1.4 GB of mise tools (llvm-mingw,
Wine).

## Limits

- Wine runs the console tests well; the runner itself starts there too, but a game run is checked on Windows.
- The executables link libc++ and the UCRT dynamically: `--runner` copies `libc++.dll`, `libunwind.dll` and the FFmpeg
  DLLs beside the runner, so the directory runs as is on Windows 10 and 11.
- MSYS2's UCRT64 GCC (the usual Windows toolchain) uses libstdc++; this check uses libc++. A tree that passes here can
  still break with GCC on Windows, and the reverse, but both share MinGW's headers and the Windows API.
