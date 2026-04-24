# ngen

## Response Style

When I ask a question about the codebase (e.g. "why is X done this way?", "shouldn't this be Y?"), 
do the following:

1. **Answer the question directly** — explain the reasoning, trade-offs, or history behind the current approach.
2. **If you see a better approach**, describe it briefly and explain the pros/cons.
3. **Do NOT start implementing changes** unless I explicitly say something like "go ahead", "fix it", "implement that", or "make the change".

Questions that start with "Why", "Shouldn't", "Is this correct", "What's the difference", "Does this make sense" 
are analysis requests — treat them as discussion, not action items.

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

## File naming
- No snake_case in filenames. Use lowercase concatenated names (e.g. `sceneloader.cpp`, `devicevulkan.h`).
- Platform-specific files put the platform as the last part of the name (e.g. `devicevulkan`, `swapchainvulkan`).

## Folder structure
- All source code lives under `src/`.
- `src/rhi/` — backend-agnostic RHI interfaces.
- `src/rhi/vulkan/` — Vulkan backend implementation. Additional backends go in sibling folders (e.g. `src/rhi/d3d12/`).
- `src/renderer/` — renderer front-end (render graph, resource management).
- `src/scene/` — scene loading, ECS, materials.
- Cross-cutting files (`main.cpp`, `types.h`, `camera.*`) live directly in `src/`.

## Build
Use 'make' to build.
Use 'bear -- make' to generate compile_commands.json.
Use 'make format' for formatting the code.

