# Minimal `ngen-build` example

A tiny project graph that compiles `src/main.cpp` into a `hello` executable. The goal is to show the smallest
useful shape of a `build.cpp` and how the staged binaries fit together. Everything else (libraries, aliases,
tools, the cxx ObjectFile per-TU surface) is documented in [`../build_system.md`](../build_system.md) — this
file just covers the bare bones.

## Layout

```
build/example/
  README.md       # this file
  build.cpp       # the project graph
  src/
    main.cpp      # the program
```

`build.cpp` describes one platform, two configurations, and one program. `src/main.cpp` is the program.

## Running it

The example is small enough that the full `ngen-build` orchestrator isn't needed — we can run the graph and
runner stages directly. From the project root:

```sh
# 1. Build the example's graph binary from build/example/build.cpp.
mkdir -p build/example/_out
c++ -std=c++23 -O0 -g -o build/example/_out/example-graph build/example/build.cpp

# 2. Emit the IRs for this project. Writes build/example/_out/<platform>/<config>/build.ngenir.
(cd build/example && ./_out/example-graph)

# 3. Run the build via the parent project's already-built `ngen-build-run`.
#    (If you haven't bootstrapped that yet, do so from the project root first — see ../../CLAUDE.md.)
(cd build/example && ../../_out/ngen-build-run --ir _out/host/debug/build.ngenir)
```

After step 3, `build/example/_out/host/debug/hello` is the compiled executable:

```sh
./build/example/_out/host/debug/hello
# → hello from ngen-build example
```

The `example-graph` binary also accepts `--list` and `--dump-graph` (since `ir::main` provides those):

```sh
(cd build/example && ./_out/example-graph --list)
# → Platforms: host  Configurations: debug, release  Targets: hello (default)
```

## What `build.cpp` shows

Read [`build.cpp`](build.cpp) top to bottom. The five things going on:

1. **`cxx::toolchain()`** — the compiler/archiver/std defaults.
2. **`cxx::platform("host")`** — one platform identity, with the toolchain attached.
3. **`cxx::configuration("debug" | "release")`** — two build configurations.
4. **`Project p; p.platform(...); p.config(...);`** — registering them with the project.
5. **`cxx::program("hello").sources({"src/main.cpp"})`** — one program. `sources` materialises a
   `cxx::ObjectFile` per source file behind the scenes.
6. **`return ir::main(argc, argv, p);`** — hand argv off to the framework's CLI dispatcher.

To turn this into a real project: copy `build.cpp` + `src/` somewhere, copy `build/` next to them, then follow
[`../../CLAUDE.md`](../../CLAUDE.md)'s bootstrap command to compile `build/bootstrap.cpp` into your own
`ngen-build`. From there `./_out/ngen-build -p host -c debug` does the same thing as the manual three-step
invocation above.
