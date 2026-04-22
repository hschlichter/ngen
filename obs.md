# Observability — Usage Reference

## What this gives you

The engine ships a streaming observation bus. Code emits structured events via `OBS_EVENT`; a writer thread streams them to a JSON Lines file. You read the file to confirm what the engine actually did on a given run.

Three things you do:

1. **Add observations** where a decision or state change should be visible.
2. **Run the engine** with `--obs-output=path.jsonl`.
3. **Read the file** with `jq` or any line-by-line tool.

Observations stay in the code. There's no "remove when done" step — they document intent and will narrate behavior the next time anything near that code runs.

## Emitting an observation

```cpp
#include "observationmacros.h"

OBS_EVENT("Scene", "PrimVisibilityChanged", "/World/Cube")
    .field("visible", true);
```

`OBS_EVENT(category, type, name)` returns a fluent builder; chain `.field(key, value)` as many times as needed; the event is submitted at end-of-statement.

| Argument | Rule |
|---|---|
| `category` | One of the canonical set (below). Currently: `"Scene"`, `"Render"`, `"Engine"`. |
| `type` | CamelCase verb phrase: `PassExecuted`, `LayerMuted`, `JobSubmitted`. |
| `name` | Stable identifier for the subject: USD prim path, layer path, pass name, job id. **Never** a pointer, handle, or ephemeral index. |

Canonical categories:

| Category | Covers |
|---|---|
| `"Scene"` | USD scene edits, layer ops, SceneUpdater executions, undo/redo, scene lifecycle (open/save/new) |
| `"Render"` | Frame lifecycle, frame graph compile/execute, pass execution and culling, swapchain changes, render-world upload, mesh/texture cache misses |
| `"Engine"` | Cross-cutting infrastructure — JobSystem submit/complete, bus lifecycle |

Add a new category only when nothing existing fits.

## Field conventions

`.field(key, value)` accepts ints, floats, bools, string literals, `std::string`, `std::string_view`. Keys are snake_case string literals.

- **Pure arguments only.** The whole chain is skipped when the category is silenced or observability is compiled out (`-DOBSERVABILITY_DISABLED`). Don't rely on side effects in `.field()` arguments — same rule as `assert()`.
- **Stable values.** Pointers, handles, thread-ids-as-numbers, OS resource IDs → ephemeral, don't emit. Prim paths, resource names, counts, format strings, hashes → stable, emit freely. If two runs of the same scene produce different field values, they can't be diffed.
- **Many simple fields, not one compound.** `.field("width", 2048).field("height", 2048)` beats `.field("size", "2048x2048")`.

## Patterns

System execution narration:

```cpp
OBS_EVENT("Scene", "SystemExecuted", "SceneUpdater")
    .field("edits", count)
    .field("path", fast ? "fast" : "async");
```

State transition:

```cpp
OBS_EVENT("Scene", "LayerMuted", layerPath)
    .field("by", "user");  // or "cli", "internal"
```

Decision rationale (for culled / skipped work):

```cpp
OBS_EVENT("Render", "PassCulled", pass.name)
    .field("reason", "no downstream reads");
```

## Running and capturing

```
$ ./ngen --obs-output=./obs.jsonl assets/scene.usda
```

**If `--obs-output` is not provided, observability is entirely off.** No sink is installed, no writer thread starts, and every `OBS_EVENT` short-circuits to a no-op. The engine runs at zero observability cost. You must pass the flag to get any output at all.

Optional filters (mutually exclusive, only meaningful when `--obs-output` is given):

- `--obs-only=Scene,Engine` — only these categories emit.
- `--obs-exclude=Render` — everything except these.

Default when `--obs-output` is given: all canonical categories emit. The output file is written incrementally; you can `tail -f` it while the engine runs.

## Reading a dump

Each non-metadata line is one observation. The first line is a metadata record:

```
{"schema":"ngen.obs","schema_version":1,"started_at_ns":1713700800000000000}
```

Skip it or filter on `.ts_ns`. Common `jq` recipes:

```bash
# Every Scene observation
jq -c 'select(.category == "Scene")' obs.jsonl

# Everything about /World/Cube
jq -c 'select(.name == "/World/Cube")' obs.jsonl

# All PassExecuted events, as a table
jq -r 'select(.type == "PassExecuted") | [.name, .ts_ns] | @tsv' obs.jsonl

# Observations from one frame (if the emitter included the field)
jq -c 'select(.fields.frame == 120)' obs.jsonl

# Count emissions by type
jq -r 'select(.ts_ns) | .type' obs.jsonl | sort | uniq -c | sort -rn
```

## Troubleshooting

**My observation didn't appear.** Check, in order:

1. `--obs-output` not passed? Without the flag, the bus has no sink and every `OBS_EVENT` is a no-op — no file is produced at all.
2. Category silenced? Look at `--obs-only` / `--obs-exclude` on the command line.
3. Compiled out? Look for `-DOBSERVABILITY_DISABLED` in the build.
4. Emission site not reached? The branch your `OBS_EVENT` is in didn't execute — add an observation higher up in the path to narrow it down.
5. Crashed before flush? Up to ~50ms of in-flight observations are lost on a crash — the writer thread flushes the sink periodically, not per-event.
6. `bus.shutdown()` not called? The tail of the stream may not be flushed.

**File is empty or suspiciously short.** `bus.shutdown()` likely not reached before process exit. Fix in `main()`.

**Values vary across runs.** Something ephemeral got into a field — a pointer, a handle index, a non-deterministic ID. Replace with a stable identifier.

**Observation shape changed unexpectedly.** Check that `schema_version` in the file's first line matches what your reader expects.

## Don'ts

- Don't emit from per-draw, per-vertex, or per-entity inner loops. Pick the wrapping decision or state transition instead.
- Don't put expensive computations inside `.field()`. They run on the hot path and their results are serialized and discarded.
- Don't invent categories casually. If nothing in the canonical list above fits, add one there first so the vocabulary stays predictable.
- Don't remove observations because you're "done with them." They stay permanently — that's the point.
