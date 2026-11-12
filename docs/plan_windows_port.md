# Windows port: the windows-vulkan platform

**Status. In progress.** Implemented and verified on Windows (see Verification, "Results"); open until `linux-vulkan` is built and run on Linux
with these changes, then the toolchain ADR is written and the plan lands.

## Current state

ngen builds and runs on Linux only: `build.cpp` declares one platform, `linux-vulkan`. The build system's runner spawns commands through
`/bin/sh -c` with `fork`/`execv` (`src/build/run/process.hpp`), quotes POSIX-style (`shell_quote`) and starts its own stages with `std::system("./_out/…")`
(`src/build/bootstrap.cpp`). The engine uses POSIX directly in about twenty files: TCP sockets with `poll` and `eventfd` (`src/rpc/core/`), `posix_spawn` in
the asset server's pack jobs, `fork`/`exec` in `ngen-cli`, inotify in `filewatcherlinux.cpp`, `dlopen` for RenderDoc, pthread thread names, `getpid`,
`isatty`. SDL3 is found with `pkg-config`; OpenUSD is built into `external/openusd_build` with `build_usd.py`.

## Scope

**In**

- A `windows-vulkan` platform in `build.cpp` (`NGEN_PLATFORM_WINDOWS`), beside `linux-vulkan`, for `debug` and `release`.
- `ngen-build` bootstraps and runs on Windows.
- Every program builds: `ngen-view`, `ngen-asset-server`, the packers, `ngen-pack`, `ngen-packdump`, `ngen-worldcheck`, `ngen-usdcheck`, `ngen-cli`,
  `ngen-introspect`, the RHI examples.
- Dependencies on Windows documented in the root README's Build section: the Vulkan SDK through winget, SDL3 and OpenUSD built from submodules.

**Out**

- `gamerelease` on Windows (its link flags are GNU ld's: `-Wl,-s`, `--gc-sections`). It stays declared; it is not expected to link.
- The `format` and `tidy` targets on Windows (they work if `clang-format` / `clang-tidy` are on PATH, but are not verified).
- A Windows file watcher using `ReadDirectoryChangesW` — Windows uses the watcher's existing poll-only mode (see Deferred).
- RenderDoc in-application capture on Windows beyond loading `renderdoc.dll` if it is already injected.
- CI for Windows.

## Decisions

- **Toolchain: `clang++` targeting the MSVC ABI (`x86_64-pc-windows-msvc`), `llvm-ar`, `lld`.** It takes the GNU-style flags the framework already emits, so
  the cxx command builders barely change, and it links against the MSVC-built OpenUSD and SDL3. Alternatives: `clang-cl` (every flag translated by the
  framework), MinGW (keeps more POSIX, but OpenUSD and the Vulkan SDK do not support it well).
- **One C runtime: the dynamic release CRT (`-fms-runtime-lib=dll`) in every configuration.** OpenUSD is built release with `/MD`; mixing CRTs or
  iterator-debug levels across the STL boundary does not link.
- **Dependencies.** Vulkan SDK through `winget install KhronosGroup.VulkanSDK` (headers, `vulkan-1.lib`, `glslc`, `shaderc_shared`, validation layers;
  `VULKAN_SDK` is set by the installer). SDL3: winget's only SDL3 package is a runtime-only repackage (no headers or import library), so SDL3 is a submodule,
  `external/sdl` at `release-3.4.14`, built with CMake into `external/sdl3_build` (gitignored, like `openusd_build`). OpenUSD: the same `build_usd.py` as on
  Linux, with Visual Studio 2022.
- **Commands run without a shell on Windows.** The runner passes the baked command line straight to `CreateProcessW`; `shell_quote` uses the Windows
  command-line quoting rules there (what `CommandLineToArgvW` and the CRT parse back). No command the build emits needs shell features, so there is no
  dependency on `sh.exe` (Git Bash) or `cmd.exe` semantics. The alternative, running `sh -c` from Git for Windows, keeps one code path but ties the build to
  Git Bash being installed and on a known path, and costs a shell per edge.
- **Per-platform source files, gated per source.** Code with a large platform-specific part goes into `*linux.cpp` / `*windows.cpp` beside its portable header
  (the existing `filewatcherlinux.cpp` naming), in the library that owns it. `build.cpp` gates each with
  `for_source(path, [](cxx::ObjectFile& f) { f.only_on({...}); })`, a small framework addition: an object file can carry variant gating, and a target skips
  its disabled objects. One-line differences (`getpid`, a thread name) use `#ifdef _WIN32` in place.
- **Runtime DLLs are copied next to the programs** by a `runtime-dlls` tool edge (`cmake -E copy`): `SDL3.dll`, OpenUSD's DLLs and its
  `usd/` plugin folder, `tbb12.dll`. Windows has no rpath; copying keeps the binaries runnable from any directory without PATH edits.
- **An `os` library** (`src/os/`, decided during implementation): process id, process alive, own executable path, resident memory, thread names,
  stop signals and child processes were each needed by several libraries and programs; one header with `oslinux.cpp` / `oswindows.cpp` replaced
  `#ifdef`s in a dozen files. `profile` links it, so every program does. The RPC sockets and the file watcher keep their OS split in their own
  library (`rpcsocketlinux.cpp` / `rpcsocketwindows.cpp`, `filewatcherlinux.cpp` / `filewatcherwindows.cpp`).
- **`ngen-cli` on Windows** (decided during implementation): no exec, so forwarded tools run as children and the cli returns their exit code; the
  root `ngen-cli.exe` is a copy, refreshed by `set` (a symlink needs administrator rights or Developer Mode); and a running cli moves its own file
  aside while a build runs, since Windows cannot overwrite a running program.

## Steps

1. **Build system on Windows** (`src/build/`).
   - `framework/glob.hpp`: `shell_quote` with Windows rules under `_WIN32`; `capture_tokens` uses `_popen`/`_pclose`.
   - `run/process.hpp`: `CreateProcessW` with an inherited anonymous pipe for combined stdout/stderr, `WaitForSingleObject` + `GetExitCodeProcess`,
     `TerminateProcess` for `signal`.
   - `run/hash.hpp`: stat via `std::filesystem` (`last_write_time`, `file_size`) on Windows (no `st_mtim`).
   - `run/progress.hpp`, `run/scheduler.hpp`: `_isatty`, `SetConsoleCtrlHandler` for Ctrl+C; enable VT processing for colour.
   - `bootstrap.cpp`: run the graph and runner stages through `Process` instead of `std::system`, with a host-executable helper
     (`_out/ngen-build-graph.exe`); self-build uses `clang++` on Windows; `-v` sets `TERM=dumb` in the environment instead of a shell prefix.
   - `framework/cxx/toolchain.hpp`: `archive_groups(bool)` (lld-link resolves archives in any order and rejects `--start-group`) and `rpaths(bool)`.
   - `framework/cxx/objectfile.hpp`: `only_on` / `except_on`; `ir/emit.hpp`: skip disabled objects when gathering a target's objects.
2. **`build.cpp`**: the `windows-vulkan` platform — `clang++`, `llvm-ar`, `-fuse-ld=lld`, `-fms-runtime-lib=dll`, `.exe`, Vulkan SDK include and lib paths
   from `VULKAN_SDK`, SDL3 from `external/sdl3_build`, defines `NGEN_PLATFORM_WINDOWS`, `_CRT_SECURE_NO_WARNINGS`, `NOMINMAX`, `WIN32_LEAN_AND_MEAN`; system
   libs `ws2_32` etc. OpenUSD link names (`usd_usd.lib`, `tbb12.lib`) per platform; the `runtime-dlls` tool; per-source gating.
   - `run/depfile.hpp`: a backslash escapes only a space, a tab or `#` (clang writes Windows paths with `\`; reading every `\` as an escape made each
     compile edge dirty on every run).
3. **RPC sockets** (`src/rpc/core/`): `rpcsocket.h` grows a socket handle type and the operations the server and client use (accept, send, receive,
   would-block, poll, a waker that replaces `eventfd`); `rpcsocketlinux.cpp` / `rpcsocketwindows.cpp` (Winsock, `WSAPoll`, a loopback socket pair as the
   waker, sockets not inherited by children). The server polls a listener only when it has one (`WSAPoll` rejects an invalid socket).
4. **`src/os/`**: `os::runProcess` (the asset server's packers, `glslc` in `ngen-packer-shader`, `ngen-build -l` in the cli),
   `os::runProcessInteractive` (the cli's tools), `processId`, `processAlive`, `executablePath`, `residentBytes`, `setThreadName`,
   `blockStopSignals` / `waitForStopSignal`.
5. **The rest**: file watcher on Windows (poll-only), `packercopy` (`std::filesystem::copy_file`), RenderDoc (`GetModuleHandle` /
   `LoadLibrary("renderdoc.dll")`), `localtime_s`, `open_memstream` → `tmpfile` in `viewdumps.cpp`, the calibrated GPU clock against
   `VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER` (what `steady_clock` reads on Windows; `CLOCK_MONOTONIC` is not a Windows domain).
6. **README**: "Building on Windows" under Build (winget for LLVM, CMake, Python, the VS Build Tools and the Vulkan SDK; SDL3 and OpenUSD builds;
   bootstrap with `clang++`; what differs). `src/build/README.md`: Windows command execution and quoting, per-source gating, the toolchain knobs,
   depfile backslashes. `src/os/README.md`, AGENTS.md's folder list and Build section, `REFERENCE.md`, `src/profile/README.md`, `src/asset/README.md`.

## Visibility

None added. The port changes no record, RPC method, script verb or window: every existing one is meant to behave the same on Windows, and the
verification below reads them to show that. The platform is already visible where it matters: `ngen-cli set` prints the variant, the build directory
names it, and the asset server's start-up line says whether it watches through inotify or polls (now from the watcher, not the flag). One reading
is Linux-only and reads as absent on Windows: the driver's GPU busy percentage (sysfs) in the `profile` record.

## Verification

- `clang++ -std=c++23 -o _out/ngen-build.exe src/build/bootstrap.cpp` succeeds, and `./_out/ngen-build -p windows-vulkan -c debug` builds every default
  target with no failed edge; a second run reports nothing to do.
- The RHI examples run with `--check --validation` and exit 0.
- `ngen-asset-server` starts; `ngen-view` with `assets/three_cubes.usda` and a script that records `render` runs headless and exits 0; the trace
  (`ngen-cli introspect trace --until-exit=view`) has no warning or error; the records show a non-zero draw count.
- `ngen-introspect list` sees both processes.
- `linux-vulkan` still builds (not verifiable on this machine; checked by review that every Linux path is unchanged).

### Results (Windows 11, clang 23.1.5, NVIDIA RTX 5090, driver 616.92, Vulkan SDK 1.4.363.0)

- Bootstrap compiles; `ngen-build -p windows-vulkan -c debug` builds all 388 edges, a second run does nothing. `-c release` builds all 388.
- All 17 RHI examples exit 0 with `--frames=10 --check --validation` (in a window: the driver has no `VK_EXT_headless_surface`, so SDL's offscreen
  driver cannot make a surface). `timestamps` failed validation until the calibrated clock used the performance-counter domain.
- `ngen-view assets/three_cubes.usda --frames=200 --fail-on-validation` with a recording script exits 0; the trace has 130 events from the view and
  the asset server, none above info, `FirstFramePresented` and `SceneResident` among them; the `render` record shows 20 passes and 4 draws in
  `GeometryPass`, the `culling` record 4 instances drawn. A screenshot shows the three cubes lit and shadowed on their base.
- `ngen-introspect list` reaches the asset server over the Winsock RPC.
- Not yet: Linux.

### Deviations from the steps

- The DLL copy uses `cmake -E copy`, not `copy_if_different`: an output that keeps its old time would look stale to the runner.
- `ngen-packer-shader` runs `glslc` through `os::runProcess` on Linux too (`posix_spawnp`, no shell), replacing `std::system`; `runProcess` on Linux
  became `posix_spawnp` so a bare program name is looked up on `PATH`.
- `ngen-cli` lists variants through `os::runProcess` instead of `popen` on both OSes.

## Deferred / follow-ups

- **`ReadDirectoryChangesW` file watcher** — when edit-and-repack latency on Windows matters (poll-only mode is coarser).
- **`gamerelease` on Windows** — when a shipping build is wanted: `/OPT:REF`, LTO with lld-link, stripped PDBs.
- **Windows CI** — once the port is stable.
- **ADR for the toolchain and CRT choice** — when this plan lands.
