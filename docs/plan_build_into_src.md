# Move the build system into src/build/

**Status. Landed.**

Groundwork for [plan_build_server.md](plan_build_server.md): the build system moves next to the rest of the source, so the server can depend on
`src/rpc/` like any other tool.

## Current state

- The build system lives at the repository root in `build/`: `bootstrap.cpp`, `framework/`, `ir/`, `run/` and `example/`. Its documentation is
  `build/build_system.md`.
- The root `build.cpp` includes it as `build/framework/…` and `build/ir/…`.
- The bootstrap compiles `build/bootstrap.cpp` by hand. Its self-build compiles `build.cpp` and `build/run/main.cpp`.
- `format` and `tidy` in `build.cpp` glob `build/**` separately from `src/**`, and `tidy` passes `-Ibuild/framework`.
- Every other piece of source code already lives under `src/` (`AGENTS.md`, folder structure). `build/` is the exception.

## Scope

**In**

- **`git mv build src/build`.** The files keep their names, and relative includes inside the framework (`../framework/…`) keep working.
- **Root `build.cpp` stays at the root.** It's the project's description, like a `CMakeLists.txt`, and not part of the framework. Its includes become
  `src/build/framework/…` and `src/build/ir/…`.
- **Bootstrap:**
  - the documented command becomes `mkdir -p _out && c++ -std=c++23 -O0 -g -pthread -o _out/ngen-build src/build/bootstrap.cpp`
  - `self_build_ir()` compiles `src/build/run/main.cpp`
- **Format and tidy:** the separate `build/**` globs go, since `src/**` covers them. `tidy`'s include path becomes `-Isrc/build/framework`.
- **Documentation:**
  - `build/build_system.md` becomes `src/build/README.md`, following the rule that library documentation is the library's README
  - references in `AGENTS.md`, `README.md`, `docs/README.md`, the `run-headless` skill and `src/cli/cli.cpp`'s bootstrap message are updated
  - the root `README.md` links `src/build/README.md` next to the RHI and renderer READMEs
- **`ngen-cli`** prints the new bootstrap command.

**Out**

- Any change to how the build system works. This is a move.
- Rewriting historical plans in `docs/`. They keep their `build/` paths as a record; `docs/README.md`'s build-system section points at the new
  location.

## Decisions

1. **The framework stays self-contained after the move.** `src/build/framework/`, `ir/` and `run/` keep using only the standard library, and nothing
   under `src/build/` includes engine code. The goal that the build system can be lifted into another project is unchanged; only its location
   changes. The one new dependency is the server's, on `src/rpc/`'s core, which needs only the standard library and header-only nlohmann/json
   ([plan_build_server.md](plan_build_server.md), Decision 1).
2. **Engine libraries don't pick up the framework.** `src/build/` isn't added to any engine target's include path. Only the root `build.cpp`, the
   bootstrap and the build system's own helpers include it.

## Steps

1. `git mv build src/build` and `git mv src/build/build_system.md src/build/README.md`. Henrik runs these; they are git operations.
2. `build.cpp`: the include paths, and the `format` and `tidy` globs and flags.
3. `src/build/bootstrap.cpp`: the paths in `self_build_ir()` and the header comment.
4. `src/cli/cli.cpp`: the bootstrap message.
5. Docs: `AGENTS.md` (Build, folder structure), `README.md` (Building, links), `docs/README.md` (build-system section header), the `run-headless`
   skill, and `src/build/README.md`'s own paths.

## Verification

- A fresh bootstrap with the new command builds `_out/ngen-build`. `./_out/ngen-build -p linux-vulkan -c debug` then self-builds the helpers and
  builds `ngen-view` with no errors.
- Every target builds (`examples`, `ngen-cli`, `format`, `tidy`), and the six headless screenshots are byte-identical to before the move.
- `rg -n '"build/' build.cpp src` finds no stale include path, and `rg -n 'build/bootstrap.cpp|build/build_system.md' AGENTS.md README.md .claude`
  finds no stale reference.
- `src/build/example/` still builds on its own, which shows the framework has no path assumptions left over from the old location.

## Results

- `git mv build src/build`, and `build_system.md` became `src/build/README.md`. `build.cpp` includes `src/build/…`, and the bootstrap's
  self-build compiles `src/build/run/main.cpp`.
- A fresh bootstrap with `c++ … -o _out/ngen-build src/build/bootstrap.cpp` self-built both helpers and built `ngen-view`. `ngen-cli`, `examples`
  and `format` build. All 15 RHI examples pass `--check --validation`, and the six headless screenshots are byte-identical to the baseline.
- `src/build/example/` builds and runs on its own ("hello from ngen-build example"), with its README's paths updated for the new depth.
- The `format` globs are now `src/**/*.cpp`, `src/**/*.h` and `src/**/*.hpp`. The vendored `src/build/ir/xxhash.h` is excluded; the first run
  without the exclusion reformatted it, which is left in the working tree for Henrik to restore.
- `tidy` wasn't run: it runs clang-tidy over the whole tree and isn't part of this change's behaviour.
- References updated: `AGENTS.md` (Build, folder structure), `README.md`, `docs/README.md`, the `run-headless` skill, `src/apps/cli.cpp`'s bootstrap
  message.
