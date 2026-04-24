# buildhpp — Concepts, Ideas, and Implementation Detail

This document describes the design of `buildhpp`: a minimal, header-only C++
build-system *library* in which build specifications are expressed as ordinary
C++ programs. The aim is to capture the concepts well enough that the system
can be reimplemented in another language, environment, or with a different
scope.

---

## 1. Core Idea

`buildhpp` is **not** a build tool that interprets a configuration file.
Instead, the "build configuration" *is a C++ program* that links against a
small header-only library (`build.hpp`). That program constructs an in-memory
**build graph** (rules, targets, commands, flags) and then hands the graph to a
**backend** that either serializes it to a file or actually executes it.

The canonical backend produces and runs a `.ninja` file, delegating scheduling,
incremental rebuilds, and parallelism to [Ninja](https://ninja-build.org/).
The abstraction is intentionally backend-agnostic: any system capable of
consuming a DAG of `(target, deps, command)` tuples can be plugged in.

Three conceptual layers:

1. **Core data model** (`build.hpp`) — pure data types: `Target`, `Command`,
   `Flags`, `Rule`, `Graph`. No I/O, no side effects.
2. **Backend traits** (`build.hpp`) — two customization points,
   `SerializerTrait<T>` and `ExecutorTrait<T>`, specialized per backend.
3. **Backend implementation** (`ninja.hpp`) — a trait specialization for a
   tag-type `Ninja` that serializes the graph to Ninja syntax and invokes
   `ninja`.

A user-written `build.cpp` composes all three by constructing a `Graph` and
calling `Builder<Ninja>::build(graph, targetName)`.

---

## 2. Why This Shape?

- **No new DSL.** The build description is C++: loops, functions, conditionals,
  type checking, refactoring, and IDE support come "for free".
- **No runtime dependency on a scripting interpreter.** Once compiled, the
  build program is a single binary.
- **Backend-pluggable.** The core graph knows nothing about Ninja. Swapping
  in a Make backend, a shell-script backend, or an in-process executor is a
  matter of specializing two trait templates.
- **Incrementality is delegated.** The library itself does no timestamp or
  content-hash tracking; that responsibility lives with whichever backend
  consumes the graph.
- **Bootstrappable.** The library uses itself to build itself (see §7).

---

## 3. Data Model

All types live in `namespace build`. Paths use `std::filesystem::path`, aliased
as `build::Path`.

### 3.1 `Flags`

A thin wrapper around `std::vector<std::string>` for compiler/linker flags
shared across the whole graph.

- `add(std::string)` — append a flag (move-in).
- `empty() const` — predicate.
- `toString() const` — space-join; **no quoting is performed**. The contract is
  that the caller is responsible for any shell-escaping a flag needs.

### 3.2 `Target`

Represents a node in the build DAG.

- Constructor: `Target(Path p, bool phony = false)`.
- Accessors: `path()`, `isPhony()`, `name()` (stringified path).
- Equality operators compare path + phony.
- The "phony" flag mirrors Ninja's `phony` concept: a name that has no
  on-disk artifact and exists purely to group dependencies.

### 3.3 `Command`

An argv-style command: `std::vector<std::string>`.

- Variadic constructor:
  `template<class... Args> explicit Command(Args&&... a) : args{ std::forward<Args>(a)... }`.
- `addArg(std::string)`.
- `toString()` — joins with single spaces, and **only** adds double-quotes
  around arguments that contain a space. No escaping of embedded quotes or
  backslashes. This is deliberately simple; a reimplementation targeting
  untrusted inputs should use a proper shell-quoting routine.

### 3.4 `Rule`

A plain aggregate:

```cpp
struct Rule {
    Target target;              // the output
    std::vector<Target> deps;   // inputs / prerequisites
    Command cmd;                // how to produce target from deps
};
```

There is no per-rule flag list in the current design; flags live on the
graph and are applied globally at serialization time.

### 3.5 `Graph`

The root container.

- `std::unordered_map<std::string, Rule> rules` — keyed by the target's
  stringified path. Implication: **at most one rule per target name**, and
  adding a second rule for the same target silently overwrites.
- `Flags globalFlags`.
- `addRule(Rule)` / `addFlag(std::string)` helpers.

The map is iterated (unordered) by serializers. Ninja tolerates this, but any
backend that cares about declaration order must sort explicitly.

---

## 4. Backend Customization Points

Two class templates act as customization points, parameterized by a **tag
type** identifying the backend.

```cpp
template <typename T>
struct SerializerTrait {
    static auto serialize(const Graph&, const Path&) -> bool { /* static_assert */ }
};

template <typename T>
struct ExecutorTrait {
    static auto execute(const Graph&, const Path&, const Target&) -> bool { /* static_assert */ }
};
```

- Default primary templates intentionally `static_assert(sizeof(T) == 0, …)`,
  forcing backends to provide specializations.
- `Builder<T>` stores a file path and forwards `build(graph, target)` to
  `ExecutorTrait<T>::execute`. The tag type `T` is never instantiated; it
  exists purely for overload resolution.

The split of serialize vs. execute is intentional: backends that only need to
emit a file (e.g., for CI cache artifacts) can implement just `SerializerTrait`
and call it themselves; backends that drive a process can implement
`ExecutorTrait` on top of the serializer.

### 4.1 `Builder<T>`

Tiny façade:

```cpp
template <typename T>
class Builder {
public:
    explicit Builder(Path fp) : filepath(std::move(fp)) {}
    bool build(const Graph& g, const Target& desired) const {
        return ExecutorTrait<T>::execute(g, filepath, desired);
    }
private:
    Path filepath;
};
```

`filepath` is the *output* build-description file (e.g. `"app.ninja"`), not a
source file.

---

## 5. Ninja Backend (`ninja.hpp`)

The file defines an empty tag type `struct Ninja;` and specializes both traits.

### 5.1 `SerializerTrait<Ninja>::serialize(g, file)`

Emits Ninja-syntax into `file`:

1. Open `std::ofstream`; on failure log to `stderr` and return `false`.
2. Compute a single `flags = …` variable from `g.globalFlags.toString()`,
   appending a trailing space if non-empty.
3. Emit preamble:

   ```
   ninja_required_version = 1.3

   flags = <flags>

   rule cmd
     command = $cmd $flags
     description = building $out
   ```

4. For each `(name, Rule)` in `g.rules`:

   ```
   build <target>: cmd <dep1> <dep2> …
     cmd = <command string>
   ```

   If the target is phony, the statement is written as
   `build <target>: phony cmd <deps…>` (this mirrors the current code; note
   that in native Ninja `phony` is itself a rule, so a re-implementation may
   want to emit `build <t>: phony <deps…>` without the `cmd` tail when phony
   — see §9 Known Limitations).

5. Return `true`.

### 5.2 `ExecutorTrait<Ninja>::execute(g, file, desired)`

1. Call `SerializerTrait<Ninja>::serialize(g, file)`; bail on failure.
2. Print `Generated <file>`.
3. Build a shell command: `"ninja -v -f " + file.string() + " " + desired.name()`.
4. `std::system(cmd)`; non-zero → log and return `false`.
5. Return `true`.

Notes:

- Uses `-v` (verbose) unconditionally; a reimplementation may want this
  configurable.
- `desired.name()` is not shell-quoted. Target names with spaces will break.
- Relies on `ninja` being on `PATH`.

---

## 6. Example: User Build Program (`build.cpp`)

```cpp
int main(int argc, char* argv[]) {
    if (argc != 2) { /* usage */ return 1; }
    build::Target desired{ argv[1] };

    build::Graph graph;
    graph.addFlag("-Wall");
    graph.addFlag("-std=c++20");
    graph.addFlag("-DHELLO");
    // …

    graph.addRule({
        build::Target{"simple.o"},
        { build::Target{"simple.cpp"} },
        build::Command{"clang++", "-c", "simple.cpp", "-o", "simple.o"}
    });
    graph.addRule({
        build::Target{"app"},
        { build::Target{"simple.o"} },
        build::Command{"clang++", "simple.o", "-o", "app"}
    });

    build::Builder<Ninja> builder{"app.ninja"};
    return builder.build(graph, desired) ? 0 : 1;
}
```

Invocation: `./build app` → writes `app.ninja`, runs `ninja -v -f app.ninja app`.

### 6.1 Flag Semantics in the Example

Flags added to `Graph` become the Ninja variable `$flags` and are appended to
**every** invocation of the single `rule cmd`, because the rule is defined as
`command = $cmd $flags`. So in the example, the link step also receives
`-Wall -std=c++20 -DHELLO …`. That is acceptable for Clang drivers but a
reimplementation may want per-rule or per-target flag sets.

---

## 7. Bootstrap Chain

The repository contains three layered programs, each built by the previous,
demonstrating self-hosting:

```
bootstrap.ninja ──ninja──▶ b (bootstrap binary)
                             │
                             ▼ (runs)
              [generates prebuild.ninja, runs ninja] ──▶ prebuild
                             │
                             ▼ (runs)
              [generates build.ninja, runs ninja]    ──▶ build
                             │
                             ▼ (runs with target arg)
              [generates app.ninja, runs ninja]      ──▶ app
```

### 7.1 `bootstrap.ninja` (the only hand-written Ninja file)

Defines two rules the library itself never emits:

```
rule cxx
    command = clang++ -Wall -std=c++20 -O2 -MMD -MF $out.d -c $in -o $out
    depfile = $out.d
    deps = gcc
    description = [CXX] $out

rule link
    command = clang++ $in -o $out
    description = [LINK] $out

build bootstrap.o: cxx bootstrap.cpp
build b: link bootstrap.o
default b
```

Two things are notable and **not** present in the generated files:

- `depfile`/`deps = gcc` — enables Ninja's GCC-style header-dependency
  scanning. The library's own serializer does not emit this, so generated
  `.ninja` files do not track header changes. A reimplementation should
  consider exposing `depfile`/`deps` on rules.
- `default b` — chooses a default target. The library-emitted files omit
  `default`, which is why `Builder::build` must pass the desired target on the
  `ninja` command line.

### 7.2 `bootstrap.cpp`

Orchestrates a three-step bootstrap:

1. `generate_prebuild()` — in-process: builds `prebuild.ninja` describing how
   to compile `prebuild.cpp` → `prebuild`, then invokes `ninja`.
2. `std::system("./prebuild")` — runs the resulting `prebuild` binary, which
   builds `build`.
3. `std::system("./build <target>")` — runs `build` with the user-supplied
   target, which builds `app.ninja` and invokes `ninja` to produce the final
   artifact.

Each hop uses the same `build::Builder<Ninja>` pattern, proving the library
can describe its own build.

### 7.3 `prebuild.cpp`

A minimal graph with one rule (`build` ← `build.cpp`) and one invocation of
`Builder<Ninja>::build`. Its only purpose is to emit and run `build.ninja`
to produce the `build` executable.

### 7.4 Why three layers?

Only one layer is strictly necessary (a single `build` binary). The extra
layers exist to demonstrate that the library is composable and that each
stage can itself be a buildhpp program. In a production reimplementation,
collapsing to a single `build` with the bootstrap as a one-off shell/ninja
stanza is reasonable.

---

## 8. Invariants and Assumptions

- **Paths** are filesystem paths (`std::filesystem::path`). Cross-platform
  path semantics are whatever `std::filesystem` gives you.
- **Target identity** is `(path, phony)`; the `Graph` keys on `path.string()`
  alone, so two rules producing the same path with different phony flags
  collide.
- **No cycle detection.** Cycles are whatever the backend detects. Ninja will
  error.
- **No parallelism control in the library.** Parallelism is the backend's
  concern.
- **No quoting guarantees.** The library does the minimum: `Command::toString`
  wraps args containing spaces in double quotes; embedded quotes, backslashes,
  or shell metacharacters pass through unchanged.
- **Exit on first failure.** Every layer returns `bool` / non-zero exit code
  on the first error and prints to `stderr`.
- **Single implicit Ninja rule.** The serializer emits exactly one Ninja rule
  (`cmd`) whose command is parameterized by `$cmd` and `$flags`. Every build
  statement provides its own `cmd` as a variable override. This keeps the
  serializer small but foregoes per-rule Ninja features.

---

## 9. Known Limitations (to address or consciously keep in a reimplementation)

1. **No header dependency tracking.** Emit `depfile = $out.d` and
   `deps = gcc`/`msvc` on a C/C++ rule.
2. **Global flags only.** Add per-rule or per-target flag lists.
3. **Single Ninja `rule`.** Generating distinct rules per "kind" (cxx, link,
   ar, mkdir…) yields nicer logs and lets Ninja deduplicate common text.
4. **Phony emission.** `build <t>: phony cmd <deps>` emits a phony *and* a
   command; either pick phony **or** cmd. Conventional Ninja is
   `build <t>: phony <deps>` with no command.
5. **Unordered iteration.** `std::unordered_map` makes the emitted file's
   order non-deterministic; for reproducibility use an ordered container or
   sort before emitting.
6. **Silent rule collisions.** `addRule` overwrites; detect and error, or
   return a status.
7. **No shell escaping.** `Command::toString` cannot safely handle args with
   quotes or `$`. Use a proper shell-quoting function, or (better) bypass the
   shell by writing a response file and using `ninja`'s `rspfile` feature.
8. **`std::system` everywhere.** Prefer `posix_spawn` / `CreateProcess` for
   argv-level control and to avoid shell interpretation.
9. **No default target in generated files.** Either always emit
   `default <target>` when a desired target is known at serialization time,
   or keep the CLI-argument approach but document it.
10. **No clean/install/phony-group conventions.** Real build systems want
    `clean`, `all`, and the ability to express them as phony groups.

---

## 10. File-by-File Summary

| File | Role |
|------|------|
| `build.hpp` | Header-only core: `Flags`, `Target`, `Command`, `Rule`, `Graph`, `SerializerTrait<T>`, `ExecutorTrait<T>`, `Builder<T>`. |
| `ninja.hpp` | Backend: declares tag `Ninja`, specializes both traits to emit + run `.ninja` files. |
| `bootstrap.ninja` | Hand-written Ninja file to build the `b` bootstrap binary. Only file not generated. |
| `bootstrap.cpp` | Bootstrap driver; generates `prebuild.ninja`, runs `prebuild`, runs `build <target>`. |
| `prebuild.cpp` | Builds the `build` binary from `build.cpp` via buildhpp. |
| `build.cpp` | User-level example: builds `simple.cpp` → `simple.o` → `app`. |
| `simple.cpp` | Trivial "Hello simple" program used as the leaf artifact. |
| `.gitignore` | Ignores object files, generated ninja files, and the three bootstrap binaries (`b`, `prebuild`, `build`, `app`). |
| `LICENSE` | License. |

---

## 11. Reimplementation Checklist

To port or rewrite this system, the following are the essential pieces in
rough order of dependency:

1. **Value types** for path, flag-list, command (argv vector), target (path +
   phony bit), rule (target + deps + command).
2. **Graph container**: rules keyed by target name, global flags. Decide up
   front whether to keep unordered or ordered semantics (recommend ordered
   for reproducibility).
3. **Backend interface**: two operations — `serialize(graph, outfile)` and
   `execute(graph, outfile, desiredTarget)`. In C++ this is trait
   specialization; in other languages, an interface/trait/protocol with two
   methods works equally well.
4. **Ninja backend**:
   - Emit `ninja_required_version`, `flags = …`, one generic `rule cmd`.
   - Per rule: `build <t>[: phony] cmd <deps…>` + indented `cmd = …`.
   - Execute by shelling out to `ninja -v -f <file> <target>`.
5. **A user build program** that instantiates a graph, adds rules/flags, and
   calls the backend.
6. **(Optional) Bootstrap**: a hand-written minimal build script (Ninja,
   Make, or shell) that compiles the user program; then the user program
   takes over.

Improvements worth making during a rewrite (see §9): header-dep tracking,
per-rule flags, multiple Ninja rules per "kind", shell-safe quoting,
deterministic iteration order, rule-collision detection, explicit `default`
target emission.

---

## 12. Minimal Mental Model

> A `Graph` is `{ rules: map<TargetName, (Target, [Dep], Command)>, flags: [Flag] }`.
> A `Backend` is two functions: `write(graph, path)` and
> `run(graph, path, desiredTarget)`.
> A **build program** builds a `Graph` in memory and hands it to a `Backend`.
> The backend does everything else (scheduling, incrementality, parallelism).

That is the entire system.
