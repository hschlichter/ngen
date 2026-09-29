# Program entry points in src/apps/

**Status. Landed.**

Groundwork for the tools of [plan_tool_architecture.md](plan_tool_architecture.md). Every program gets its `main` in one place, and the windowed tools
share one application shell.

## Current state

- **`ngen-view`'s main** is `src/main.cpp`: about 1,200 lines holding the frame loop, SDL event handling, the session verbs (`applyCommand`), the
  gizmos, picking and dump handling. It sits directly in `src/` as a "cross-cutting file" (`AGENTS.md`, folder structure).
- **`ngen-cli`'s main** is `src/cli/cli.cpp`.
- **`ngen-build`'s main** is the bootstrap, `build/bootstrap.cpp`, which becomes `src/build/bootstrap.cpp`
  ([plan_build_into_src.md](plan_build_into_src.md)). Its helpers have their mains in the build framework (`run/main.cpp`, and later the server's).
- **The RHI examples** each have a main in `src/rhi/examples/`.
- **Planned programs:**
  - `ngen-rpc` ([plan_rpc.md](plan_rpc.md))
  - `ngen-editor` ([plan_editor_split.md](plan_editor_split.md))
  - `ngen-introspect` (step 4)
  - the packers `ngen-packer-shader`, `ngen-packer-usd`, `ngen-packer-texture` and `ngen-packer-assemble` ([plan_pack_rules.md](plan_pack_rules.md))

## Scope

**In**

- **The rule:** every program's `main` lives in `src/apps/<name>.cpp`, where `<name>` is the program name without `ngen-` and without dashes:

  | Program | Entry point |
  |---|---|
  | `ngen-view` | `src/apps/view.cpp` |
  | `ngen-cli` | `src/apps/cli.cpp` |
  | `ngen-rpc` | `src/apps/rpc.cpp` |
  | `ngen-editor` | `src/apps/editor.cpp` |
  | `ngen-introspect` | `src/apps/introspect.cpp` |
  | `ngen-packer-shader`, `-usd`, `-texture`, `-assemble` | `src/apps/packershader.cpp`, `packerusd.cpp`, `packertexture.cpp`, `packerassemble.cpp` |

- **`src/apps/tool/`**, the shared application shell for windowed tools: the SDL window, the RHI device and swapchain, the ImGui backend, the RPC
  endpoint and the frame loop. `ngen-editor` and `ngen-introspect` are built on it. It has no renderer: neither tool draws a 3D scene.
- **An entry point stays thin.** `src/apps/<name>.cpp` parses arguments, wires libraries together and runs the loop. Logic that another program or
  a test could use goes into a library. For ngen-view, the session commands move to `src/session/sessioncommands.*` ([plan_rpc.md](plan_rpc.md),
  step 4), and the gizmo and picking code moves next to the runtime scene ([plan_editor_split.md](plan_editor_split.md)).
- **Now:** move `src/main.cpp` to `src/apps/view.cpp` and `src/cli/cli.cpp` to `src/apps/cli.cpp`, and update `build.cpp`, `AGENTS.md` (folder
  structure) and the READMEs. New programs are created in `src/apps/` from the start.

**Out**

- **`ngen-build` and its helpers.** They keep their mains in `src/build/`, so the build framework stays self-contained and can be lifted into
  another project ([plan_build_into_src.md](plan_build_into_src.md), Decision 1).
- **The RHI examples.** They stay in `src/rhi/examples/`, as the RHI's own verification programs next to the code they check
  (`src/rhi/README.md`, "Examples").
- **Splitting `view.cpp` up.** Moving logic out of it happens in the plans that change that logic, not here.
- **ngen-view on the tool shell.** The view has a renderer and its own frame pacing. Whether it adopts `src/apps/tool/` too is left for when the
  shell exists.

## Decisions

1. **Name without prefix or dashes**, following the file naming rule (lowercase, concatenated). The alternative, keeping the full program name as the
   file name (`ngen-view.cpp`), breaks the no-dashes rule.
2. **The two exceptions above** (the build system and the RHI examples) keep programs next to the library they belong to, where that locality is the
   point. Pushback welcome if you want them in `src/apps/` too.
3. **One shared shell folder, `src/apps/tool/`**, not one library per tool. The editor and the introspection tool differ in their windows and
   methods, not in the shell.

## Steps

1. Henrik moves the files, since they are git operations:
   - `git mv src/main.cpp src/apps/view.cpp`
   - `git mv src/cli/cli.cpp src/apps/cli.cpp`
2. `build.cpp`: the `ngen-view` and `ngen-cli` sources, and `src/apps` on the include path where needed.
3. `AGENTS.md`, folder structure: `src/apps/` for program entry points (the rule and the two exceptions), and `src/apps/tool/` for the shared
   shell. `main.cpp` comes off the cross-cutting list.
4. `README.md`: the architecture overview's "App (main.cpp)" becomes `src/apps/view.cpp`. The ngen-cli section points at `src/apps/cli.cpp`.
5. Update the plans that name entry points: [plan_rpc.md](plan_rpc.md) (`ngen-rpc`), [plan_pack_rules.md](plan_pack_rules.md) (the packers) and
   [plan_editor_split.md](plan_editor_split.md) (the editor and the shell).

## Verification

- `./ngen-cli build`, `./ngen-cli build ngen-cli` and `./ngen-cli build examples` succeed, and `./ngen-cli` still works after the rebuild.
- The six headless screenshots are byte-identical to before the move.
- `fd -e cpp . src | rg -v '^src/(apps|build|rhi/examples)/' | xargs rg -l '^(auto|int) main\('` finds no `main` outside the allowed places.

## Results

- `git mv src/main.cpp src/apps/view.cpp` and `git mv src/cli/cli.cpp src/apps/cli.cpp`; `build.cpp`'s `ngen-view` and `ngen-cli` sources point at
  them. Both build, and `./ngen-cli` works after rebuilding itself.
- `AGENTS.md`'s folder structure has the `src/apps/` rule, with its two exceptions, plus `src/build/`. `main.cpp` is off the cross-cutting list.
  `README.md`'s architecture overview and ngen-cli section use the new paths.
- The six headless screenshots are byte-identical to the baseline.
- `src/apps/tool/` doesn't exist yet; the editor or the introspection tool creates it.
