# ngen

## Response Style

When I ask a question about the codebase (e.g. "why is X done this way?", "shouldn't this be Y?"), 
do the following:

1. **Answer the question directly** — explain the reasoning, trade-offs, or history behind the current approach.
2. **If you see a better approach**, describe it briefly and explain the pros/cons.
3. **Do NOT start implementing changes** unless I explicitly say something like "go ahead", "fix it", "implement that", or "make the change".

Questions that start with "Why", "Shouldn't", "Is this correct", "What's the difference", "Does this make sense" 
are analysis requests — treat them as discussion, not action items.

## Working style

I direct decisions up and down the decision tree. Work is a back-and-forth, not a handoff. This applies to
every task, with or without a plan document — the plan format below is one expression of it, not the trigger
for it.

- At genuine decision forks — multiple defensible options, architecture or taste calls, scope changes —
  stop and present the options with trade-offs and a recommendation, then wait for direction. Don't pick
  one silently. Mechanical steps that follow from an already-made decision don't need a check-in; just do them.
- Surface considerations I may not have thought of — risks, alternatives, interactions with other parts of
  the engine — at the decision point, not after the work is done.
- Check that we're still on track: when new information contradicts an assumption the current direction
  rests on, say so and pause rather than silently adapting.
- Don't jump ahead. Ground proposals in the existing project documentation and code — point to the plan,
  doc, or code that supports a direction. If nothing does, flag it as a new assumption and ask.

## Tools

For searching this codebase, prefer the built-in `Grep` and `Glob` tools — they already use ripgrep internally and are the fastest option.

When shelling out via Bash, **never** run these commands:
- `grep`, `egrep`, `fgrep` — use `rg` (ripgrep) instead.
- `find` — use `fd` instead.

This applies to every invocation: standalone, in pipelines (`… | grep foo`), and in compound commands (`cd src && find . -name '*.cpp'`). If you catch yourself typing `grep` or `find`, stop and rewrite with `rg` / `fd`. No exceptions, no "just this once."

## Markdown style

When writing prose-heavy markdown (design docs, READMEs, CONCEPTS, long-form
explanations), hard-wrap lines at ~160 columns rather than one long line per
paragraph. Keep code blocks, tables, and link-heavy lines unwrapped — those
break if wrapped.     

## Plan documents

Feature work starts with a plan in `docs/plan_<topic>.md` (snake_case). One plan per feature; cross-reference
shared work between plans instead of combining them. `docs/README.md` is the index — add new plans to it and
update status annotations there when a plan lands or is superseded. The `new-plan` skill
(`.claude/skills/new-plan/SKILL.md`) covers the file mechanics for creating, landing, and superseding plans.

Every plan has, in order:

1. **Status header** — first line after the title: `**Status. Draft | In progress | Landed | Superseded by <doc>.**`
   Update it when the state changes. When a plan is superseded, keep it and mark it — it is a record of the
   path not taken.
2. **Current state** — short; assume the reader knows the engine, link rather than re-explain.
3. **Scope** — explicit In and Out lists. Deferred items go in a "Deferred / follow-ups" section at the end,
   ideally with the condition that would trigger them.
4. **Steps** — concrete: name the files, sketch the real structs/signatures. Prefer a short sequential step
   list sized to one iteration; use phases only when each phase ships something observable on its own.
5. **Verification** — observable, binary criteria: obs-bus events to check, byte-identical outputs, what a
   test scene should show. "Compiles" is not a criterion.

Decisions in plans: when there are meaningful alternatives, list them with trade-offs and a recommendation
("I lean X because …; pushback welcome"). Lock decisions explicitly with the why; put genuinely unresolved
items in an "Open questions" section instead of picking silently.

## C++ style

- C++23.
- Never compress code for compactness: one statement per line, full braces on every control-flow block, no
  single-line `if (cond) stmt;`, no column-aligned assignments or comments, no multiple statements per line.
  This applies to code sketches in chat and design docs too, not just files — if code looks long when written
  honestly, that's information; don't paper over complexity with formatting.
- Match the surrounding code's idiom, naming, and comment density.
- Mechanical formatting is `./_out/ngen-build -p <platform> -c <config> format`'s job (clang-format) — don't
  hand-format against it.

## File naming
- No snake_case in filenames. Use lowercase concatenated names (e.g. `sceneloader.cpp`, `devicevulkan.h`).
- Platform-specific files put the platform as the last part of the name (e.g. `devicevulkan`, `swapchainvulkan`).
- These rules apply to source files. Docs under `docs/` use snake_case (e.g. `plan_usd_lights.md`).

## Folder structure
- All source code lives under `src/`.
- `src/rhi/` — backend-agnostic RHI interfaces.
- `src/rhi/vulkan/` — Vulkan backend implementation. Additional backends go in sibling folders (e.g. `src/rhi/d3d12/`).
- `src/renderer/` — renderer front-end (render graph, resource management).
- `src/scene/` — scene loading, ECS, materials.
- Cross-cutting files (`main.cpp`, `types.h`, `camera.*`) live directly in `src/`.

## Build

The engine uses its own self-hosted build system (`ngen-build`) — no ninja or make at any stage. Bootstrap
once on a fresh clone (and again whenever `build/bootstrap.cpp` changes):

```sh
mkdir -p _out && c++ -std=c++23 -O0 -g -pthread -o _out/ngen-build build/bootstrap.cpp
```

From then on `./_out/ngen-build` is the only entry point. `--platform`/`-p` and `--config`/`-c` are always
required (the build system has no project-specific defaults):

- `./_out/ngen-build -p linux-vulkan -c debug` — build the default target (`ngen-view`); configs: `debug`, `release`, `gamerelease`
- `./_out/ngen-build -p linux-vulkan -c debug format` — clang-format the tree
- `./_out/ngen-build --compile-commands -p linux-vulkan -c debug` — refresh `compile_commands.json` (opt-in; re-run when the project graph changes)
- `./_out/ngen-build -h` — full flag list (clean, rebuild, tidy, list, graph dumps, fuzzy target matching, …)

The engine binary lands at `_out/linux-vulkan/debug/ngen-view` (or the equivalent under the active config).
See `build/build_system.md` for the framework internals (extension model, IR + emitter, runner / scheduler,
adding platforms/configurations).

## Verifying changes

Verification runs headless through the observation bus — build, run with `--obs-output`, read the JSONL
evidence. The `run-headless` skill (`.claude/skills/run-headless/SKILL.md`) has the full procedure, test
scenes, and machine constraints; `obs.md` documents the observation conventions. Observations added for a
change stay in the code — there is no "remove when done" step.

