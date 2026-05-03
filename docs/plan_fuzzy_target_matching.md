# Plan — fuzzy target matching for `ngen-build`

Goal: let `./_out/ngen-build <query>` resolve to any addressable thing in the project graph by typing a substring of its name. Concretely:

```text
ngen-build renderth   → src/renderer/renderthread.cpp     (single-TU compile)
ngen-build view       → ngen-view                         (default program)
ngen-build rend       → renderer                          (intermediate library)
ngen-build form       → format                            (global tool)
ngen-build rhivu      → rhivulkan                         (intermediate library)
```

Exact-name input keeps working unchanged; fuzzy is only consulted on miss.

## 1. Why this is small now

Phases 1–4 of `plan_per_source_target.md` did the structural work. Every `.cpp` is already a `build::Target` with a unique name; every library, program,
tool, and alias is a `build::Target` with a unique name. The graph already knows every addressable thing — we just need to surface that knowledge to the
shell side of the bootstrap chain. There is no new graph concept, no new wrapper, no emitter logic to add for the resolution itself.

## 2. Manifest emission (graph stage)

`detail::Emitter::emit` already walks `Project::build_all()` × platform × config and writes per-variant `compile_commands.json` plus the merged
`_out/compile_commands.json`. Add a parallel pass that writes a target manifest per variant.

Path: `_out/<platform>/<config>/targets.txt` (mirrors `compile_commands.json` layout). One row per addressable node, three tab-separated columns:

```text
<kind>\t<name>\t<ninja-target>
```

Kinds: `program`, `library`, `alias`, `tool`, `global-tool`, `objectfile`.

Concrete rows for the current `linux-vulkan` × `debug` variant:

```text
program       ngen-view                                                ngen-view:linux-vulkan:debug
library       renderer                                                 _out/linux-vulkan/debug/lib/librenderer.a
library       rhivulkan                                                _out/linux-vulkan/debug/lib/librhivulkan.a
alias         rhi-backend                                              rhi-backend:linux-vulkan:debug
tool          shaders                                                  shaders:linux-vulkan:debug
tool          clean                                                    clean:linux-vulkan:debug
global-tool   format                                                   format
global-tool   tidy                                                     tidy
objectfile    renderer/src/renderer/renderthread.cpp                   _out/linux-vulkan/debug/obj/renderer/src/renderer/renderthread.cpp.o
objectfile    sceneusd/src/scene/usdscene.cpp                          _out/linux-vulkan/debug/obj/sceneusd/src/scene/usdscene.cpp.o
... (~165 ObjectFile rows)
```

Two design points:

- **`<name>` is the build::Target name verbatim** — for ObjectFiles that's the parent-prefixed form (`renderer/src/...`), which is what's actually unique
  in the graph. The fuzzy matcher does substring matching, so `renderth` still hits.
- **`<ninja-target>` is whatever ninja accepts directly.** No further computation in bootstrap. Programs and aliases get the per-variant phony that the
  emitter already writes; libraries (which have no per-variant phony today) get the absolute archive path; ObjectFiles get the absolute object path;
  global tools get the bare phony. Bootstrap hands this column straight to ninja.

The emitter has every input it needs at `emit()` time: it iterates `Project::build_all()`, knows each node's kind via its extensions, and the per-variant
output path is already cached in `outputs_` keyed by `name|platform|config`. Manifest emission is a 30-line addition next to `write_compile_commands()`.

## 3. Bootstrap resolver

Order in `bootstrap.cpp::main` becomes:

1. Parse args (unchanged).
2. Run prebuild + ngen-build-graph (unchanged) → manifest now exists.
3. **New**: if `args.target` doesn't match a manifest `<name>` exactly, run the fuzzy resolver against the manifest for the active variant. On success
   replace `args.target` with the resolved `<ninja-target>`. On miss fall through to ninja unchanged (preserves the current "ninja error if name is bogus"
   behavior).
4. Invoke ninja with the resolved target.

`ninja_target(args)` collapses: today it appends `:platform:config` for non-special targets. With the manifest, the third column already carries the
correct form, so the helper just returns `args.target` after resolution. The `format`/`tidy`/`clean` special-case disappears — they're rows in the manifest
like everything else.

## 4. Match strategy

Resolution against the manifest, cheapest first:

1. **Exact** match on `<name>` → done.
2. **Substring** match (case-insensitive) on `<name>`. Score every hit with this tier-and-tiebreak:

   | Tier (priority desc.) | What it matches                                              |
   | --------------------- | ------------------------------------------------------------ |
   | Filename stem         | OF rows, filename without `.cpp` (`renderthread`)            |
   | Top-level name        | program / global-tool / alias `<name>`                       |
   | Library name          | library `<name>`                                             |
   | Path component        | OF rows, last directory component (`renderer`)               |
   | Full name             | anything else                                                |

   Tiebreak inside a tier: `len(candidate) - len(query)` ascending (closer fit wins).

3. **Single best**: print `→ matched <name>` to stderr, hand `<ninja-target>` to ninja.
4. **Tied or close runners-up**: print top 5 with their kinds, exit non-zero, ask for a longer prefix. No interactive disambiguation in v1.
5. **No matches**: pass `args.target` through verbatim. Ninja produces its own "unknown target" error.

Why this scoring rather than something simpler: typing `renderer` should clearly resolve to the library, not to one of the ~30 OFs that contain
`renderer/` in their name. Tiering "library name" above "path component" handles that without a special case. Typing `renderth` has no library or
top-level hit, so it falls through to filename-stem and resolves cleanly to the OF.

Subsequence (gap-tolerant) matching is intentionally not in v1 — substring is enough for every realistic example and predictable to reason about.

## 5. UX details

- **Stderr line** on resolution: `ngen-build: matched 'renderth' → src/renderer/renderthread.cpp` (or whatever; one line, terse). Goes to stderr so it
  doesn't pollute scripts that capture stdout.
- **Ambiguity output**: list at most 5 candidates, one per line, with kind. Exit code 2 (distinct from ninja's failure modes).
- **`--list` unchanged**: the existing graph-stage `--list` already shows roots. Adding `--list --all` to dump the full manifest is an obvious follow-up
  but not in scope.
- **Verbosity**: `-v` / `-vv` are passed through to ninja as today. Resolver output is independent of these flags.

## 6. What stays the same

- Exact-name invocations: byte-identical behaviour.
- All current CLI flags (`--platform`, `--config`, `--backend`, `--list`, `-v`/`-vv`).
- `_out/build.ninja` content — fuzzy matching is a bootstrap-side concern that consumes the manifest, not a graph-side rewrite.
- `compile_commands.json` shape and emission.
- The bootstrap chain stages and their dependency tracking.

## 7. Edge cases worth calling out in the plan, deferring in v1

- **Manifest absent on first run.** If the graph stage failed, the manifest doesn't exist. Resolver detects this and falls through to ninja — same as
  the no-match path. Ninja then errors out on the missing build.ninja, which surfaces the underlying problem.
- **Same source compiled into two libraries.** Two OF rows with different `<name>` (parent prefix differs), same source path. Substring match on the
  source filename hits both → ambiguity prompt, user picks. Correct behaviour.
- **`clean` and friends become per-variant.** Today bootstrap special-cases `clean`/`format`/`tidy` to skip `:platform:config`. With the manifest, `clean`
  is a per-variant tool (it gets `clean:linux-vulkan:debug` from the manifest); `format` and `tidy` are global-tools and get the bare name. Net behaviour
  is the same — `clean` is still platform/config-scoped today, just opaquely so. Confirm in implementation.
- **Aliases.** `rhi-backend` resolves at emit time to `rhivulkan`; the manifest emits the alias's own per-variant phony as the ninja target. Invoking
  `rhi-backend:linux-vulkan:debug` already works today and continues to.
- **Soft-fail on missing OF.** Out of scope — `for_source` already aborts on missing sources at config time, so the graph can't have ghost OFs.

## 8. Implementation sketch (rough sizing)

- `build/framework/backendninja.hpp` — add a `write_targets_manifest()` method on `detail::Emitter` analogous to `write_compile_commands()`. Iterates
  `Project::build_all()` × platform × config, emits one row per node by inspecting which extension is attached. ~50 lines.
- `build/bootstrap.cpp` — load the per-variant manifest, run the resolver, rewrite `args.target`. ~100 lines (parsing + matcher + UX). Stays
  framework-free as today.
- `build/build.cpp` — no change.
- `build_system.md` — add a §11.x sub-section under CLI describing fuzzy resolution and the manifest. Update §13 with a note that the graph stage now
  also emits a target manifest per variant.

## 9. Why not a phase 5 of the per-source plan

Fuzzy matching is a *user-interface* feature on top of the graph; it doesn't change the graph or the emitter beyond writing a manifest file. The
per-source plan was about graph shape. Keeping these separate means the per-source work is complete on its own merits (per-TU compile_flags, std,
warning_off — all already useful) and fuzzy matching can land later or never without leaving anything half-finished.
