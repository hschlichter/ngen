# World

The World is the runtime scene: every prim of a scene, composed from its scene packs (`src/asset/README.md`, "Scene packs"), with no USD
in it. `WorldComposer` composes it as USD composes a stage, for what the engine composes; `World::visit` walks it. Nothing here fetches:
the caller feeds the composer packs from the asset server.

| Files | What |
|---|---|
| `world.*` | `World`, `WorldContent`, `Entity` and the components, `visit` |
| `worldcomposer.*` | `WorldComposer`: layer packs to a `World` |
| `xformops.*` | a prim's local transform from its `xformOpOrder` |
| `fingerprint.h` | the fingerprint line `ngen-worldcheck` and `ngen-usdcheck` print |

## What a World holds

- **Every composed prim is an entity**, whatever its type or purpose: scopes, shaders, guides, classes, `over`-only prims, inactive prims
  (without children, as in USD). What is drawn is decided when the World is read (purpose, visibility, LOD levels), not by leaving entities
  out.
- **Instanced content is kept once.** An instance (an instanceable prim with arcs) names a prototype in `World::prototypes` and has no
  children of its own; instances with the same arcs share one prototype. A prototype's paths are relative to it (`/Top`); `visit` expands
  instances and gives every entity its full path.
- **An entity** has its name, type, parent and children, whether it is defined, abstract (a class) and active, its point count (point-based
  prims), its authored purpose and visibility, its local transform (types that are `UsdGeomXformable`), the mesh asset its geometry is and the
  material asset it is and, for a material, its own inputs as composed (`materialInputs`, its interface), and its material binding (the
  bound material as a key, `materialKey`: its pack's asset id and its inputs' values, `<asset>?<input>=<value>&…`, and
  `strongerThanDescendants`). A material that references another file's network and sets its inputs (a tile setting a library
  material's texture) is that file's pack with this one's values.
- **Components** beside the entities, per content: lights (UsdLux's inputs, with UsdLux's fallbacks), LOD roots (levels, default index, loaded levels,
  and the heuristic: its type, thresholds, `extent` with UsdLod's fallback, `projectionMethod`) and payload placements (asset ids, loaded,
  `extentsHint`).
- **Scene metadata**: the root layer's up axis and meters per unit, with USD's fallbacks (Y, 0.01) when it authors none; the root layer
  stack's asset ids.
- **`visit`** resolves down the hierarchy: world transforms (`!resetXformStack!` included), visibility (an invisible prim hides its
  subtree), purpose (inherited from the nearest ancestor that authors one), the bound material (the nearest binding, unless an ancestor's is
  `strongerThanDescendants`), and whether the entity is in a LOD level its root doesn't draw.

## Composition

`WorldComposer` follows USD's prim indexing (`pxr/usd/pcp`), so a World has the prims, types and opinions a USD stage of the same files
has:

- **A prim's index is a tree of nodes**, each a site (a prim path in a layer stack) that contributes opinions. The root node is the root
  layer stack at the prim's path; a reference or payload adds the index of its target, in the target file's layer stack, under the node
  whose spec authors it. Opinions are read from the nodes in strength order (the tree depth first, a node before its children; in a node,
  its layers strongest first), per attribute: the first opinion wins.
- **Siblings are ordered as USD orders them**: variants before references, references before payloads, an arc authored deeper in namespace before one
  coming from an ancestor, then in authored order. Arcs authored on several layers of a stack are taken strongest layer first, each once.
- **A child's index** is its parent's with every node's path extended by the child's name, plus the arcs authored at the child's path. A
  reference to a prim that isn't a root prim brings its ancestors' arcs too.
- **Namespace mapping**: a node maps its target's path to the authoring prim's; relationship targets (material bindings, LOD heuristics)
  are mapped through the nodes above the spec that authors them. An internal reference maps paths outside its target unchanged, as in USD,
  so a binding to a material outside the referenced prim still resolves.
- **The default prim**: a reference or payload naming no prim targets the target file's `defaultPrim`.
- **Variant sets**, as Pcp composes them: a prim's sets are those any node of its index authors (`variantSetNames`); each set's selection is
  the strongest `variants` opinion for it anywhere in the index, so a referencing layer's selection beats the asset's own, and a stronger
  sublayer's beats a weaker one's. The selected variant is a node under the one that authors the set, at its variant path
  (`/asset{shape=a}`), and its own arcs and sets compose with it (nested sets, a reference inside a variant). A reference's target leaves
  its own sets to the prim composed; the target's ancestors choose theirs where they are. A set with no selection composes no variant (no
  fallbacks), and a selection naming a variant that isn't authored is a message. The variant nodes are kept nodes of an instance, so
  instances with different selections have different prototypes.
- **Instancing**, as USD's `Pcp_ChildNodeIsInstanceable`: an instanceable prim is an instance when an arc authored on it brings specs. Its
  prototype keeps only those nodes (with specs, in the subtree of an arc authored on the instance) and composes once for every instance with
  the same ones; every other node is inert below the instance: the local layer stack, and arcs from the instance's ancestors (a model file's
  own children of a prim it references in). The instance itself keeps all its opinions.
- **Not composed**, as the packer reports: inherits, specializes, relocates, layer offsets. A prim inheriting from a class
  has the class's children and opinions in USD and not here.

Children are ordered by the strongest spec that names them, then the weaker ones'. USD orders a prim's children the same way when one spec
names them all, which is where order matters (a LOD root's levels).

## Loading: payloads and LOD levels

Every payload loads, except those under a LOD root's levels that aren't loaded. A LOD root (a prim with `LODRootAPI`) has its children as
levels, finest first; it loads the level it wants, `lod:default:index` until `setLodLevel` sets one for its path, and its coarsest level.
It draws the finest loaded level not finer than the wanted one, so a level still loading falls back to a coarser one.

As UsdLod has a renderer do it, a root's heuristic is the first of its `lod:heuristics` targets of a type and domain the engine reads
(`LODScreenSizeHeuristic`, `imaging`) with valid bounds (finite, not empty), as UsdLod has a renderer ignore a heuristic that gives no usable result;
a root with none keeps its default (`LodRoot::heuristicChosen` false; the packer warns about an empty or non-finite extent). The heuristic's bounds
are its `boundingVolume`'s extent when that names a boundable with a valid one, else its own `extent`, in the root's space as UsdLod's query reads
them. A level's own authored visibility is for readers that don't select LOD (UsdLod's recommendation is the default level visible, the others
invisible): the level the engine draws is shown whatever it authors, and visibility below it applies as usual (`Entity::lodItem`).

A LOD root inside a prototype loads, in the prototype, every level any of its instances wants; which level each instance wants and draws
is state of the instance (`World::lodLevels`, by the root's full path), outside the prototype. USD instead gives instances with different
loaded levels different prototypes.

**A level is a content of its own.** A LOD root's children are stand-in entities, one per level; each is linked (`Entity::linkedContent`) to
a content in `World::linkedContents` whose entity 0 is the level prim itself, composed loaded or not (a loaded level's payload brings opinions
for the level prim too), with everything under it. `visit` walks a level as that content. Which levels load is decided after composing, by
linking (resolution): so changing a level never changes the content its root is in, only which level content its stand-in links to.

**A prim with payloads is a content of its own too**, linked the same way (to `World::linkedContents`) whenever its parent's content loads
payloads: its entity 0 is the prim with its payloads' opinions, everything under it below. So a pack arriving for a payload recomposes that
payload's content, not the content it sits in; a district's layout, thousands of payloads, is composed piece by piece as its packs arrive.

## Composed content is kept across composes

The stage is composed again every compose; prototypes and level contents are kept (`World::prototypes`, `World::linkedContents`, at stable
indices) and reused by key: a prototype by its kept nodes, a level by its prim index (every node's site, arc and namespace mapping) and
whether it loads. A content composed while a pack it named wasn't held (a gap) is composed again, in place, the next time it is reached;
one that had every pack is reused as it is, since a pack arriving later is one it didn't name. Layer stacks are kept too, as the kept
contents' indexes point at them, and rebuilt in place when one missed a sublayer. A pack that changes (`addPack` of an id already held: a
repack or an edit) drops every kept content and rebuilds every stack: contents don't record which packs they read. `stats()` says what the
last compose composed, kept and reached, and how long the stage, the resolution and the bindings took.

Every content has a `version`, new each time it is composed, and `complete` once it was composed with every pack it named held (or
failed): a reader can keep what it derived from a content by version, and wait for a complete one. `evict(unusedFor)` lets go of the kept
contents no resolution has reached for that long, and not by the latest one: each is emptied in place (its index stays, a new version) and
composed again if it is reached later. The caller decides the delay; ngen-view uses its unload delay.

Material bindings are resolved every compose, in their own content first and then anywhere in the World by full path, as a target may be in
another content (a level's material outside the level).

## Composing in rounds

`compose()` composes from the packs held and returns the layer packs it names and doesn't hold: sublayers, references, and the payloads it
loads. An arc to a pack that isn't held yet composes as a gap. The caller fetches what is needed, `addPack`s it and composes again until
nothing is needed; each round goes one level of arcs deeper. A pack the server couldn't provide is `markFailed`, and its arcs compose as if
they weren't authored. `messages()` lists what the last compose couldn't compose: a reference to a file with no default prim, a target
that isn't there, an arc cycle, a binding to a prim that isn't there.

## Checking a World against USD

`ngen-worldcheck` (`src/apps/worldcheck.cpp`) composes a scene from its packs, fetched from its variant's asset server, and prints its
fingerprint; `ngen-usdcheck` (`src/apps/usdcheck.cpp`, linking OpenUSD) opens the same scene with OpenUSD, loads the same payloads, and
prints USD's. The two are compared as files:

```sh
./_out/linux-vulkan/release/ngen-usdcheck --transforms --output /tmp/usd.txt assets/layered/root.usda
./_out/linux-vulkan/release/ngen-worldcheck --transforms --output /tmp/world.txt assets/layered/root.usda
cmp /tmp/usd.txt /tmp/world.txt
```

A fingerprint (`fingerprint.h`) is one line per prim, instances expanded, sorted: its path, its type and its point count (0 unless it is
point-based), and with `--transforms` its world matrix. `--lod <root path>=<level>` sets a LOD root's level before composing, in both tools;
`--instancing` prints instance and prototype counts instead. Both run in the asset server's directory; `ngen-usdcheck` reads the files from
disk there. A prim the packer warned about (an inherit, an `over` authoring geometry) differs where the warning says it does.
