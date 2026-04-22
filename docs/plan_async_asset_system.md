# Async Asset System — Design Plan

Iterable. Push back on anything.

## 1. Why now

Cold-start is already visible: opening a non-trivial USD scene runs the extractor, builds meshes, decodes textures, rebuilds the BVH — all on the main thread, all synchronous. Shaders currently rely on a Makefile rule (`glslc`) to pre-produce `.spv` files; works fine for a fixed set, but there's no content-hash invalidation, no async recompile, and nothing to extend to new asset types.

We want:

1. **Fast warm loads.** An already-cooked asset should open in the time it takes to mmap a file.
2. **Incremental cold loads.** If only one asset changed, cook only that one.
3. **Async by default.** The engine requests, a worker cooks or loads, the engine keeps running.
4. **Extensible.** Adding a new asset type (texture, animation, font) shouldn't require plumbing changes through the engine.
5. **Decoupled from the engine binary.** Long-term, a separate `ngen-cook` CLI can pre-bake an entire project with no Vulkan/SDL dependencies pulled in.

"Super fast" specifically means: the hot path is `open(file) + mmap + reinterpret_cast`. No parsing, no decompression, no format conversion. Anything heavy happened once, ahead of time, in a cook step.

---

## 2. Goals and non-goals

**In scope (Phase 1):**
- `src/assetsystem/` module: cooker registry, content-hash cache, async load queue.
- USD cooker (extractor output → mmap-friendly blob).
- Shader cooker (GLSL → SPIR-V → hashed cache).
- Async load API consumable from renderer/scene.

**In scope (Phase 2+):**
- Texture cooker (stb decode + mipmap + BC7 compression).
- `ngen-cook` standalone CLI that cooks a whole project.
- Hot reload (source changes → re-cook → live swap).

**Explicitly not in scope:**
- Replacing USD as the scene authoring surface. USD stays the edit target; cooking only caches the *derived* products of USD (mesh buffers, BVH, resolved bindings).
- Any new runtime serialization format for live scene data (snapshots, network sync).
- Packing multiple assets into archives / bundles. One asset → one cooked file, at least initially.
- Cache eviction / garbage collection. Let `.cache/` grow; manual `rm -rf` to prune.

---

## 3. Architecture overview

Four layers, split into two libraries so the engine can ship without the cook path:

```
┌─────────────────────────────────────────────────┐
│  Engine (renderer, scene)                       │
│      ↕                                          │
│  AssetSystem (src/assetsystem/runtime/)              │
│      • load-only API, mmap, AssetHandle<T>      │
│      • ICookerDispatcher hook (null in runtime) │
│      ↕                                          │
│  Cooker library (src/assetsystem/cookers/)  ← editor-only
│      ├── Cooker registry                        │
│      ├── Content-hash computer                  │
│      ├── Cooker impls: USD, shader, ...         │
│      └── EditorCookerDispatcher                 │
└─────────────────────────────────────────────────┘
```

**Two-library rule.** `src/assetsystem/runtime/` is **always linked** — into both the editor engine and any runtime-only engine variant. It handles *reading* cached blobs: mmap, header validation, `AssetHandle<T>`. `src/assetsystem/cookers/` is **editor-only** — cookers, registry, content hashing, shaderc, stb, pxr. A runtime-only engine never links it. See §12 for the runtime-only build.

The two libraries meet at a single interface: `AssetSystem::setCookerDispatcher(std::unique_ptr<ICookerDispatcher>)`. Editor builds install a concrete dispatcher at init; runtime builds don't. Cache miss behavior branches on whether a dispatcher is installed — no `#ifdef`s in the consumer API.

**Layering rules (invariants, not aspirations):**
- The *runtime* library depends on nothing in `src/rhi/`, `src/renderer/`, SDL, USD, shaderc, or stb. Only the stdlib + JobSystem.
- The *cooker* library depends on the runtime library + pxr + shaderc + stb. Nothing engine-side.
- That means `ngen-cook` links only cooker + runtime; a runtime-only engine links only runtime + engine.

### Directory layout

```
src/assetsystem/
  runtime/                 # always linked
    cookedformat.h         # CookedHeader, offset helpers (read-only)
    mmapfile.h/.cpp        # platform mmap wrapper
    cache.h/.cpp           # .cache/ lookup by hash (read-only)
    assethandle.h          # AssetHandle<T>
    assetsystem.h/.cpp     # engine-side API; installable dispatcher hook
    cookerdispatcher.h     # ICookerDispatcher interface
  cookers/                 # editor-only
    cooker.h               # ICooker interface, CookInput/CookOutput
    cookerregistry.h/.cpp  # register-by-extension, lookup
    contenthash.h/.cpp     # SHA-256 of input + dep bytes (cook-side)
    editordispatcher.h/.cpp  # EditorCookerDispatcher — impl of ICookerDispatcher
    impls/
      usd.h/.cpp           # UsdCooker (uses pxr)
      shader.h/.cpp        # ShaderCooker (uses shaderc)
      texture.h/.cpp       # TextureCooker (Phase 2; uses stb + ISPC texcomp)
  cook_main.cpp            # ngen-cook CLI entry (separate Makefile target)
```

---

## 4. The `ICooker` interface

```cpp
struct CookContext {
    std::string_view sourcePath;        // "assets/three_cubes.usda"
    std::span<const std::byte> source;  // pre-read bytes
    std::function<std::vector<std::byte>(std::string_view dep)> resolveDep;
};

struct CookOutput {
    std::vector<std::byte> cookedBlob;       // the mmap-target bytes
    std::vector<std::string> depPaths;       // "assets/mat.jpg", recorded in cache manifest
    uint32_t formatVersion = 0;              // bumps invalidate cache across engine upgrades
};

class ICooker {
public:
    virtual ~ICooker() = default;
    virtual auto cookerId() const -> std::string_view = 0;   // "usd", "shader", etc.
    virtual auto cookerVersion() const -> uint32_t = 0;       // bump on cooker logic change
    virtual auto cook(const CookContext& ctx) -> std::expected<CookOutput, std::string> = 0;
};
```

**Why hand-rolled instead of flatbuffers/capnp for the blob:**
- The project already leans on hand-rolled POD + `reinterpret_cast` patterns. Matches house style.
- Exact layout control — the cooked format is the contract, not generated code.
- No new submodule, no codegen step, no schema language to learn.
- Trade-off: manual alignment/endianness discipline. Mitigation: assume host-endian, refuse to load cross-endian blobs (the cache is a local dev convenience, not a distribution format).

Revisit if/when the blob shapes start to rot. Flatbuffers is a fine escape hatch.

---

## 5. Cooked format principles

Every cooked blob starts with a fixed header:

```cpp
struct CookedHeader {
    char magic[8];           // "NGENCOOK"
    uint32_t formatVersion;  // cooker's formatVersion
    uint32_t cookerIdHash;   // FNV-1a of cookerId
    uint64_t sourceHashLo;   // content hash of (source + deps + cooker version)
    uint64_t sourceHashHi;
    uint64_t bodyOffset;     // where the cooker-specific payload starts
    uint64_t bodySize;
};
```

**Body layout rules:**
- Everything is byte-aligned to the field type's alignment (not packed).
- Pointers are stored as **self-relative `int64_t` offsets** (bytes from the offset field to the target), so the blob is position-independent — mmap and cast and it works.
- Strings are length-prefixed `{uint32_t len; char data[];}`; no null terminators.
- Arrays are `{uint32_t count; int64_t offsetToFirst;}` pointing into a separate data region.

Readers don't allocate. A cooker picks a root type (e.g. `CookedUsdFile*`, `CookedShader*`) and the consumer gets a `const T*` over the mmap'd region — walk the structure directly.

Host-endian assumption documented in `cookedformat.h`. The `magic` field is used to detect mismatched endianness; loading refuses rather than misinterpreting.

---

## 6. Content-hash cache

Layout:

```
.cache/
  manifest.json              # optional human-readable index for debugging
  objects/
    ab/
      abcdef…0123.ngenobj    # SHA-256-keyed cooked blob
  sources/
    abcdef…0123.meta         # {cookerId, sourcePath, depPaths[], cookerVersion, timestamp}
```

**Hashing rule.** Cache key = SHA-256 of:
1. `cookerId` + `cookerVersion` (bumping either invalidates everything that cooker produced)
2. Source file's content
3. Each declared dep's content (recursive — a shader `#include`'d header changing invalidates the shader)

Key is computed *before* cooking. If `objects/<key>.ngenobj` exists and header's hash matches, skip the cook entirely — hot path is `stat + mmap`.

**Location.** `.cache/` in the repo root by default. Override via `NGEN_ASSET_CACHE=/tmp/cache` env var. CI can share a cache across runs if it wants. Don't check `.cache/` into git (add to `.gitignore`).

---

## 7. Dependency tracking

Each cooker is responsible for enumerating its deps and handing them to the cache hasher.

**USD cooker:** Walks sublayer / reference / payload arcs via `pxr::UsdStage::GetUsedLayers()`. For every referenced layer, record its identifier and hash it. Textures referenced in `UsdShadeMaterial` count too.

**Shader cooker:** Either (a) use `shaderc`'s include callback to capture every resolved `#include`, or (b) run a pre-pass regex scan. (a) is more accurate.

**Texture cooker:** Usually dep-free (input is self-contained), unless we chain texture atlas configs.

The key design point: deps are *inputs to the hash*, not post-hoc metadata. Stale-on-change falls out of the hash.

---

## 8. Async load API

Engine-side consumer surface (`src/assetsystem/runtime/assetsystem.h`):

```cpp
template <class T>
class AssetHandle {
public:
    bool ready() const;
    const T* get() const;  // nullptr until ready
};

class AssetSystem {
public:
    static void init(JobSystem* jobs);
    static void shutdown();

    // Request a cooked asset. Returns immediately with a pending handle;
    // cook (if needed) + mmap happens on a background job.
    template <class T>
    static AssetHandle<T> load(std::string_view sourcePath);

    // Block until ready. For startup paths; prefer polling during the frame loop.
    template <class T>
    static const T* loadSync(std::string_view sourcePath);
};
```

Under the hood:
- `load()` enqueues a job on JobSystem that resolves the cooker, hashes deps, checks cache, cooks if necessary, mmaps the result, populates the handle.
- `AssetHandle` is cheap to copy — it's a shared pointer to a small `AssetState` struct holding `std::atomic<bool> ready`, the `mmap` handle, and a `T*` view.
- Consumer reads `ready()` each frame; uses `get()` only when true.

**Threading.** JobSystem is fine for Phase 1 — cooks are bursty, not sustained. If cooks ever block worker throughput for regular engine jobs, split into a dedicated small pool.

**Cancellation / eviction.** Out of scope for Phase 1. Loaded assets stay until the process exits; `.cache/` persists across runs.

---

## 9. Per-asset-type specifics

### 9.1 USD

USD stays the authoring/edit surface (parent rule: USD is the scene, Model A). We don't bake it away. What we cache is the *expensive derived data* the extractor produces from USD, at a granularity that supports both fast cold loads and — critically — **fast live edits**.

#### Why live-edit speed is a first-class goal

Adding a single cube in the editor today takes noticeably long. The cost isn't USD composition (pxr is fast at that); it's the downstream work that fires after every composition-visible change:

- `USDRenderExtractor::extract(...)` — walks the stage and re-extracts mesh/material data. Currently O(stage size), invoked whole.
- `SceneQuerySystem::rebuild(...)` — rebuilds the BVH from scratch. Also O(stage size).

Both run whether one prim changed or a thousand. For a scene with serious geometry, adding a cube pays for extraction across the whole stage. The asset system has to be shaped so this cost stops scaling with *total* scene size and starts scaling with *changed* prims.

Cooking on its own doesn't achieve that — it's a cold-load win. The combination that does achieve it:
1. Per-prim cached mesh/material data, random-access by prim path within the cache.
2. Incremental extract that consumes `dirtySet()` and only hydrates changed prims from the cache.
3. Incremental BVH update (insert/refit for dirty leaves, not full rebuild). *Note: BVH incrementalization is a separate workstream from the asset system; flagged below as a follow-up. The asset system makes it possible, it doesn't do it.*

#### Cook unit and cache granularity

**Cook unit = a referenceable `.usda` file.** Each file that's either a reference target or the top-level scene gets its own cache entry. Shared assets (a chair referenced from 500 scenes) cook once; editing a chair re-cooks only `chair.usda` and leaves every referencing scene's cache intact.

**Each cache entry is a prim-indexable blob, not monolithic.** The blob has a header section mapping "prim path within this file → offset to that prim's mesh/material data." Hydrating one prim is `mmap` + table lookup + copy-into-MeshLibrary — no walk, no scan. Hydrating N prims is N lookups into the same mapped region.

Contents of a cooked file entry:
- For every mesh prim defined locally in the file: vertex/index buffers, vertex stride, topology, primvar layout.
- For every material defined locally: resolved `UsdShade` inputs, texture references (as content hashes of the cooked texture entries — not embedded bytes).
- Prim hierarchy (parent/child/name, local-space default transforms, flags).
- An in-file prim-path → data-offset index.

Explicitly *not* in the entry:
- World-space transforms (depend on where the asset is placed — scene concern, not asset concern).
- Composition state (who references whom).
- Referenced files' data (entries link to each other by content hash, not by embedding).
- BVH (depends on composed transforms, built at load time).

#### Primshapes (cube / sphere / cylinder / cone) are cooked too

`src/scene/primshapemesh.cpp` generates these procedurally today. Under the asset system they become cache entries keyed by synthetic identifiers (`"primshape:cube"`, `"primshape:sphere-32"`, etc.) and hashed by the generator version. Adding a cube then becomes: resolve `primshape:cube` → cache hit → hydrate — no extract call, no full-scene work. This is the mechanism that makes simple edits feel instant.

Same path for user-imported geometry: the geometry lives in some `.usda` file, which has a cached entry, which has a per-prim index.

#### Load flow (cold open)

1. Open the scene `.usda` with USDScene (pxr layer parse — cheap).
2. Composition resolves: sublayers, references, payloads, variants applied. USDScene already does this.
3. For every prim in the composed stage, look up its source file's cache entry by content hash. Mmap it if not already mapped.
4. Index into the mapped entry for that prim's mesh/material data. Hydrate MeshLibrary / MaterialLibrary directly.
5. Build BVH from hydrated meshes and composed transforms.

No `USDRenderExtractor::extract` walk. The extractor becomes the fallback for cache misses — it runs per-prim (for the missing prims only), produces the derived data, *and* writes back to the cache for next time.

#### Load flow (live edit)

User adds a cube:

1. `USDScene::createPrim` runs. Pxr authors a new prim. (Edit stays in USD — single source of truth.)
2. `processChanges()` → `dirtySet()` reports the new prim in `primsResynced`.
3. Incremental extractor walks *only the dirty prims*. For the new cube prim, it resolves to `primshape:cube`, hits the cache, hydrates into MeshLibrary.
4. RenderWorld patches that one new instance.
5. SceneQuery inserts one BVH leaf (once the BVH is incrementalized; before that, rebuild remains the bottleneck).

Total work scales with "one prim added," not with stage size. That's the win the asset system enables.

#### Edits that change a cached file

If the user's edit *modifies the contents of a file* (not just adds instances that reference existing files — e.g., they edit a material parameter on a prim defined in the scene file itself), the cache entry for that file is stale. Two choices:

- **Invalidate + re-cook on next cold open.** Simplest. Live editing still works — the extractor fallback runs for dirty prims on every edit, using whatever's freshest in USD. But the cached entry for the edited file is out of date until the next save-and-reopen.
- **Incremental re-cook.** On commit of an Authoring edit, kick an async job that re-cooks just the affected file. Cache stays fresh during the session.

Start with the first, move to the second if re-cook latency across reopens hurts. Both are compatible with the structure above.

---

**Summary of what the asset system buys and doesn't buy:**

| Problem | Fixed by asset system? |
|---|---|
| Cold scene open re-runs full extractor | Yes — cache-hit path skips extract for unchanged prims. |
| Adding a cube re-runs full extractor | Partial — the extractor must become incremental (consume `dirtySet()`), and it hydrates from the cache when it can. Asset system provides the cache and enables the rewrite. |
| `SceneQuerySystem::rebuild` is O(N) on every edit | **No.** BVH incrementalization is a separate workstream. Flagged here so we don't confuse scope: live edits won't feel instant until BVH updates are incremental *and* the extractor is incremental. |
| Identical assets referenced across scenes cost repeated work | Yes — content-hash addressing ensures one cook per unique file content. |

### 9.2 Shader

Replace the Makefile `%.spv: %` rule with a cooker invocation (still from the Makefile, but calling `ngen-cook` for shaders specifically, or a library entry point). Inputs:
- Source `.vert` / `.frag` / `.comp`.
- Resolved `#include` headers (tracked as deps).

Output:
- Cooked blob = `CookedHeader` + SPIR-V bytes. Readers cast to `const uint32_t*` starting at `bodyOffset`.

`RhiDevice::createShaderModule` changes signature slightly (or grows an overload) to accept cooked bytes instead of a filepath. The engine asks AssetSystem for the shader; AssetSystem returns the mmap'd SPIR-V; RHI consumes it.

Uses `shaderc` (Google) for the compile — vendored as a submodule. Drops the external `glslc` CLI dependency and gives us the include callback.

### 9.3 Textures (Phase 2)

Currently: stb_image decode inline, raw RGBA8 to GPU, no mipmaps, no compression.

Cooked texture:
- BC7 (or BC5 for normal maps) compressed, sized to a power-of-two, mipmap chain pre-generated.
- Body layout: `{uint32_t mipCount; MipInfo mips[]; uint8_t blob[];}` — RHI uploads one contiguous blob.

Trade-off: BC7 encode is slow (~seconds per 4K texture). That's exactly why cooking upfront is the answer.

Compressor choice: ISPC texcomp (fast BC7) — vendor as submodule. Alternative: `bc7enc_rdo` (header-only). Phase 2 decision.

---

## 10. Observability hooks

The asset system makes several claims about runtime behavior — "warm loads skip cooking," "live edit hydrates one prim," "runtime builds never cook" — that can only be confirmed by watching actual emission streams. Each hook below exists so a specific claim is *checkable* from a `.jsonl` dump without having to trust the implementation.

Category: **`"Assets"`** — a new canonical category alongside `"Scene"`, `"Render"`, `"Engine"`. Add to the canonical set documented in `obs.md` when Phase 1 lands.

### 10.1 Core emissions (Phase 1)

All emitted from the runtime library — works in editor and runtime-only builds.

| Type | When | Fields | Name |
|---|---|---|---|
| `AssetRequested` | Consumer calls `AssetSystem::load` / `loadSync` | `source_path`, `cooker_id` | `source_path` |
| `CacheHit` | Resolver found matching entry in `.cache/` | `source_path`, `cache_key` (short hex), `cooker_id`, `bytes` | `source_path` |
| `CacheMiss` | No entry, or stored hash didn't match | `source_path`, `cache_key`, `cooker_id`, `reason` (`"missing"` / `"source_changed"` / `"dep_changed"` / `"cooker_version_bumped"` / `"format_version_bumped"`) | `source_path` |
| `AssetMmapped` | Cache file mapped | `cache_key`, `bytes` | `cache_key` |
| `AssetNotCooked` | Cache miss, no dispatcher installed (runtime-only) | `source_path`, `expected_cache_key` | `source_path` |

**Claim checks these underwrite:**
- *"Warm runs are stat+mmap."* → After an asset has been loaded once, subsequent loads emit `CacheHit` + `AssetMmapped` with zero `CacheMiss` / `CookStarted`.
- *"Runtime builds never cook."* → A runtime binary emits `AssetNotCooked` on miss rather than any `CookStarted`. Presence of `CookStarted` in a runtime stream is a bug.

### 10.2 Cooker emissions (Phase 1, cookers library only)

Emitted from the editor-side cooker library. Never present in a runtime-only stream.

| Type | When | Fields | Name |
|---|---|---|---|
| `CookStarted` | Dispatcher invokes `ICooker::cook` | `source_path`, `cooker_id`, `cooker_version` | `source_path` |
| `CookFinished` | Cooker returned success | `source_path`, `cooker_id`, `output_bytes`, `dep_count` | `source_path` |
| `CookFailed` | Cooker returned an error | `source_path`, `cooker_id`, `error` | `source_path` |
| `CookInvalidated` | Dep hash changed, triggering a re-cook | `source_path`, `cooker_id`, `dep_path` (the dep that changed) | `source_path` |

**Claim checks:**
- *"Editing a shader header re-cooks dependent shaders."* → Touching `common.glsl` produces `CookInvalidated{dep_path: "common.glsl"}` for each shader that includes it, then `CookStarted` / `CookFinished` for each.
- *"No cook runs when inputs haven't changed."* → Zero `CookStarted` in a re-run after a clean first run.

### 10.3 USD-specific emissions (Phase 2)

Emitted from the extractor and `UsdCooker`. These are the hooks that verify the fast-edit story — without them, the claim that "adding a cube doesn't re-extract the stage" is unverifiable.

| Type | When | Fields | Name |
|---|---|---|---|
| `ExtractIncremental` | Incremental extractor completes a dirty-only pass | `dirty_count`, `stage_prim_count`, `cache_hits`, `cache_misses` | `"USDRenderExtractor"` |
| `ExtractFull` | Fallback full extraction ran | `prim_count`, `reason` (`"cold_open"` / `"cache_unavailable"` / `"first_run"`) | `"USDRenderExtractor"` |
| `AssetResolved` | A prim was resolved to a cache entry | `cache_key`, `cooker_id` | prim path (USD) or synthetic key (`"primshape:cube"`) |
| `PrimHydrated` | Mesh/material data copied from cache into runtime libs | `source` (`"cache"` / `"extract"`), `cache_key` | prim path |

`PrimHydrated` is the chattiest of the set — fires once per prim on cold open and once per dirty prim on edit. Acceptable at current scales (a scene with 10k prims → 10k hydrations on cold open) but the plan should revisit if this ever sits inside a per-frame hot path (it won't, but worth writing down).

**Claim checks:**
- *"Cold load of a warm-cache scene skips the extractor walk."* → Stream has many `AssetResolved` + `PrimHydrated{source: "cache"}`, zero `ExtractFull`, and at most one `ExtractIncremental` with `cache_hits == stage_prim_count` and `cache_misses == 0`.
- *"Adding one cube hydrates exactly one prim."* → After the edit, stream has one `ExtractIncremental{dirty_count: 1}` followed by one `PrimHydrated{name: "/World/Cube1", source: "cache"}`. If `dirty_count > 1` or the extractor emits anything else, the incremental path is leaking.
- *"Primshapes are cache-backed, not regenerated inline."* → Adding a cube produces `AssetResolved{name: "primshape:cube"}` + `CacheHit`. If it produces `CookStarted{cooker_id: "primshape"}` only on first-ever cube, fine; on every add, bug.
- *"Editing a referenced asset invalidates only that file."* → Save a change to `chair.usda` + reopen → stream shows `CacheMiss{source_path: ".../chair.usda", reason: "source_changed"}` once, `CacheHit` for every other referenced file.

### 10.4 Runtime vs editor stream differences

By construction:

- **Editor stream** sees all three groups (10.1–10.3).
- **Runtime-only stream** sees 10.1 only. The presence of *any* 10.2 emission in a runtime-only stream means cooker code leaked into the runtime build and the CI link-check missed it.

That invariant doubles as an integration test: run `ngen-runtime`, `jq -c 'select(.category=="Assets" and (.type=="CookStarted" or .type=="CookInvalidated" or .type=="CookFinished"))'`, assert empty.

### 10.5 What's intentionally not emitted

- **Per-byte / per-mmap-page detail.** `AssetMmapped` fires once per cache file, not per access.
- **Cache pruning / GC.** Out of scope for Phase 1 — no events needed until pruning exists.
- **Individual dep enumeration during cook.** Deps are a field on `CookFinished` (count) and `CookInvalidated` (the specific dep that changed). No per-dep-scanned emission — that would be chatty and uninteresting.
- **Load timings.** Observations describe what happened, not how long it took (parent observability plan §8). If we ever need latency, that goes through a separate profiling system.

---

## 11. Standalone `ngen-cook` CLI

Separate Makefile target, separate executable, same cooker library.

```
$ ./ngen-cook assets/                    # cook everything discoverable
$ ./ngen-cook assets/scene.usda          # cook one file
$ ./ngen-cook --check assets/            # verify cache, exit non-zero if stale
$ ./ngen-cook --clean                    # blow away .cache/
```

**Link rules.** `ngen-cook` links only the cooker library + pxr + shaderc + stb. No SDL, no Vulkan, no renderer code. That's the test that keeps the cooker library honest — if it compiles, the separation is intact. Add a CI target that builds just `ngen-cook` to enforce it.

**CI / build integration.** Eventually, the main `make` rule depends on `ngen-cook assets/` having run. Cold checkout: one full cook, then the engine opens instantly. Subsequent builds touch only changed assets.

---

## 12. Runtime-only engine variant

The engine currently bundles editor + debug tools. A shipping / runtime variant must load pre-cooked assets without carrying the cook path along for the ride: no shaderc, no pxr, no stb, no cooker registry, no content hashing. If an asset isn't already in `.cache/`, it's a hard error, not a just-in-time cook.

### 12.1 What splits

| Subsystem | Editor build | Runtime build |
|---|---|---|
| `src/assetsystem/runtime/` | linked | linked |
| `src/assetsystem/cookers/` + impls | linked | **not linked** |
| shaderc / pxr (cook-side usage) / stb | linked | **not linked** |
| `AssetSystem::setCookerDispatcher(...)` call | called at init | **never called** |
| Cache miss behavior | dispatch to cooker | hard error, logged and returned to caller |

The runtime binary can still open a `.usda` at runtime *iff* USDScene is in scope for runtime builds — that's a separate decision (§12.4).

### 12.2 How the split is enforced

**No `#ifdef`s.** The split is library membership, not preprocessor conditionals. Two mechanisms together:

1. **Linker-level:** `src/assetsystem/cookers/` compiles into its own object set. The Makefile has two all-targets:
   - `make` / `make editor` — builds `ngen-editor` = runtime objs + cookers + engine + editor UI.
   - `make runtime` — builds `ngen-runtime` = runtime objs + engine (minus editor UI) + a stub `main` that never installs a dispatcher.

2. **Init-site:** `ngen-editor`'s `main.cpp` calls `AssetSystem::setCookerDispatcher(std::make_unique<EditorCookerDispatcher>())`. `ngen-runtime`'s `main.cpp` doesn't. That one line is the entire runtime/editor distinction at the call site.

When no dispatcher is installed, `AssetSystem::load()` on a cache-miss returns an `AssetError::NotCooked` result with the expected cache path. The runtime engine decides what to do — log + fatal for Phase 1, graceful fallback to a "missing asset" placeholder later.

### 12.3 Enforcing the invariant at build time

Because the split is library-membership, a stray `#include "cookers/shader.h"` from engine code would link-fail the runtime build but succeed the editor build — silent until someone tries to ship. Two-line defense:

- Add a CI target that builds `ngen-runtime` on every PR. A runtime-build failure means someone added a cooker dep to engine code that shouldn't have it.
- A simple script (`scripts/check_runtime_deps.sh`) runs `nm` on the runtime binary and fails if any `cookers::` symbol appears. Belt and suspenders.

### 12.4 Editor-only engine subsystems

Beyond the asset-cooker split, there are other subsystems today that a runtime build wouldn't want:

- `src/ui/` (editor UI windows — scene tree, properties, layers, asset browser, etc.)
- `src/renderer/passes/editoruipass.cpp` (the frame-graph pass that draws ImGui)
- ImGui itself, `rhieditoruivulkan.cpp`
- Gizmo passes? Debatable — in-game debug overlays might still want them.

These are **out of scope for this plan** but follow the same separation pattern: move them into a clearly-bounded dir (probably rename `src/ui/` → `src/editor/`) and exclude that dir's objs from the runtime Makefile target. A follow-up plan (`plan_runtime_engine_split.md`) should cover the full editor/runtime engine-side split, since it touches a lot more than assets.

For *this* plan, the cooker-library split is the load-bearing piece: it guarantees the asset system doesn't block a runtime build from ever existing. The rest is a straightforward follow-up.

### 12.5 Does the runtime build still need USDScene?

Open question. Two paths:

- **Yes, slim USDScene.** Runtime USD opens the `.usda`, but the per-file cache entries (§9.1) provide all the mesh/material data — USDScene is essentially just layer parse + composition. Still pulls pxr as a dep. Editing APIs are `#define`d out or stripped.
- **No, cooked-only scene.** Runtime reads only cooked cache entries, plus a small cooked manifest describing the composed stage (prim tree + references + resolved transforms). No pxr dependency. USDScene becomes editor-only. Needs a small `CookedSceneLoader` that produces the same MeshLibrary / MaterialLibrary / SceneQuery shape USDScene+extractor produce today.

Leaning **cooked-only** for the runtime variant — dropping pxr saves ~tens of MB of linked code and makes the runtime build genuinely lean. But that forces the cache format to be complete enough to stand on its own (including a cooked "scene manifest" describing composition results), which pressures §9.1's design. Worth deciding before Phase 2 lands, not before Phase 1.

---

## 13. Rollout phases

**Phase 1 — Core skeleton + shaders + library split.**

Smallest useful step, proves the mmap + hash-cache path end-to-end on the simplest asset type. Establishes the runtime/cooker library boundary from day one.

- `src/assetsystem/runtime/` and `src/assetsystem/cookers/` as separate object sets; enforce no-cross-include rule.
- `ICookerDispatcher` interface in runtime; `EditorCookerDispatcher` in cookers; `AssetSystem::setCookerDispatcher` hook.
- `ShaderCooker` (shaderc vendored as submodule).
- `AssetSystem` with sync `loadSync<T>()` only — proves the loading path without introducing async complexity.
- Port all 9 shaders off the Makefile rule onto the cooker. Delete `%.spv: %` and `SHADERS_SPV` from the Makefile.
- `.cache/` added to `.gitignore`.
- **Observation hooks:** §10.1 core emissions (`AssetRequested`, `CacheHit`, `CacheMiss`, `AssetMmapped`, `AssetNotCooked`) and §10.2 cooker emissions (`CookStarted`, `CookFinished`, `CookFailed`, `CookInvalidated`) wired in. New `"Assets"` category added to the canonical set in `obs.md`.

**Success criterion:** engine runs, shader reloads are `stat + mmap`, first-run cooks take the expected hit, re-run is instant. The cooker library and runtime library have zero cross-source includes going the wrong direction. Verified by reading the `.jsonl`: cold run has `CookStarted` × 9, warm run has `CacheHit` × 9 and zero `CookStarted`.

**Phase 2 — USD cooker + async.**

- `UsdCooker` producing per-file cache entries (prim-indexable mesh/material blobs, §9.1).
- Primshape cooker for cube/sphere/cylinder/cone (`primshape:*` synthetic keys).
- **Incremental extractor.** `USDRenderExtractor::extract` rewritten to consume `dirtySet()` and hydrate only dirty prims from the cache. Falls back to in-place extraction + cache-write on cache miss. This is what makes "add a cube" stop costing O(stage). Called out as the phase's headline outcome.
- Engine `openScene` consults the cache before running any extraction.
- `AssetSystem::load<T>()` async variant — JobSystem-backed, poll-by-ready.
- §10.3 USD-specific observation hooks wired in (`ExtractIncremental`, `ExtractFull`, `AssetResolved`, `PrimHydrated`). These are the emissions that let us verify the fast-edit claim — an add-cube test becomes a one-line `jq` check.
- Decision land on §12.5 (runtime needs slim USDScene or cooked-only).

**Verification scenarios:**
- **Cold open of an un-cached scene:** stream contains one `ExtractFull{reason: "cold_open"}` followed by many `CookStarted`/`CookFinished` pairs and `PrimHydrated{source: "extract"}`.
- **Re-open of the same scene:** zero `ExtractFull`, zero `CookStarted`; many `AssetResolved` + `CacheHit` + `PrimHydrated{source: "cache"}`.
- **Add one cube to an open scene:** one `ExtractIncremental{dirty_count: 1}`, one `AssetResolved{name: "primshape:cube"}` + `CacheHit`, one `PrimHydrated{source: "cache"}`. If any of these counts are larger, the incremental path leaks and the bug is visible in the dump.

**Not in Phase 2:** incremental BVH update in `SceneQuerySystem`. Flagged in §9.1's summary table — until that's incrementalized too, live-edit latency is bounded by `SceneQuery::rebuild`, not the extractor. Separate workstream; track as a follow-up once Phase 2 measurements show the extractor cost gone and the BVH cost exposed.

**Phase 3 — Separate CLI + runtime-only engine target.**

- `ngen-cook` binary, separate Makefile target.
- `ngen-runtime` Makefile target — engine without cooker library, without editor UI dir. Starts as a stub that opens a cooked scene and renders; no interactive controls needed to prove the link boundary.
- CI checks that `ngen-cook` doesn't pull in SDL/Vulkan *and* that `ngen-runtime` doesn't pull in cooker / shaderc / stb symbols.
- CI runtime-behavior check: run `ngen-runtime` against a pre-cooked scene with `--obs-output`, assert the dump contains zero `CookStarted` / `CookInvalidated` / `CookFinished` emissions (§10.4). If any appear, cooker code has leaked into the runtime binary.
- `make` updated to invoke `ngen-cook assets/` before building either engine variant.

**Phase 4 — Textures + hot reload.**

- `TextureCooker` with BC7 compression, mipmap pre-gen.
- File-watcher → re-cook → engine swap. Hot reload is tempting to do earlier but depends on `AssetSystem` being solid first. Editor-only — runtime builds don't hot reload.

---

## 14. Key decisions worth pushback

1. **Hand-rolled POD format vs flatbuffers.** I lean hand-rolled for Phase 1 to match house style and avoid a codegen step. The cost is manual discipline around layout. Reasonable to disagree — flatbuffers would save some work on version migration later.
2. **USD cache stores derived data, not a replacement format.** Keeps USD as edit target, only caches what the extractor produces. Per-file cache entries are prim-indexable so the extractor can hydrate one prim cheaply (the mechanism behind fast live-edit). The alternative (cook USD into an opaque engine-native scene format) would be faster still but loses live editability.
3. **Runtime/cooker library split enforced by linker, not `#ifdef`.** One dispatcher install-site is the entire difference between editor and runtime at call-time. `#ifdef` would be simpler short-term, harder to audit long-term.
4. **Use JobSystem for cook tasks.** Simple, but cook tasks can block non-trivially (USD open, shader compile). If that starves other engine work, a dedicated small pool is the answer. I'd wait for actual starvation before doing that.
5. **`.cache/` in repo root vs platform cache dir.** Repo root is easier during dev (inspectable, easily nuked). Platform cache dir (XDG / `~/Library/Caches`) is more conventional. Env var override split the difference.
6. **No asset bundling / archives.** One source → one cache entry, one mmap'd file on load. Adds `n` open()s per scene but mmap is cheap. Revisit if we hit fd / dentry pressure at scale.
7. **Runtime-only engine binary arrives in Phase 3, not Phase 1.** The *library structure* makes it possible from day one, but the binary itself (with the broader editor/runtime split it implies for `src/ui/`, editor passes, etc.) slots in once there's a reason to ship. Phase 1 just guarantees nothing precludes it.

---

## 15. Open questions

- **Cache concurrency.** Two engine processes cooking the same asset into `.cache/` simultaneously — file lock, write-to-temp-then-rename, or "last writer wins"? Probably rename-atomic is enough.
- **Cooker version coupling.** A bump to `ShaderCooker::cookerVersion` invalidates all shader caches. Fine. Does the *cache format* (`CookedHeader`) need its own version? Probably — a `formatMagic` change would mean "every blob in `.cache/` is unreadable." Likely yes, separate from per-cooker versions.
- **Shaderc vs glslang.** Both work, both vendorable. Shaderc has a friendlier C++ API and Google maintains it. Glslang is the Khronos reference. Lean shaderc; note if anyone has a preference.
- **Runtime + USDScene coupling.** §12.5 — slim USDScene in runtime, or cooked-only runtime? Needs a call before Phase 2 locks in the per-file cache format (and, for cooked-only, the shape of the cooked scene manifest that replaces USD composition at runtime).
