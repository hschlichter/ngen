# USD Payloads and Variants: Foundations for Scalable Scene Composition

## Overview

In Universal Scene Description (USD), **payloads** and **variants** are core composition mechanisms that enable scalable, modular, and efficient scene construction. While USD does not implement a streaming system itself, these features form the **foundation upon which streaming workflows are built**.

This document describes their behavior and how they support such systems purely from a USD perspective.

---

# Payloads

## Definition

A **payload** is a composition arc that allows a prim to reference external USD data that can be **loaded or unloaded on demand**.

### Example

```usda
def Xform "Tile_0_0"
{
    payload = @tile_0_0.usda@
}
```

## Key Properties

- **Deferred composition**: Payload content is not necessarily loaded when the stage opens
- **Explicit control**: Loading is controlled via API (`Load` / `Unload`)
- **Hierarchical**: Payloads apply to a prim and its entire subtree
- **External data source**: Typically references another USD file

## Behavior

- When a payload is **unloaded**:
  - The prim exists
  - Its contents do not participate in composition
- When a payload is **loaded**:
  - The referenced data is composed into the stage
  - All contained prims become visible and accessible

## Role in Scalable Scenes

Payloads enable:
- Partitioning large scenes into smaller units
- Avoiding unnecessary memory usage
- Deferring heavy composition work

They provide a **coarse-grained mechanism** for controlling scene population.

---

# Variants

## Definition

A **variant set** defines multiple mutually exclusive configurations for a prim. At any time, **only one variant is active**.

### Example

```usda
def Xform "Tree"
{
    variantSets = "lod"

    variants = {
        string lod = "high" (
            references = @tree_high.usda@
        )
        string lod = "low" (
            references = @tree_low.usda@
        )
    }
}
```

## Key Properties

- **Exclusive selection**: Only one variant is composed at a time
- **Non-additive**: Variants do not stack like references
- **Dynamic switching**: The active variant can be changed at runtime
- **Encapsulation**: Each variant can contain its own composition arcs (references, payloads, etc.)

## Behavior

- Only the **selected variant** contributes to the scene
- Unselected variants are:
  - Not composed
  - Not loaded
  - Not evaluated

## Role in Scalable Scenes

Variants enable:
- Multiple representations of the same asset
- Level-of-detail (LOD) configurations
- Alternative layouts or states

They provide a **fine-grained mechanism** for selecting between alternatives.

---

# Combining Payloads and Variants

Payloads and variants address different concerns and are often used together.

## Separation of Concerns

| Concern              | Mechanism |
|---------------------|----------|
| Scene population     | Payload |
| Representation choice| Variant |

## Example Structure

```usda
def Xform "Tile_0_0"
{
    payload = @tile_0_0.usda@
}
```

Inside `tile_0_0.usda`:

```usda
def Xform "Tile_0_0"
{
    variantSets = "lod"

    variants = {
        string lod = "high" (
            references = @tile_high.usda@
        )
        string lod = "low" (
            references = @tile_low.usda@
        )
    }
}
```

## Interpretation

- The **payload** controls whether the tile exists in the stage
- The **variant** controls which representation of that tile is active

---

# Composition Model

## Payloads

- Act as **optional composition branches**
- Can be included or excluded from the stage
- Operate at a **subtree level**

## Variants

- Act as a **switch between alternatives**
- Only one branch is active
- Operate at a **prim level**

## Interaction

- Variants may contain payloads or references
- Payloads may contain prims with variant sets
- Composition is resolved based on:
  1. Active variant selections
  2. Payload load state

---

# Basis for Streaming-Oriented Workflows

From a USD perspective, payloads and variants enable:

## Spatial Partitioning

- Scene divided into discrete units (e.g., tiles)
- Each unit represented by a payload

## Conditional Scene Population

- Payloads allow parts of the scene to be present or absent
- Stage composition reflects only loaded data

## Representation Switching

- Variants allow multiple representations of the same logical entity
- Only one representation is active at a time

## Hierarchical Control

- Payloads and variants can be nested
- Enables multi-level organization of scene complexity

---

# Important Characteristics

## Deterministic Composition

- The stage is fully defined by:
  - Layer stack
  - Variant selections
  - Payload load state

## Non-Destructive

- All composition is opinion-based
- Source assets remain unchanged

## Explicit Control

- No implicit behavior such as:
  - Automatic loading
  - Distance-based switching
  - Background streaming

---

# Summary

- **Payloads** provide:
  - Deferred, controllable scene population
  - Coarse-grained inclusion of data

- **Variants** provide:
  - Exclusive selection between alternatives
  - Fine-grained representation control

- Together, they form:
  - A compositional foundation for scalable scenes
  - A structure that supports streaming-like systems when paired with external logic

USD defines the **data model and composition mechanics**, while higher-level systems define **when and how** those mechanisms are used.

