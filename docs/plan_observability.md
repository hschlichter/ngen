# Observability Layer — Implementation Plan

Iterable design doc. Nothing here is final; push back on anything.

Companion to [`observability_api_design_engine_agnostic.md`](observability_api_design_engine_agnostic.md), which defines the *abstract* API. This doc maps that design onto the ngen engine and proposes a concrete rollout.

> **AI/Claude readers:** §9 is a standalone usage reference. Skip ahead if you just need to know how to add observations and read the output.

---

## 1. Why now

The engine has grown past the point where "run it and eyeball it" is a workable feedback loop:

- Multi-threaded with async work — bugs surface as subtle state divergences rather than crashes.
- USD is the scene system — edits go through a fast/slow-path updater with async jobs. "Did my edit land?" is not trivially observable.
- No logging infrastructure exists (just scattered `std::println`). No central sink.

We need a structured introspection layer so changes can be confirmed by reading evidence rather than by re-reading diffs. The primary consumer is **Claude** (or another AI agent) working on the codebase: Claude adds observations as part of a change, runs the engine, reads the observation stream, and reasons about whether the intended behavior actually happened. No verifier, no predicate DSL — Claude *is* the verifier, and the artifact is its evidence.

The abstract design's `Observation` is the right core shape as a *starting point*. We diverge from it in three ways spelled out below: `ExecutionSummary` and `ResourceState` collapse into ordinary observations (§3), any notion of a grouping container is dropped — observations are a flat time-ordered stream (§3), and the `Verifier` / `ExpectationSpec` layer is dropped (§8). What remains is a small thread-safe bus that streams each observation to a sink (by default a JSON Lines file) as it happens — no retained buffer.

---

## 2. Engine-specific findings that shape the design

Findings from the code review that change how the abstract API should be implemented here:

1. **Scene is USD, not ECS.** There is no `Entity` type. Authoritative identifiers are USD prim paths (`/World/Cube/Mesh`). Observations about scene objects must use prim paths, not synthesized IDs. `PrimHandle` indices are ephemeral — do not put them in observations.

2. **Multiple producer threads.** The main thread, job workers, and other background threads all emit. Emission must be thread-safe and non-blocking — no contention mutex on a hot path.

3. **No debug/release split in the build.** `Makefile` always uses `-O0 -g`. We don't have `#ifdef NDEBUG` to gate on. Zero-overhead-when-disabled needs an explicit `-DOBSERVABILITY_DISABLED` or a runtime flag — decided below.

4. **No logging lib vendored.** We're not replacing spdlog/fmt; we're defining the first structured sink. The bus does not double as a human-readable logger (see §8), but it does mean we have no existing infrastructure to conform to — greenfield.

---

## 3. Architecture mapping

How the abstract types land on real files.

### 3.0 One stream, flat observations

The abstract design has three observation types (`Observation`, `ExecutionSummary`, `ResourceState`) grouped into a per-batch container. We collapse it all.

**One type: `Observation`.** `ExecutionSummary`, `ResourceState`, and any other shape become conventions for what goes in the `fields` payload:

- "System X ran with N items" → `OBS_EVENT("Scene", "SystemExecuted", "SceneUpdater").field("edits", N)`
- "Prim X was transformed" → `OBS_EVENT("Scene", "PrimTransformed", "/World/Cube").field("path", "fast")`
- "A state value changed" → `OBS_EVENT("Scene", "LayerMuted", "/layers/debug.usda").field("by", "user")`

**No containers, no batches.** There is no grouping type, no scope API, no implicit boundaries. The bus stores a single flat time-ordered stream. If a subsystem wants to scope observations (by job, by transaction, by tick, by whatever) it encodes that scope as a regular field — `"job_id": "..."`, `"txn": 3` — and emits ordinary scoping observations (`TransactionBegin`, `TransactionEnd`, etc.) as needed.

Rationale: different subsystems have different notions of scope. A scene editor thinks in edits and transactions; a background job thinks in job IDs; an input system is continuous. Baking any one scope into the bus makes participating more expensive than it needs to be. The universal primitive is "a subsystem emits a timestamped fact." Every other concept is domain data, not bus machinery.

**Streaming, not buffering.** The bus does not retain observations beyond the moment it takes to hand them off. `emit()` is a lock-free enqueue onto an MPSC queue; a dedicated writer thread dequeues and writes to the configured sink. Producer threads (main, render, job workers) never touch disk or take a contended lock. Memory footprint is the queue's pending items (typically near-empty) plus the sink's I/O buffer (~8KB for a buffered file stream). Full threading details in §3.3.

Bus API:

```cpp
class ObservationSink {
public:
    virtual void write(const Observation&) = 0;
    virtual void flush() = 0;
    virtual ~ObservationSink() = default;
};

class ObservationBus {
public:
    void setSink(std::unique_ptr<ObservationSink>);  // takes ownership; starts writer thread

    void emit(Observation&&);   // lock-free enqueue on the producer thread
    void flush();               // round-trips through writer thread; see §5.3
    void shutdown();            // drain + flush + join writer; call before main exits

    void setCategoryEnabled(std::string_view, bool);  // startup only, before threads run
    bool categoryEnabled(std::string_view) const;     // returns false if no sink; see §3.5
};

The bus starts with no sink. Until `setSink()` is called, `categoryEnabled()` returns `false` for every category, so every `OBS_EVENT` short-circuits to a no-op — no writer thread, no allocations, no work. A run without `--obs-output` pays nothing.
```

Phase 1 ships one sink: `JsonLinesFileSink`. A `NullSink` and an in-memory test sink come for free later.

Trade-offs:

- **Aggregate queries** ("how many times did Y run?") are O(observations) in the *reader*. At current scales, irrelevant.
- **Producer-thread cost is bounded and allocation-dominated.** Emission is a lock-free enqueue — no sink mutex, no I/O syscall, no write. Cost per emit is the `Observation` construction (strings + small vector), addressed in §5.1 if it ever matters.
- **Bounded crash loss.** The writer thread flushes the sink periodically (see §3.3); a crash can lose up to one flush period of in-flight observations (~50ms). Acceptable for our use case.
- **No retained in-process history.** Can't look back at earlier observations from within the program — they're already on disk (or en route). Parse the tail of the file instead.

### 3.1 Source tree

```
src/obs/                             # new module
  observation.h                      # Observation, KeyValue
  observationbus.h/.cpp              # ObservationBus — thread-safe emit, sink-backed
  observationsink.h                  # ObservationSink interface
  jsonlinesfilesink.h/.cpp           # JSON Lines file sink (Phase 1 default)
  observationmacros.h                # OBS_EVENT (or compile-out)
```

Rationale: `src/obs/` (not `src/observability/`) keeps with the existing short-folder convention (`src/scene/`, `src/ui/`). Flat module, no backend split — the bus has no backend-specific code.

### 3.2 Hook sites (concrete)

All entries below are ordinary `OBS_EVENT` emissions — the bus has no special API any of them invoke. The one non-emission site is `bus.shutdown()` at program exit (§3.3), which the application code calls explicitly.

| Category | Emission | Site | Phase |
|---|---|---|---|
| **Scene** | `PrimTransformed` | `src/scene/sceneupdater.cpp:42-65`, SetTransform branch | 1 |
| | `SystemExecuted("SceneUpdater")` | `sceneupdater.cpp:17` entry + exit | 1 |
| | `PrimVisibilityChanged` | `sceneupdater.cpp`, visibility edit branch | 2 |
| | `LayerMuted` / `LayerUnmuted` | `sceneupdater.cpp`, layer mute/unmute branches | 2 |
| | `LayerLoaded` / `LayerUnloaded` | `sceneupdater.cpp`, layer load branches | 2 |
| | `SceneRebuildRequested` | `sceneupdater.cpp`, async fork point | 2 |
| | `SceneOpened` / `SceneSaved` / `SceneNewed` | `src/main.cpp`, scene-lifecycle branches | 2 |
| | `EditBatchCommitted` / `UndoApplied` / `RedoApplied` | `src/scene/undostack.cpp` | 2 |
| **Render** | `FrameBegin` / `FrameEnd` | `src/renderer/renderer.cpp:332` and `:478` | 2 |
| | `FrameGraphCompiled` (`pass_count`, `culled_count`) | `renderer.cpp:462`, after `frameGraph.compile()` | 2 |
| | `PassExecuted` (`name` = pass name) | Execute lambda in each `src/renderer/passes/*.cpp` — 9 passes | 2 |
| | `PassCulled` (`reason`) | `src/renderer/framegraph.cpp:179-184`, culling loop | 2 |
| | `SwapchainRecreate` (`reason`) | `renderer.cpp:336-342` and `:479` | 2 |
| | `MeshUploaded` (`vertex_count`, `index_count`) | `renderer.cpp:224`, cache-miss branch | 2 |
| | `TextureUploaded` (`width`, `height`) | `renderer.cpp:238`, cache-miss branch | 2 |
| | `SceneUploadReceived` (`mesh_count`, `material_count`, `instance_count`) | `src/renderer/renderthread.cpp:62-65` | 2 |
| | `RenderThreadStart` / `RenderThreadStop` | `renderthread.cpp:9` and `:12` | 2 |
| **Engine** | `JobSubmitted` / `JobCompleted` (`name`, `job_id`) | `src/jobsystem.cpp` submit site and worker-complete path | 2 |

This is the Phase-1-and-2 roster. The pattern for new emissions: wherever a subsystem makes a decision or changes state that a reader would want to confirm, emit an observation. Taxonomy grows organically; the bus doesn't care.

**Explicitly not instrumented** (too chatty, too internal, or trivially redundant with another observation):

- Transient resource pool (`resourcepool.cpp`): per-pass allocate/reuse/release — would fire many times per frame. If needed for a specific investigation, add under a dedicated category (e.g. `"RenderResources"`) filtered off by default.
- Descriptor-pool recreation in `renderer.cpp:263` — internal, fires on most scene changes.
- `RenderSnapshotProcessed` in `renderthread.cpp:72` — redundant with `FrameEnd`.
- `FrameGraphDebugSnapshotCaptured` — only fires when the editor UI asks; internal plumbing.
- Anything in `src/rhi/` — hardware-abstraction layer, not a Claude-relevant concern.

### 3.3 Thread model for the bus

Speed on the producer threads (main, render, job workers) matters — they do the engine's real compute work, and observability must not steal cycles from them. That sets the design:

- Producer threads do **no I/O, no mutex acquisition, no blocking** on the sink.
- A dedicated writer thread owns the sink and does all serialization and disk I/O.
- The two are connected by a lock-free MPSC queue.

**Library.** We vendor [moody-camel's `concurrentqueue`](https://github.com/cameron314/concurrentqueue) as a git submodule at `external/concurrentqueue/` — single-header, MIT-licensed, battle-tested. Specifically we use `moodycamel::BlockingConcurrentQueue<Observation>`: it adds a lightweight semaphore so the writer thread can sleep when the queue is empty rather than spinning. Writing a correct lock-free MPSC by hand is a trap; don't.

**Cost on producer threads.** `emit()` is:

1. Filter check: hash lookup in the frozen category map (§3.5) — one branch, no synchronization.
2. Construct the `Observation`: stack header + `std::string` moves + `std::vector<KeyValue>` for fields.
3. `queue.enqueue(std::move(obs))` — lock-free, roughly 20-50ns uncontended.

No mutex, no I/O syscall, no sink call. Hot-path cost is dominated by `Observation` allocation (§5.1), not by queuing.

**Writer thread.** Owned by the bus, started when `setSink()` is called:

```cpp
while (running) {
    Observation obs;
    if (queue.wait_dequeue_timed(obs, 50ms))
        sink->write(obs);
    if (steady_clock::now() - lastFlush > 50ms) {
        sink->flush();
        lastFlush = steady_clock::now();
    }
}
```

Periodic flush every ~50ms bounds crash-loss without ever blocking a producer.

**Shutdown.** Explicit `ObservationBus::shutdown()` called from `main()` before return:

1. Clear `running`.
2. Enqueue a sentinel to wake the writer immediately.
3. Writer thread drains remaining observations, flushes the sink, exits.
4. Bus joins the writer thread.

`atexit()` is *not* used — ordering with thread joins is unreliable and the C++ standard library's atexit interactions with threads have historically been fraught.

**`bus.flush()` semantics.** Callers (keypress, etc.) that need "make sure everything I've emitted so far is on disk" enqueue a flush sentinel and wait on a condition variable the writer signals after processing it. Not free, but rare.

**Backpressure.** Unbounded queue in Phase 1. If the writer thread can't keep up, memory grows — which is a bug to diagnose, not a runtime condition to mask. A capped queue with a drop policy can be added later if long-running production scenarios need it.

### 3.4 Zero-overhead when disabled

Two gating layers:

- **Compile-time:** `-DOBSERVABILITY_DISABLED` routes the macro through dead code. Off by default during development; a release build could flip it.
- **Runtime:** `ObservationBus::setEnabled(false)` short-circuits `emit()` to an atomic-load check. For the common case (development build, obs on), always live. For a production build that still wants the option (e.g., enable via flag for field repro), runtime gate is what we use.

Both exist because they serve different audiences. Don't collapse them.

#### The disable trick

A naive `#define OBS_EVENT(...) ((void)0)` breaks the fluent chain — you can't call `.field(...)` on `void`. Instead the macro expands to a no-op builder inside a `while (false)`:

```cpp
namespace obs::detail {
    struct NoopBuilder {
        template <class T>
        NoopBuilder& field(std::string_view, const T&) noexcept { return *this; }
    };
}

#ifdef OBSERVABILITY_DISABLED
    #define OBS_EVENT(cat, type, name) \
        while (false) ::obs::detail::NoopBuilder{}
#else
    #define OBS_EVENT(cat, type, name) \
        ::obs::detail::Builder{cat, type, name}
#endif
```

A call site like:

```cpp
OBS_EVENT("Scene", "SystemExecuted", "SceneUpdater")
    .field("edits", (int)processed)
    .field("result", resultToString(result));
```

under `-DOBSERVABILITY_DISABLED` expands to:

```cpp
while (false) ::obs::detail::NoopBuilder{}
    .field("edits", (int)processed)
    .field("result", resultToString(result));
```

- **Compile time:** chain is parsed and type-checked; `NoopBuilder::field` is a template that accepts any argument type.
- **Runtime:** `while (false)` never enters the body; none of the `.field()` calls execute; arguments — including `resultToString(result)` — are **not evaluated**.
- **Codegen:** under `-O1`+ dead-code elimination removes the body entirely. Under `-O0` the dead branch is emitted but unreachable — a curiosity, not a correctness issue. `-DOBSERVABILITY_DISABLED` would normally pair with a release build anyway.

`while (false)` (rather than `if (false)`) is chosen because it swallows a trailing `;` cleanly and doesn't dangle-else next to a preceding `if`.

The enabled branch shown above is simplified — the real macro also consults the runtime category filter (§3.5), so arguments to `.field()` aren't evaluated when a category has been silenced at startup either.

**Discipline the macro can't enforce:** treat `OBS_EVENT` arguments and `.field()` values as pure — same rule as `assert()`. If a call relies on a side effect in one of its arguments, that side effect disappears when observability is compiled out or the category is silenced. Typically this means "don't put function calls that do real work inside a `.field()`."

### 3.5 Runtime category filtering

Observations accumulate over time — `OBS_EVENT` calls are meant to stay in the code as permanent narration of behavior (§7), not debug prints to remove later. But some emission sites sit in hot paths (many times per tick), so we need a way to silence categories at startup without editing code.

**Filter axis: category.** The existing first argument to `OBS_EVENT(cat, type, name)` is the filter key. Silencing `"Scene"` stops every `Scene:*` emission. Finer-grained filtering (category + type, per-name patterns) is deliberately out of scope for Phase 1 — easy to add later, and the broad brush covers the motivating case.

**Default: everything on.** The user silences what's noisy, rather than opting in to what's interesting. Matches "we want to see behavior by default."

**CLI:**

```
--obs-only=Scene,Input         # allowlist: only these categories emit
--obs-exclude=Render,Physics   # denylist: everything except these
```

Either alone works. Specifying both is an error.

**Startup-only.** Filter state is configured once during CLI parsing and never changes mid-run. A session-level toggle (keybinding, REPL command) is easy to add later behind the same `setCategoryEnabled` API.

**Storage.** `std::unordered_map<std::string, bool>` on the bus. Populated from CLI once at startup — **before any producer thread is spawned** — and frozen after. `categoryEnabled(sv)` is a plain hash lookup: no mutex, no atomics, no synchronization. Unknown categories return the default (enabled).

**No-sink short-circuit.** `categoryEnabled()` returns `false` unconditionally if no sink has been installed. This means a run without `--obs-output` makes every `OBS_EVENT` skip the entire builder chain — no argument evaluation, no enqueue, no writer thread started. Observability imposes zero cost on runs that don't opt in.

Contract: writes happen before threads start, reads happen from threads. If we ever add mid-run toggling (keybinding, REPL), the read path needs synchronization — `std::shared_mutex` or RCU — at that point, not now.

**Macro with filter applied.** The real form of the enabled branch checks the filter before constructing the `Builder`, so arguments to `.field()` are not evaluated when the category is silenced:

```cpp
#ifdef OBSERVABILITY_DISABLED
    #define OBS_EVENT(cat, type, name) \
        while (false) ::obs::detail::NoopBuilder{}
#else
    #define OBS_EVENT(cat, type, name) \
        if (!::obs::bus().categoryEnabled(cat)) { } else \
            ::obs::detail::Builder{cat, type, name}
#endif
```

Cost of a silenced emit: one hash lookup on the frozen map. No lock, no allocation, no argument evaluation, no sink write.

**Canonical category set.** Phase 2 stabilizes three categories:

| Category | Covers |
|---|---|
| `"Scene"` | USD scene edits, layer ops, SceneUpdater system executions, undo/redo, scene lifecycle (open/save/new) |
| `"Render"` | Frame lifecycle, frame graph compile/execute, pass execution and culling, swapchain changes, render-world upload, mesh/texture cache misses |
| `"Engine"` | Cross-cutting infrastructure — JobSystem submit/complete, bus lifecycle |

Add a new category only when nothing existing fits — the point is that CLI filter arguments have a predictable vocabulary.

---

## 4. Concrete usage examples

Worked examples grounded in this codebase. The macro shape shown here (`OBS_EVENT(cat, type, name).field(k, v)`) matches §5 of the abstract design doc.

### 4.1 Instrumenting a subsystem

User drags a gizmo → `SetTransform` edit lands in `SceneUpdater::update()` fast path at `src/scene/sceneupdater.cpp:42-65`:

```cpp
case EditKind::SetTransform: {
    usdScene.setTransform(cmd->prim, cmd->transform);
    boundsCache.invalidate(cmd->prim);

    OBS_EVENT("Scene", "PrimTransformed", usdScene.pathOf(cmd->prim))
        .field("local", matrixHash(cmd->transform))
        .field("path", "fast");
    break;
}
```

At the end of `update()`:

```cpp
OBS_EVENT("Scene", "SystemExecuted", "SceneUpdater")
    .field("edits", (int)processed)
    .field("path", wasFastPath ? "fast" : "async")
    .field("result", resultToString(result));   // "None" | "TransformsOnly" | "Full"
```

`OBS_EVENT` returns a temporary builder; `.field()` calls accumulate into it; the observation is pushed to the bus when the full-expression ends (builder destructor). No explicit submit. Compiles to `((void)0)` under `-DOBSERVABILITY_DISABLED`.

Two conventions worth calling out, both following §5.5's identity-for-diffing rule:

- The observation's `name` is the USD prim path (`/World/Cube`), never `PrimHandle::index` — indices are ephemeral across runs.
- `local` is a hash of the 4×4 matrix, not the matrix itself. Full matrices are noisy for diffing; a 64-bit hash answers "same or different" cleanly.

### 4.2 JSON Lines stream output

What the `.jsonl` file looks like — one observation per line, written as it's emitted. No opening array, no closing bracket. Excerpt from a run where the user dragged a prim, then muted a layer:

```
{"schema":"ngen.obs","schema_version":1,"started_at_ns":1713700800000000000}
{"ts_ns":18341834,"thread":"main","category":"Scene","type":"PrimTransformed","name":"/World/Cube","fields":{"local":"a3f2c901...","path":"fast"}}
{"ts_ns":18341901,"thread":"main","category":"Scene","type":"SystemExecuted","name":"SceneUpdater","fields":{"edits":1,"path":"fast","result":"TransformsOnly"}}
{"ts_ns":22104020,"thread":"main","category":"Scene","type":"LayerMuted","name":"/layers/debug.usda","fields":{"by":"user"}}
{"ts_ns":22104190,"thread":"main","category":"Scene","type":"SystemExecuted","name":"SceneUpdater","fields":{"edits":0,"path":"async","result":"Full"}}
{"ts_ns":22105000,"thread":"worker-1","category":"Engine","type":"JobStarted","name":"scene_rebuild","fields":{"job_id":"j-42"}}
{"ts_ns":22112337,"thread":"worker-1","category":"Engine","type":"JobCompleted","name":"scene_rebuild","fields":{"job_id":"j-42","result":"ok"}}
```

Notes:

- First line is a schema metadata record; readers that don't care skip it (or filter on presence of `ts_ns`).
- `ts_ns` is monotonic since program start.
- Every fact is an observation — no special event types, no aggregated sidecar tables. Whatever state or scope a subsystem wants to convey goes in `fields`.
- JSONL means tools like `jq -c`, `grep`, and line-by-line parsers work directly. Claude can scan the file incrementally without loading it all.
- A crash produces a truncated-but-parseable file — the last line may be incomplete, but everything above it is readable.

### 4.3 The Claude feedback loop end-to-end

This is the loop the whole layer exists to support. Example task: "Add a new edit type `PrimVisibilityToggle` to `SceneUpdater`."

1. **Claude makes the code change.** Adds `EditKind::VisibilityToggle` and a new branch in `SceneUpdater::update()` that flips the prim's `visible` attribute via USD.

2. **Claude adds observations as part of the change.** The new behavior needs to be visible — otherwise confirming it worked means re-reading the diff, which doesn't prove runtime behavior. So Claude adds:

   ```cpp
   case EditKind::VisibilityToggle: {
       bool next = !usdScene.getVisibility(cmd->prim);
       usdScene.setVisibility(cmd->prim, next);

       OBS_EVENT("Scene", "PrimVisibilityChanged", usdScene.pathOf(cmd->prim))
           .field("visible", next);
       break;
   }
   ```

3. **Claude builds and runs.**

   ```
   $ make
   $ ./ngen --obs-output=./artifacts/obs.jsonl assets/three_cubes.usda
   ... user toggles visibility a couple times, then quits ...
   [obs] stream closed: ./artifacts/obs.jsonl (127 observations)
   ```

4. **Claude reads the stream and reasons.** Opens `obs.jsonl`, filters by line (e.g. `jq -c 'select(.type == "PrimVisibilityChanged")' obs.jsonl`) and checks:
   - Observations present, one per toggle.
   - `name` is a valid USD path, not an index.
   - `visible` alternates `true` → `false` → `true` as expected.
   - `SystemExecuted("SceneUpdater")` follows each toggle with `result` reflecting the edit.

   If something's missing or wrong, Claude already knows *which* part of the change didn't land — the missing observation points directly at the code it expected to run. No predicate engine needed; pattern-matching JSON against intent is something an LLM does natively.

5. **Observations stay in the code.** There's no cleanup step. The `OBS_EVENT` calls document what the change was supposed to do and will narrate the same behavior to the next person (human or AI) who touches this subsystem.

This is the whole loop. No spec files, no `--verify`, no verifier output to parse. The artifact is the evidence and the reader is the judge.

---

## 5. Key design decisions (discussion-worthy)

These are the decisions where I'd like pushback before building.

### 5.1 `Observation`'s representation — allocation cost

The abstract design has `std::string category/type/name` + `std::vector<KeyValue>`. Every emitted observation allocates. With streaming, each observation is serialized and discarded immediately — so allocations are short-lived but still happen on producer threads.

**Proposal:** Phase 1 accepts the allocation. Phase 2 optimization (only if profiler calls for it):
- `string_view`-based category/type (all categories are compile-time literals — trivially interned).
- A small thread-local scratch buffer for serialization, reused across emissions. Avoids heap traffic entirely in the hot path.

This is a *refactor that won't change the public macro API*, so we can defer it without painting ourselves in.

### 5.2 Output: JSON artifact only

The abstract design proposes both a JSON artifact and an in-process `EngineObservationAPI`. Claude is out-of-process by construction, so the in-process API has no consumer. **Drop it.** The bus writes JSON; readers (Claude, any future in-editor viewer) read files.

This also means we don't owe anyone a stable C++ query API. The JSON schema is the contract, versioned via `schema_version`.

### 5.3 Streaming model & flush cadence

Flushing is a writer-thread concern, not a producer concern — producers never touch disk. Triggers:

- **Writer-thread periodic flush.** Every ~50ms the writer calls `sink->flush()` regardless of queue state. Bounds crash-loss without stalling producers. Phase 1.
- **CLI flag `--obs-output=<path.jsonl>`** opens a `JsonLinesFileSink` for the whole program run. Simplest for Claude — "run the thing, read the file."
- **`bus.flush()` on keypress in the editor.** Enqueues a flush sentinel; the writer processes it; the calling thread blocks on a condvar until the writer signals done. Use sparingly — it's a round-trip.
- **`bus.shutdown()` at program exit.** Stops accepting emits, drains the queue, flushes the sink, joins the writer thread. Called explicitly from `main()` before return.

Crash-survival: at most one flush period (~50ms) of in-flight observations are lost on a crash. Everything older is on disk and parseable (truncated-but-readable JSONL).

One thing streaming explicitly gives up: **no retrospective in-process history.** Can't look back at earlier observations from within the program — they're already en route to disk. Parse the tail of the file instead.

### 5.4 Observation identity for diffing

Two runs of the same scene should produce streams Claude can meaningfully compare — "did my change make this subsystem start doing something new?" only works if run-to-run noise is filtered out.

**Rule:** observations must not contain pointers, `PrimHandle` indices, OS/driver handle values, or anything that varies across runs. They contain: prim paths, stable names, counts, formats, hashes. The JSON serializer enforces this (typed fields only; no opaque blobs).

This is a *convention*, not something the type system enforces. Document it and review emissions against it.

---

## 6. Phased rollout

Three phases. Phase 1 is the smallest useful diff; the later two are additive.

### Phase 1 — Scaffolding (target: smallest useful diff)

- Vendor moody-camel's `concurrentqueue` as a git submodule at `external/concurrentqueue/`.
- `src/obs/` module created.
- `ObservationSink` interface + `JsonLinesFileSink` implementation.
- `ObservationBus` with `emit()` (lock-free enqueue) / `flush()` (writer round-trip) / `setSink()` / `shutdown()` / `setCategoryEnabled()` / `categoryEnabled()`.
- Writer thread owned by the bus: dequeues from the `BlockingConcurrentQueue<Observation>`, writes to the sink, periodic flush every ~50ms.
- Category filter storage: frozen `std::unordered_map<std::string, bool>`, populated before threads start, no synchronization after (§3.5).
- `OBS_EVENT` macro (fluent builder with `.field(k, v)`), compile-time disable (`-DOBSERVABILITY_DISABLED`), runtime category filter baked into the enabled branch.
- CLI flags: `--obs-output=<path.jsonl>` (required to produce any output), `--obs-only=a,b` / `--obs-exclude=a,b` (mutually exclusive, only meaningful with `--obs-output`).
- Default behavior without `--obs-output`: no sink installed, `categoryEnabled()` returns false for all categories, every `OBS_EVENT` is a no-op. Zero-cost when not opted in.
- Sink writes a schema-version metadata line as its first record.
- Explicit `bus.shutdown()` call from `main()` before return (only relevant if a sink was installed).
- `PrimTransformed` + `SystemExecuted("SceneUpdater")` emitted from `src/scene/sceneupdater.cpp` to prove the end-to-end loop.
- Extract §9 of this plan verbatim (minus its plan-meta preamble) to `obs.md` at the repo root, alongside `CLAUDE.md`, so future Claude sessions pick up the usage guide automatically.

**Success criterion:** after a code change, Claude runs the engine with the flag, reads the `.jsonl`, and can articulate what the instrumented subsystem did.

### Phase 2 — Broaden subsystem coverage

Wire the Phase 2 hooks listed in §3.2. Scope is roughly 20 observation types across the three canonical categories (`"Scene"`, `"Render"`, `"Engine"` — see §3.5).

- **Scene.** All `EditKind` branches in `SceneUpdater`, scene lifecycle (open/save/new), layer operations, undo/redo. Enriches the existing `SystemExecuted("SceneUpdater")` with enough fields (`edit_kinds`, `path`, `result`) that the fast/async/full decision is readable.
- **Render.** Frame lifecycle (`FrameBegin`/`FrameEnd`), `FrameGraphCompiled` summary, `PassExecuted` for each of the 9 passes, `PassCulled` from the culling loop, `SwapchainRecreate`, `MeshUploaded`/`TextureUploaded` on cache miss, `SceneUploadReceived` on the main→render handoff.
- **Engine.** `JobSubmitted`/`JobCompleted` around JobSystem work.
- **Category set stabilized** in §3.5 and documented.

Deferred (not in Phase 2):

- Serialization-scratch optimization — profiler-gated (§5.1). Only if measurement justifies it.
- Transient resource pool emissions — noise, see §3.2. Add later under an opt-in category if ever needed.

### Phase 3 — Luxuries

- Live in-editor observation viewer.

---

## 7. Relationship to unit tests

Observability does **not** replace unit testing — the two answer different questions and are meant to coexist.

|                | Unit test                           | Observation                                    |
|----------------|-------------------------------------|------------------------------------------------|
| Question       | "Does this function do X?"          | "Did the running system actually do X?"        |
| Scope          | isolated, synthetic inputs          | full engine run, real scene, real threads      |
| Failure mode   | assertion trips, CI goes red        | Claude reads the stream and reasons            |
| When useful    | pure logic, contracts, invariants   | integration behavior, subsystem wiring         |
| Persistence    | permanent (in `tests/`)             | permanent (lives next to the code it narrates) |

A unit test can verify that `SceneUpdater` classifies a `SetTransform` edit as a fast-path candidate. Observation verifies that when the engine actually runs, `SceneUpdater` took the fast path and produced the expected `PrimTransformed` emission. Neither subsumes the other.

**Consequence for how observations are written.** Unit tests are separate artifacts written once, triggered by CI. Observations are *instrumentation in the production code path* — a change that needs verification gets an `OBS_EVENT` added alongside it, and the emission stays after the change lands. There is no "remove the debug print when done" step. Scratch instrumentation becoming permanent is the intended outcome, not a mistake.

This also means the bar for ergonomics is higher than for a test framework: emission must be so cheap to add that there is no friction in sprinkling it liberally. That's the argument for the fluent-builder API in §4 and against any "register this observation type first" ceremony.

---

## 8. Out of scope

Explicitly *not* doing:

- **Verification engine / ExpectationSpec / `--verify` CLI.** Claude is the reader; the artifact is the evidence. No predicate DSL.
- **Substitute for unit tests.** See §7 — they're parallel, both persist.
- **Performance measurement.** No timing fields, no durations — that's a separate concern for a future profiling system (likely Tracy). Observations answer "what happened," not "how fast." The `ts_ns` timestamp on each observation is for ordering only, not performance analysis.
- **Human-readable log formatter.** The bus emits structured observations only; it is not a replacement for `std::println`-style logs. Keeping the output purpose-built for Claude avoids drift into general-purpose logging.
- Replay / deterministic re-execution. Observations describe what happened; they don't reconstruct it.
- Network transport. Artifacts are files on disk. If we ever want a remote viewer, it reads the files.
- In-process C++ query API. The JSON file is the only output surface.

---

## 9. Usage reference for AI agents

This section is standalone — you can jump here from anywhere in the plan and get what you need without reading §1–§8.

> **When Phase 1 lands, this section is lifted verbatim to `obs.md` at the repo root** (next to `CLAUDE.md`), so that any future Claude/agent session sees the usage guide without needing to find and read the plan. The content below is deliberately self-contained for that reason — no cross-references to other sections.

### 9.1 What this gives you

The engine ships a streaming observation bus. Code emits structured events via `OBS_EVENT`; a writer thread streams them to a JSON Lines file. You read the file to confirm what the engine actually did on a given run.

Three things you do:

1. **Add observations** where a decision or state change should be visible.
2. **Run the engine** with `--obs-output=path.jsonl`.
3. **Read the file** with `jq` or any line-by-line tool.

Observations stay in the code. There's no "remove when done" step — they document intent and will narrate behavior the next time anything near that code runs.

### 9.2 Emitting an observation

```cpp
#include "obs/observationmacros.h"

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

### 9.3 Field conventions

`.field(key, value)` accepts ints, floats, bools, string literals, `std::string`, `std::string_view`. Keys are snake_case string literals.

- **Pure arguments only.** The whole chain is skipped when the category is silenced or observability is compiled out (`-DOBSERVABILITY_DISABLED`). Don't rely on side effects in `.field()` arguments — same rule as `assert()`.
- **Stable values.** Pointers, handles, thread-ids-as-numbers, OS resource IDs → ephemeral, don't emit. Prim paths, resource names, counts, format strings, hashes → stable, emit freely. If two runs of the same scene produce different field values, they can't be diffed.
- **Many simple fields, not one compound.** `.field("width", 2048).field("height", 2048)` beats `.field("size", "2048x2048")`.

### 9.4 Patterns

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

### 9.5 Running and capturing

```
$ ./ngen --obs-output=./obs.jsonl assets/scene.usda
```

**If `--obs-output` is not provided, observability is entirely off.** No sink is installed, no writer thread starts, and every `OBS_EVENT` short-circuits to a no-op. The engine runs at zero observability cost. You must pass the flag to get any output at all.

Optional filters (mutually exclusive, only meaningful when `--obs-output` is given):

- `--obs-only=Scene,Engine` — only these categories emit.
- `--obs-exclude=Render` — everything except these.

Default when `--obs-output` is given: all canonical categories emit. The output file is written incrementally; you can `tail -f` it while the engine runs.

### 9.6 Reading a dump

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

### 9.7 Troubleshooting

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

### 9.8 Don'ts

- Don't emit from per-draw, per-vertex, or per-entity inner loops. Pick the wrapping decision or state transition instead.
- Don't put expensive computations inside `.field()`. They run on the hot path and their results are serialized and discarded.
- Don't invent categories casually. If nothing in the canonical list above fits, add one there first so the vocabulary stays predictable.
- Don't remove observations because you're "done with them." They stay permanently — that's the point.

---

## 10. First concrete step

If this plan holds up, Phase 1 starts with:

1. Add `external/concurrentqueue/` as a git submodule (moody-camel).
2. Create `src/obs/observation.h` with `Observation` and `KeyValue`.
3. Create `src/obs/observationsink.h` with the abstract `ObservationSink` interface.
4. Create `src/obs/jsonlinesfilesink.{h,cpp}` — buffered `ofstream`, per-observation serializer, schema-version metadata line on open.
5. Create `src/obs/observationbus.{h,cpp}` — lock-free `emit()` via `BlockingConcurrentQueue<Observation>`, writer thread with ~50ms periodic flush, `shutdown()` for clean drain, and the frozen category-filter map.
6. Wire the filter into the `OBS_EVENT` macro's enabled branch (`if (!bus().categoryEnabled(cat)) { } else Builder{...}`).
7. Add CLI flags: `--obs-output=<path.jsonl>`, `--obs-only=...` / `--obs-exclude=...` (mutually exclusive).
8. Call `bus.shutdown()` from `main()` before return.
9. Add one `OBS_EVENT` call in `SceneUpdater::update()` to prove the loop end-to-end.
10. Extract §9 to `obs.md` at the repo root (next to `CLAUDE.md`) so it's discoverable by future Claude sessions without reading the plan.

Everything else is additive.
