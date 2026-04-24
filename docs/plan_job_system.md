# Plan: Basic Job System

## Context

The engine needs a general-purpose job system for scheduling parallel work. The immediate use case is background scene updates (see
`plan_background_scene_updates.md`), but it should also serve future needs like parallel mesh extraction, spatial index rebuilds, and frame graph pass
execution.

The engine uses C++23. No external threading libraries — pure standard library (`std::jthread`, `std::mutex`, `std::condition_variable`, `std::atomic`).

## Design

A lightweight thread pool with a shared job queue. Submitting a job returns a `JobFence` that can be waited on. Multiple fences can be waited on together.

### API

```cpp
// src/jobsystem.h

using JobFunc = std::function<void()>;

class JobFence {
    friend class JobSystem;
    std::shared_ptr<std::atomic<bool>> done;
public:
    bool ready() const;
};

class JobSystem {
public:
    void init(uint32_t numWorkers = 0);  // 0 = hardware_concurrency - 1
    void shutdown();

    auto submit(JobFunc job) -> JobFence;

    void wait(const JobFence& fence);
    void waitAll(std::span<const JobFence> fences);
};
```

### Usage Examples

**Single job with wait:**
```cpp
auto fence = jobSystem.submit([&] {
    rebuildPrimCache();
    rebuildAllTransforms();
});
// ... do other work ...
jobSystem.wait(fence);
```

**Multiple parallel jobs:**
```cpp
auto f1 = jobSystem.submit([&] { extractMeshBatch(0, 100); });
auto f2 = jobSystem.submit([&] { extractMeshBatch(100, 200); });
std::array fences = {f1, f2};
jobSystem.waitAll(fences);
renderer.uploadRenderWorld(world);
```

**Fire and forget (ignore the fence):**
```cpp
jobSystem.submit([&] { writeLogToDisk(); });
```

**Background scene update:**
```cpp
if (!editingBlocked) {
    editingBlocked = true;
    sceneUpdateFence = jobSystem.submit([&] {
        executeEdits();
        processChanges();
        extract();
    });
}
// Later:
if (sceneUpdateFence.ready()) {
    jobSystem.wait(sceneUpdateFence);  // join (instant since ready)
    swapResults();
    editingBlocked = false;
}
```

### Implementation

**`src/jobsystem.h`** — `JobFunc`, `JobFence`, `JobSystem` class.

**`src/jobsystem.cpp`** — Thread pool implementation.

```
JobFence
 └── std::shared_ptr<std::atomic<bool>> done   (shared between submitter and worker)

JobSystem
 ├── std::vector<std::jthread> workers
 ├── std::deque<Job> queue                      (Job = {JobFunc, shared_ptr<atomic<bool>>})
 ├── std::mutex queueMutex
 ├── std::condition_variable queueCV
 ├── std::condition_variable doneCV             (notified when any job completes)
 └── bool stopping = false
```

**Worker loop:**
```
while (!stopping):
    lock(queueMutex)
    wait on queueCV until queue not empty or stopping
    pop job from front of queue
    unlock
    execute job.func
    job.done->store(true)
    doneCV.notify_all()
```

**submit():**
```
auto done = make_shared<atomic<bool>>(false)
lock(queueMutex)
push {func, done} to queue
notify_one on queueCV
return JobFence{done}
```

**wait(fence):**
```
while !fence.done->load():
    // Try to steal work from the queue while waiting
    lock(queueMutex)
    if queue not empty:
        pop job
        unlock
        execute job.func
        job.done->store(true)
        doneCV.notify_all()
    else:
        // No work to steal — wait for any job to complete
        doneCV.wait(lock, [&] { return fence.done->load() || stopping; })
```

The wait implementation does useful work instead of blocking — the calling thread becomes a temporary worker. This prevents deadlocks when the pool is saturated
and the main thread is waiting.

**waitAll(fences):**
```
for each fence in fences:
    wait(fence)
```

Since `wait()` does useful work, waiting sequentially on N fences still processes jobs from the queue. If all fences complete while working on earlier ones,
subsequent waits return immediately.

**ready():**
```
return done->load(memory_order_acquire)
```

Non-blocking poll. Useful for checking completion without stalling (e.g., check once per frame for background scene updates).

**init():**
```
numWorkers = (numWorkers == 0) ? max(1, hardware_concurrency - 1) : numWorkers
spawn numWorkers jthreads running the worker loop
```

**shutdown():**
```
lock(queueMutex)
stopping = true
unlock
queueCV.notify_all()
doneCV.notify_all()
clear workers (jthread joins on destruction)
```

### Thread Count

`hardware_concurrency - 1` workers by default, reserving one core for the main thread. Minimum 1 worker.

### Lifetime

Created in `main()` before the main loop, destroyed after. Passed by reference where needed.

## Files

| File | Purpose |
|------|---------|
| `src/jobsystem.h` | JobFunc, JobFence, JobSystem class |
| `src/jobsystem.cpp` | Thread pool implementation |
| `src/main.cpp` | Create JobSystem before main loop |

## What This Does NOT Include

- **Job dependencies / DAG** — Use fences for sequencing.
- **Priorities** — Single FIFO queue.
- **Work stealing** — Workers share one queue. Add per-worker queues later if contention is a bottleneck.
- **Job allocator** — Uses `std::function` (heap allocates). Fine for coarse-grained jobs.
- **TBB** — Pure standard library, no external dependencies.

## Verification

1. Build with `make`
2. Submit a job, wait on the fence, verify it completes
3. Submit multiple jobs, `waitAll`, verify all complete
4. Check `fence.ready()` returns false before completion, true after
5. Verify `wait()` does work while waiting (submit N+1 jobs where N = worker count, wait on the last one)
6. Verify clean shutdown — no hangs, no use-after-free
7. Verify `make format` passes
