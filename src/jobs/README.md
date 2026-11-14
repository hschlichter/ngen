# Jobs

The job system (`jobsystem.*`, library `jobs`): a pool of worker threads and two queues of jobs. It is how work leaves the main, render and
scene threads: a loop split across the workers within a frame, or something slow (a large allocation) made beside the frame while it goes on.
Standard library and `profile` only.

## Two kinds of job

| Kind | What it is | Who waits | Example |
|---|---|---|---|
| `JobKind::Frame` | a short piece of the current frame's work | the thread that submitted it, with `JobSystem::wait`, running other frame jobs meanwhile | the chunks of `parallelFor` |
| `JobKind::Background` | something slow that the frame must not stop for | nobody: its `JobFence` is polled with `ready()`, and its result taken in the frame it is | a grown GPU buffer, a large texture |

The rules:

- **The main and render threads never wait for a background job.** A frame that needs one that isn't ready goes on without what depends on it,
  and takes it in a later frame. Only teardown waits (a renderer's `destroy` for a growth still in flight), never a frame.
- **`wait` runs only frame jobs.** A thread waiting for its own loop that picked up a 200 ms allocation would make the long frame the
  background job was there to avoid.
- **Workers take frame jobs first**, then background jobs: a frame waits on the first, nothing on the second.
- A frame job doesn't wait for another frame job it submitted itself except through `parallelFor` or `wait`, which help instead of blocking.

## API

```cpp
#include "jobsystem.h"

// A loop over [0, count) in chunks of `grain`, on the workers and the caller; returns when every chunk has run.
JobSystem::parallelFor(slots.size(), 4096, [&](size_t begin, size_t end) {
    packRecords(begin, end);
}, "PackInstanceRecords");

// Something slow beside the frame; polled each frame, taken once ready.
auto fence = JobSystem::submit([request] {
    makeBuffer(request);
}, "GrowInstanceBuffer", JobKind::Background);
if (fence.ready()) {
    // take the result
}
```

- `submit(job, name, kind, location)`: `name` labels the job in the profiler and the `jobs` record and must outlive the job (a string literal).
  The zone points at the caller's line (`location`). Without workers (a program that never called `init`) the job runs on the caller, at once.
- `parallelFor(count, grain, body, name)`: `body(begin, end)` per chunk; each runner takes the next chunk until none is left, so a slow chunk
  doesn't hold the rest. One chunk, or no workers, runs on the caller alone. `body` may run on several threads at once: it writes only what
  its range owns, and sums or lists it builds per range are combined after.
- `wait(fence)`, `waitAll(fences)`: for frame jobs.

## Where it sits

`JobSystem::init` starts the workers in `ngen-view`'s `main` (`src/apps/view.cpp`), one fewer than the hardware threads; `JobSystemStarted` in the
trace says how many. Each worker has a lane in the profiler (`Worker N`), where its jobs show as zones in the Jobs group under their names, and
an idle worker shows `Idle` slices. The main, render and scene threads submit; nothing else in the frame changes. The users:

- `BufferGrowth` (`src/renderer/buffergrowth.*`): the GPU tables' next, larger buffers, as background jobs named `BufferGrowth`
  (`src/renderer/README.md`, "Growth").
- The render thread's per-slot loops, as `parallelFor`: packing the changed instance records (`PackInstanceRange`), writing a change
  list's slots a chunk each (`CopyChangedRange`) and the culling groups' boxes (`GroupBoundsRange`) (`src/renderer/README.md`, "Scene data
  on the GPU").

## At runtime

- **Record** `jobs` (`src/view/profilecommands.cpp`): workers; per kind the jobs queued, running and completed since start; the background job
  running longest and for how long.
- **Window**: the Performance window's worker lanes show the jobs as they run.
