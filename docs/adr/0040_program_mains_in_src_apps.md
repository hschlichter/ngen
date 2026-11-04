# 40. Every program's `main` in `src/apps/`

Date: 2026-09-29

## Status

Accepted

## Context

ngen-view's entry point was a 1,200-line `src/main.cpp` holding argument parsing, wiring, the frame loop and a good deal of logic that belonged to
libraries. The tool architecture ([ADR 34](0034_process_split.md)) was about to add many programs: an asset server, packers, an introspection
tool, check tools. Without a rule each would put its `main` somewhere different and grow the same way.

## Decision

- Every program's `main` lives in `src/apps/<name>.cpp`, named after the program without `ngen-` and without dashes: `view.cpp` for `ngen-view`,
  `assetserver.cpp` for `ngen-asset-server`, `packertexture.cpp` for `ngen-packer-texture`.
- Entry points stay thin: argument parsing, wiring and the loop. Reusable logic goes into a library.
- Exceptions: `ngen-build` and its helpers keep their mains in `src/build/`, because the build system must stay self-contained
  ([ADR 32](0032_build_system_in_src_std_only.md)), and the RHI examples stay in `src/rhi/examples/`, because their locality is the point.
- A shared shell for the windowed tools was to live in `src/apps/tool/`.

## Consequences

- Easier: every program is found in one folder, and a large `main` is visibly out of place.
- Easier: logic gets pushed into libraries; the later lean pass moved the view's commands and frame loop out of `view.cpp`
  ([ADR 76](0076_viewframe_steps_and_hooks.md)).
- Harder: the shared windowed-tool shell was never made, so each windowed tool sets up its own window.

## Verification

Checked against `d83048c` on 2026-11-04.

- Sources: [plan_apps_folder.md](../plan_apps_folder.md), `AGENTS.md` ("Folder structure").
- Code: `src/apps/` holds `assetserver.cpp`, `cli.cpp`, `introspect.cpp`, `pack.cpp`, `packdump.cpp`, the four `packer*.cpp`, `usdcheck.cpp`,
  `view.cpp` and `worldcheck.cpp`; `build.cpp:332` onwards builds each program from its file. `src/apps/view.cpp` is 566 lines.
- Differs from plan: `src/apps/tool/` does not exist. `ngen-introspect` was built in `src/introspect/` without a shared shell, and no other
  windowed tool exists yet. `AGENTS.md` still says `src/apps/tool/` holds the shared shell.
- Rejected alternatives: file names keeping the full program name (breaks the no-dashes rule); one library per tool shell; moving `ngen-build`
  and the examples into `src/apps/`.
