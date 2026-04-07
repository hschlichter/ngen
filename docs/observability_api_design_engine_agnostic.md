# Observability API Design (Engine-Agnostic)

## 1. Purpose

The Observability API provides a **machine-readable introspection layer** for the engine. Its primary goal is to enable an automated feedback loop:

```
AI modifies code → Engine runs → Engine emits structured observations → AI verifies outcome
```

This system is not logging, tracing, or unit testing. It is a **runtime interrogation interface** designed for verification of behavioral intent.

---

## 2. Design Goals

- **Machine-readable** (not human logs)
- **Deterministic snapshots** of execution
- **Low overhead in production-disabled mode**
- **Subsystem-agnostic** (renderer, physics, ECS, networking)
- **Queryable and serializable**
- **Stable identifiers for diffing and comparison**

---

## 3. Core Concepts

### 3.1 Observation
A structured fact emitted during execution.

Examples:
- "Pass executed"
- "Texture bound"
- "Entity transformed"
- "Physics step completed"

```cpp
struct Observation {
    uint64_t frame;
    uint64_t timestamp_ns;
    uint32_t thread_id;

    std::string category;   // "Render", "Physics", "ECS"
    std::string type;       // "PassExecuted", "ResourceBound"
    std::string name;       // e.g. "lighting"

    std::vector<KeyValue> fields;
};
```

---

### 3.2 Frame Report
A complete snapshot of observations for a frame.

```cpp
struct FrameReport {
    uint64_t frame;

    std::vector<Observation> events;
    std::vector<ResourceState> resources;
    std::vector<ExecutionSummary> executions;
};
```

---

### 3.3 Resource State
Represents any persistent engine object.

```cpp
struct ResourceState {
    std::string name;
    std::string type; // "Texture", "Buffer", "Entity", "RigidBody"

    std::unordered_map<std::string, std::string> properties;
};
```

---

### 3.4 Execution Summary
Captures whether systems or passes actually ran.

```cpp
struct ExecutionSummary {
    std::string name;   // "lighting", "physics_step"
    bool executed;
    uint32_t count;

    double cpu_time_ms;
    double gpu_time_ms; // optional
};
```

---

## 4. Observation Bus

Central system for emitting observations.

```cpp
class ObservationBus {
public:
    void emit(const Observation& obs);

    void beginFrame(uint64_t frame);
    void endFrame(uint64_t frame);

    const FrameReport& getFrame(uint64_t frame) const;
};
```

Convenience macros:

```cpp
#define OBS_EVENT(cat, type, name) ...
#define OBS_FIELD(key, value) ...
#define OBS_EMIT() ...
```

---

## 5. High-Level Observation API

Provides structured access for AI or tools.

```cpp
class EngineObservationAPI {
public:
    FrameReport getFrameReport(uint64_t frame);

    ExecutionSummary getExecution(std::string_view name);

    ResourceState getResource(std::string_view name);

    std::vector<Observation> queryEvents(
        std::string_view category,
        std::string_view type);

    std::vector<ResourceState> listResources(std::string_view type);
};
```

---

## 6. Renderer-Specific Extensions

### Pass Execution

```cpp
OBS_EVENT("Render", "PassExecuted", "lighting")
    .field("draw_count", drawCount)
    .field("pipeline", pipelineName);
```

### Resource Binding

```cpp
OBS_EVENT("Render", "ResourceBound", "lighting")
    .field("slot", "albedo")
    .field("resource", tex.name)
    .field("format", tex.format);
```

### Output Tracking

```cpp
OBS_EVENT("Render", "Output", "main_color")
    .field("hash", hashValue);
```

---

## 7. Engine-Agnostic Usage Examples

### Physics

```cpp
OBS_EVENT("Physics", "Step", "physics_world")
    .field("body_count", bodyCount)
    .field("dt", dt);
```

### ECS

```cpp
OBS_EVENT("ECS", "SystemExecuted", "TransformSystem")
    .field("entities", count);
```

### Networking

```cpp
OBS_EVENT("Network", "PacketReceived", "player_input")
    .field("size", bytes);
```

---

## 8. Checkpoints

Snapshots at meaningful execution boundaries.

```cpp
class CheckpointSystem {
public:
    void capture(std::string_view name);
};
```

Example:

```cpp
checkpoint.capture("after_culling");
checkpoint.capture("after_render_graph");
```

---

## 9. Expectation Specification

Defines what the AI expects after a change.

```cpp
struct Expectation {
    std::string type;
    std::unordered_map<std::string, std::string> params;
};

struct ExpectationSpec {
    std::string description;
    std::vector<Expectation> expectations;
};
```

Example:

```json
{
  "description": "Lighting uses shadow atlas",
  "expectations": [
    {"type":"pass_executed","name":"lighting"},
    {"type":"resource_bound","pass":"lighting","slot":"shadow_map","resource":"shadow_atlas"}
  ]
}
```

---

## 10. Verification Engine

Matches expectations against observations.

```cpp
struct VerificationResult {
    bool success;
    std::vector<std::string> failures;
};

class Verifier {
public:
    VerificationResult verify(const FrameReport& report,
                              const ExpectationSpec& spec);
};
```

---

## 11. Artifact Output

Simple file-based workflow.

```
artifacts/
  run_001/
    frame_120.json
    resources.json
    outputs.json
```

---

## 12. Minimal Implementation Plan

### Phase 1
- ObservationBus
- FrameReport JSON dump

### Phase 2
- Renderer observations (passes, bindings)
- Resource tracking

### Phase 3
- ExpectationSpec + Verifier

### Phase 4
- Checkpoints + diffing tools

---

## 13. Key Principle

The system must answer:

**"Did the intended behavior actually happen, and how do we prove it with structured evidence?"**

This turns the engine into a **self-describing system** that can be interrogated by AI or tools.

