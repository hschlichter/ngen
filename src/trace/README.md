# Trace

Every ngen process records its flow and its messages into an always-on ring in memory, and streams them over RPC to whoever subscribes:
what happened and in what order (a process started, a scene opened, a file was written), and what went wrong (errors and warnings).
`ngen-introspect` subscribes to every process in its directory and merges their events by time: in its Trace tab, or as JSON lines from
`ngen-introspect trace` (`src/introspect/README.md`).

**The trace is not for data.** Counts, statistics, timings, pass lists, what is resident on the GPU, the camera: anything that can be asked
for is a record (`src/rpc/README.md`, "Records"), read when it is needed with `ngen-introspect get`, or at an exact frame with the `record`
script verb. Nothing is traced every frame. A steady state is silent: a frame that runs like the last one adds nothing to the trace.

Events stay in the code. There's no "remove when done" step: they document intent and narrate behaviour the next time anything near that code
runs.

## Emitting an event

```cpp
#include "trace.h"

TRACE_EVENT("Scene", "SceneOpened", path)
    .text(std::format("scene opened: {} prims in {:.0f} ms", prims, ms))
    .field("prims", prims)
    .field("ms", ms);

TRACE_WARNING("Render", "FormatFallback", "AAPass").text("R16G16B16A16_SFLOAT not usable as storage image; falling back to blit");

TRACE_ERROR("Render", "ShaderLoadFailed", id).text(std::format("shader {} was not packed", id));
```

`TRACE_EVENT`, `TRACE_WARNING` and `TRACE_ERROR` take `(category, type, name)` and build an event at level info, warning or error. `.text()`
sets the message for people; chain `.field(key, value)` for the values a reader filters on. The event is pushed into the ring at the end of the
statement.

**The console is the trace.** Every event is also printed, prefixed with the local time: info on stdout, warnings and errors on stderr with
`warning:` or `error:`, the event's text (its type and name when it has none). Engine code prints nothing else: no `std::println`, no
`printf`, so what a process's console shows and what its trace holds are the same. Give every event a text that reads on its own, path
included, since the console doesn't show the name or the fields. Only a program's own output stays outside: usage errors, and the JSON and
summaries the tools print. For fields added in a loop, name the builder: `trace::Builder event(trace::Level::Info, "Scene", "SceneOpened",
path);` pushes when `event` goes out of scope.

| Level | When |
|---|---|
| `info` | Flow: a lifecycle step, a load started or finished, a file written, an edit applied |
| `warning` | Something went wrong and was worked around: a format fallback, a feature off, a request that failed while the process goes on |
| `error` | Something failed: a shader that didn't load, a file that couldn't be written, a validation error |

| Argument | Rule |
|---|---|
| `category` | One of the set below. |
| `type` | CamelCase verb phrase: `SceneOpened`, `LayerMuted`, `ShaderLoadFailed`. |
| `name` | Stable identifier for the subject: USD prim path, layer path, pass name, job id. **Never** a pointer, handle, or ephemeral index. |

| Category | Covers |
|---|---|
| `Scene` | Scene open and save, applied edits (not preview edits), undo/redo, assets the scene could not read |
| `Render` | Scene uploads, swapchain recreation, files written (screenshots, captures, dumps), shader and format problems, RHI messages |
| `Engine` | Cross-cutting infrastructure: process start and exit, RPC listen, connect and failed calls, script commands, RenderDoc |
| `Asset` | The asset server: every line of its log, as `Message` events with the line in `text`; failed requests are warnings |

Add a category only when nothing existing fits.

## What not to trace

- **Data.** If the event's point is a number someone will read later, it belongs in a record. `FrameGraphCompiled` with a pass count,
  `TextureUploaded` per texture or `RenderStats` every 60 frames are all data: the `render` record holds the passes, textures and draw
  counters, and a script records it at the frame it needs.
- **Steady state.** Nothing per frame, per pass, per draw or per entity, and nothing sampled every N frames. A drag sends preview edits every
  frame; only the edit that commits it is traced.
- **Echoes of a reply.** A method's result goes back to its caller. A call that fails is traced (`RpcCallFailed`), one that works isn't.

## Field conventions

`.field(key, value)` takes ints, floats, bools, string literals, `std::string`, `std::string_view`. Keys are snake_case.

- **Always on.** Every event is built and stored, whether or not anyone subscribes, so keep `.field()` arguments cheap and free of side effects.
- **Stable values.** Pointers, handles and OS resource ids differ between runs; don't emit them. Prim paths, resource names, counts and hashes
  are stable. If two runs of the same scene produce different field values, they can't be diffed.
- **Many simple fields, not one compound.** `.field("width", 2048).field("height", 2048)` beats `.field("size", "2048x2048")`.

## How it works

- **The ring** (`tracering.*`): the process's last 65,536 events. A push takes a short lock, assigns the event its sequence number and stamps its
  time, so times never decrease with sequence. Readers copy events out by sequence number, each with its own cursor.
- **One clock.** `ts_ns` is `CLOCK_MONOTONIC` in nanoseconds, the same in every process on the machine (and the profiler's clock), so events of
  different processes merge by time.
- **The stream** (`tracestream.*`): `trace.subscribe {since_ns?}` starts a subscription on a connection, from the oldest event the ring holds or
  from `since_ns`; the reply is `{process, from, head}`. Then `trace.events` notifications arrive, `{process, dropped, events: [...]}`, every
  50 ms or once 1,024 events wait, at most 4,096 events each. `trace.unsubscribe` stops it. ngen-view's `RpcEndpoint` and the asset server
  answer both on their I/O thread.
- **Never stalls a frame.** Nothing more is sent to a subscriber while over 16 MiB wait on its connection. Events the ring overwrites meanwhile
  are counted in the next batch's `dropped`.
- **Nothing lost at exit.** A process sends its last events and waits up to a second for them to be written before its connections close:
  ngen-view after its `ProcessExiting` event, the asset server when it stops.

An event on the wire, and as a line from `ngen-introspect trace` (which adds `process` and leaves out `seq`):

```json
{"ts_ns":5844865854761,"process":"view:25117","thread":"140688089557888","level":"info","category":"Scene","type":"SceneOpened","name":"assets/three_cubes.usda","text":"scene opened: 8 prims, 2 layers in 55 ms","fields":{"layers":2,"ms":55.13,"prims":8}}
```

## Reading a trace

```bash
ngen-introspect trace --until-exit=view --output=/tmp/trace.jsonl &   # before the view starts
jq -c 'select(.level != "info") | {process, level, type, text}' /tmp/trace.jsonl   # what went wrong; empty on a clean run
jq -r '[.process, .type, .text] | @tsv' /tmp/trace.jsonl                            # the run's flow
jq -c 'select(.category == "Scene")' /tmp/trace.jsonl
jq -c 'select(.name == "/World/Cube")' /tmp/trace.jsonl
ngen-introspect trace --level=warning                                                # live: warnings and errors only
```

## Troubleshooting

**My event didn't appear.**

1. Was the trace command running before the process, or started with `--history`? Without `--history` it takes events from its own start.
2. Did the ring wrap? A long run before the tool connected keeps only the last 65,536 events; the tool reports dropped events on stderr.
3. Was the emitting branch reached? Add an event higher up in the path to narrow it down.
4. Did the process crash? A crash loses whatever was not yet sent, up to 50 ms of events.

**Values vary across runs.** Something ephemeral got into a field — a pointer, a handle index, a non-deterministic id. Replace it with a stable
identifier.

## Don'ts

- Don't trace data or steady state ("What not to trace" above). Pick the state transition, or add the value to a record.
- Don't print from engine code (`std::println`, `printf`, `std::cout`): trace it, and the trace prints it. Tools' own output stays on stdout
  and stderr.
- Don't put expensive computations inside `.field()`: they run on every pass through the code.
- Don't remove events because you're "done with them". They stay; that's the point.
