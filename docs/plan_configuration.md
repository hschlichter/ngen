# Configuration: the engine's settings declared in code, in one system

**Status. In progress.** Built 2026-11-01, decisions as leaned. The first step of [plan_lean_pass.md](plan_lean_pass.md): settings move out of the
UI into this system, so the UI can be cut without taking the engine's settings with it.

## Current state

The engine's settings have no home of their own. Most live in the UI: `ViewUI` holds the shadow cascades (`ShadowCascadeSettings`), the
sampler (`SamplerSettings`), the ambient (`AmbientSettings`), anti-aliasing, the depth prepass, the buffer and debug views, culling on and
frozen, guides and proxies shown; `CameraWindowState` holds the debug camera's switches and the camera modes; `LodWindowState` the LOD
Level view's colour mode and the pop flash. The frame loop reads them through `ViewUI` (81 references in `src/apps/view.cpp`, 32 in
`src/view/viewcommands.cpp`). The renderer's upload budget and geometry pool limit are set from the Scene Flow window (and the
`gpuscene` verb), which [plan_lean_pass.md](plan_lean_pass.md) removes. Others are constants where they are used (the debug camera's
offset, the follow height's default). Each is reached at runtime by a method of its own (`view.shadow`, `view.sampler`, `view.ambient`, `view.prepass`,
`view.antialiasing`, `view.mode`, `view.debugview`, `view.overlay`, `view.cull`, `view.lod.visualize`, `view.camera.debug`, …), each with its
parser, handler and record fields in `viewcommands.cpp`.

## Scope

**In**
- `src/config/`, a static library beside `profile` and `trace`: standard library only, no engine code.
- Typed values declared in code by the system they configure, each with a name, a default and a description.
- One way to read and set them at runtime: a record, RPC methods and a verb; a Configuration window.
- Moving the settings above into it ([plan_lean_pass.md](plan_lean_pass.md) step 1 does the moving; this plan builds the system and moves
  the render settings as its proof).

**Out**
- An init file, and keeping values across runs. The names are chosen so a file of `name = value` lines maps onto them one to one later.
- UI state: which windows are open, filters, selections, scroll. That stays in the UI.
- Requests and one-off actions (screenshots, dumps, LOD levels set by hand, the camera's pose): they are commands, not configuration.

## Decisions

### 1. How a configuration value is declared

- **A. A typed variable where it belongs**, registered by name when the program starts:
  ```cpp
  // src/renderer/shadowcascades.cpp
  config::Int shadowCascades{"render.shadow.cascades", 3, "Shadow cascades from the camera, 1 to 4.", {.min = 1, .max = 4}};
  ```
  Each system declares its own; the registry lists them all.
- **B. The settings structs as they are** (`ShadowCascadeSettings`, …), each field described to the registry by name, type and offset.

I lean **A**: a value is declared once, where it's used, with nothing to keep in step; a struct still exists where a group travels
together (the snapshot copies values into `ShadowCascadeSettings` as it does now). B keeps the structs but adds a description beside every
field that can drift from it.

### 2. Threads

A value is set and read on the main thread only. What the render thread needs travels in the `RenderSnapshot`, what the scene thread
needs in its requests, as today. No locks, no atomics. The library checks it in debug builds (the thread that first reads a value is the
only one that may).

### 3. Acting on a change

Some values need an action when they change (purposes shown ask the scene for a pass; the LOD tuning is a scene request). Each value keeps
a change count; a consumer keeps the count it last acted on and compares. No callbacks: the frame loop stays in order, and nothing runs
inside a `set`.

### 4. Types

`Bool`, `Int` and `Float` with an optional range, `Enum` (a value from named choices, set by name), `Vec3` (a colour or a vector, as three
floats). No strings until a value needs one.

### 5. Names

Dotted, by system then subject: `render.shadow.cascades`, `render.sampler.anisotropy`, `render.view.debug`, `view.culling.frozen`,
`tools.debugcamera.lod`. A prefix selects a group (the record, `config.reset render.shadow`). Names are the file format's keys later.

### 6. The bespoke setters

- **A. Replace them**: `view.shadow`, `view.sampler`, `view.ambient`, `view.prepass`, `view.antialiasing`, `view.mode`, `view.debugview`,
  `view.overlay` and the configuration parts of `view.cull`, `view.lod.visualize` and `view.camera.debug` go; `config.set` does what they
  did. Their verbs stay as short aliases that set the matching names (`prepass on` is `config render.prepass true`), so scripts keep working.
- **B. Keep them** beside `config.set`.

I lean **A**: one path to set a value, and most of the handlers in `viewcommands.cpp` that only parse and store go with it.

## Steps

1. **The library** (`src/config/config.{h,cpp}`, `build.cpp` beside `profile`):

   ```cpp
   namespace config {

   struct Range {
       std::optional<double> min;
       std::optional<double> max;
   };

   // A value's description for the registry, whatever its type: what the record, RPC and the window read.
   class Value {
   public:
       auto name() const -> const std::string&;
       auto description() const -> const std::string&;
       auto changes() const -> uint64_t; // counts every set that changed it
       virtual auto text() const -> std::string = 0;               // the value as the file and the verb write it
       virtual auto defaultText() const -> std::string = 0;
       virtual auto setText(const std::string& text) -> std::expected<void, std::string> = 0; // parsed, range-checked
       virtual auto reset() -> void = 0;
       // …
   };

   class Bool : public Value { /* get(), set(bool) */ };
   class Int : public Value { /* get(), set(int64_t), the range */ };
   class Float : public Value { /* get(), set(double), the range */ };
   class Enum : public Value { /* get() as index and name, set by name, the choices */ };
   class Vec3 : public Value { /* get() as std::array<float, 3> */ };

   // Every value declared, by name; filled as the program starts (a function-local registry, so declaration order doesn't matter).
   auto all() -> std::span<Value* const>;
   auto find(std::string_view name) -> Value*;

   } // namespace config
   ```
2. **Runtime access** (`src/view/viewcommands.cpp`, until [plan_lean_pass.md](plan_lean_pass.md) splits it): the `config` record (every value:
   name, type, value, default, description, range or choices, whether it differs from its default), `config.get {name or prefix}`,
   `config.set {name, value}`, `config.reset {name or prefix}`; verbs `config <name> <value>`, `config-reset [<prefix>]`.
3. **The render settings moved** (the proof): shadow cascades, sampler, ambient, anti-aliasing, prepass, buffer view, debug view, overlays,
   the upload budget and the geometry pool limit (the Scene Flow window's two controls, gone with that window).
   Declared in the renderer's files that own them, read on the main thread to fill the snapshot; the `ViewUI` members, their getters and
   the bespoke methods for them go (decision 6 A).
4. **The Configuration window** (`src/ui/configwindow.*`): every value by group, an editor per type, reset per value and per group, a
   marker on values changed from their default. Windows that show a setting today (the View menu's toggles, the shadow and sampler
   controls) edit the same values.
5. **READMEs**: `src/config/README.md` (declaring, threads, changes, names, types), the root README's list of libraries, the `run-headless`
   skill (the verbs).

## Visibility

- **Records**: `config` (new). The `status` record's settings fields that duplicate it (shadow, sampler, anti-aliasing) go.
- **RPC and verbs**: `config.get`, `config.set`, `config.reset`, `config`, `config-reset` (new); the bespoke setters for moved values go,
  their verbs kept as aliases.
- **Windows**: Configuration (new); the View menu and the existing controls edit configuration values.

## Verification

- **Nothing changes**: with every value at its default, the 8 reference images (three_cubes and Sponza: lit, albedo, normals, prepass)
  byte-identical, with validation on; the baseline script's verbs (`prepass on`, `view albedo`) still work through their aliases.
- **The record lists everything moved**: the `config` record has every value step 3 moved, each with its default and description; setting
  one with `config.set` shows in the record and in the next frame (`config render.shadow.cascades 1`: the cascades view shows one).
- **Ranges and choices hold**: `config render.shadow.cascades 9` is refused with the range in the error, the value unchanged; an enum set to
  a name it doesn't have is refused with the choices.
- **The UI no longer holds them**: `rg` finds no `ShadowCascadeSettings`, `SamplerSettings` or `AmbientSettings` member in `src/ui`.
- No warnings or errors in the trace.

## Results

Built 2026-11-01, release and debug builds. Where the build differs from the steps:
- **Declared in the renderer, named in one header**: each value is defined in the file of the part it configures (`lightingpass.cpp`,
  `debugviewpass.cpp`, `renderer.cpp`, `shadowcascades.cpp`, `gpuscene.cpp`); `src/renderer/renderconfig.h` declares them all with the
  helpers that read and write the snapshot's setting structs (`shadowSettings()`, `setShadowSettings()`, …). The buffer views' names moved
  next to their enum (`gbufferViewNames`, `lightingpass.h`).
- **The View menu and the Render Debug window keep their controls**: `ViewUI::draw` copies the values into locals they edit and writes them
  back after (unchanged values don't count as changes); the shortcuts and `view.overlay`'s buffer, shadow and anti-aliasing set values.
- **`config.set` takes several values at once** (`{values: {name: value}}`, checked together, nothing set when one fails) and a `reset`
  prefix: the older verbs (`view`, `debugview`, `shadow`, `sampler`, `ambient`, `prepass`, `aa`, `gpuscene`) are parsers onto it. RPC gained
  `RpcType::Any` for a value that may be a bool, number, name or array.
- **`--geometry-limit=<MiB>`** sets `render.geometry.limitMiB`.
- **19 values**: `render.view.buffer`, `render.view.debug`, `render.overlay.buffer`, `render.overlay.shadow`, `render.antialiasing`,
  `render.prepass`, `render.sampler.{anisotropy, lodBias, minLod, nearestMip}`, `render.shadow.{cascades, tile, splitLambda, pcf, distance}`,
  `render.ambient.{sky, ground}`, `render.upload.budgetMiB`, `render.geometry.limitMiB`.

Checks:
- **Nothing changes**: the 8 reference images byte-identical, run through the old verbs (`--view=albedo`, `prepass on`) now set as
  configuration; the prepass run has its `DepthPrepass` pass.
- **The record and the verbs**: the `config` record lists the 19 with defaults and descriptions; `config render.shadow.cascades 1`, the
  `shadow cascades=2,distance=150` and `ambient sky=0.5,0.5,0.5` short forms show in it; `config-reset render.ambient` puts the sky back;
  `config render.shadow.cascades 9` is refused ("9 is outside 1 to 4"), `config render.view.buffer nope` with the choices listed.
- **Applied on change**: `gpuscene budget=32` at the village takes the `gpuscene` record's upload budget from 64 MiB to 32 MiB.
- **The main-thread rule**: the debug build runs with the Configuration and Render Debug windows open and values set by verbs, no
  assertion. (It caught an ImGui misuse in the window's number inputs, fixed.)
- **The UI no longer holds them**: no `ShadowCascadeSettings`, `SamplerSettings` or `AmbientSettings` member in `src/ui`; `ViewUI`'s
  getters and setters for them gone; the `status` record's shadow, sampler, anti-aliasing and ambient fields gone (the `config` record has
  them). Validation on, no errors in the trace.

## Deferred / follow-ups

- **An init file** read at start (`name = value`), and a way to save the changed values. Trigger: settings wanted across runs, or a
  project's defaults differing from the engine's.
- **Per-project defaults**. Trigger: a second project.
- **Strings and lists as values**. Trigger: a setting that needs one.
