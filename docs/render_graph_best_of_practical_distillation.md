# Render Graph Best-of (Practical Distillation)

## 1. Purpose

This document distills what actually matters when designing a render graph, based on common patterns across engines and public implementations.

---

## 2. Core Idea

A render graph is:

> A declarative description of frame work (passes + resources) that is compiled into an optimized execution plan with automatic synchronization and memory
> management.

---

## 3. The Only 5 Things That Matter

### 1. Pass Declaration (not execution)
- Describe what a pass does
- Do NOT execute immediately

### 2. Resource Usage
- Explicitly declare read/write access
- This is the foundation for everything else

### 3. Dependency Resolution
- Graph edges come from resource usage
- No manual ordering

### 4. Compilation Step
- Convert graph → execution plan
- Insert barriers
- Allocate resources
- Cull unused passes

### 5. Transient Resources
- Most resources live only for a frame
- Enables aliasing and memory reuse

---

## 4. Mental Model

### Wrong mental model
- “It’s a list of render passes”

### Correct mental model
- “It’s a compiler for GPU work”

---

## 5. Minimal Conceptual Model

```text
Pass
 ├── reads resources
 ├── writes resources
 └── execution function

Resource
 ├── logical handle
 └── usage history
```

Graph = passes + resource edges

---

## 6. What You Should NOT Focus On

Avoid overthinking:

- complex inheritance trees
- perfect abstraction of all APIs
- visual node editors
- representing every GPU feature

Focus instead on:

- correctness of dependencies
- simplicity of pass declaration
- clarity of resource usage

---

## 7. The Real Value of a Render Graph

### Automatic synchronization
- eliminates manual barrier management

### Resource lifetime tracking
- enables aliasing
- reduces memory usage

### Pass scheduling
- correct ordering
- possible async execution

### Debugging clarity
- graph view of frame

---

## 8. Common Design Pattern

```text
Build phase
    ↓
Compile phase
    ↓
Execute phase
```

### Build
- user defines passes and resources

### Compile
- resolve dependencies
- allocate resources
- insert barriers

### Execute
- record and submit commands

---

## 9. Key Data Structures

```cpp
struct Pass {
    ResourceHandle reads[...];
    ResourceHandle writes[...];
    ExecuteFn execute;
};

struct Resource {
    ResourceDesc desc;
    Lifetime lifetime;
};
```

---

## 10. Rules of Thumb

- Always declare resource usage explicitly
- Never manually order passes
- Keep passes small and focused
- Treat resources as logical, not physical
- Separate declaration from execution

---

## 11. Minimal Feature Set (Start Here)

You only need:

- pass list
- resource handles
- read/write tracking
- topological sort
- simple execution loop

Everything else can come later.

---

## 12. Advanced Features (Later)

- resource aliasing
- async compute scheduling
- multi-queue support
- pass culling
- pipeline state caching

---

## 13. Common Mistakes

- mixing execution and declaration
- exposing backend concepts too early
- overengineering resource system
- trying to support everything at once

---

## 14. One-Sentence Summary

> A render graph is a lightweight compiler that turns high-level rendering intent into efficient, correctly synchronized GPU work.

