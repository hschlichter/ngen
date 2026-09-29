# ngen-cli

**Status. Draft.**

## Current state

Every tool has its own entry point and its own flags:

- `./_out/ngen-build -p <platform> -c <config> [target]` builds. Platform and config are required on every call. The build system has no project
  defaults by design (`build/bootstrap.cpp`, header comment).
- `./_out/<platform>/<config>/ngen-view <scene> [flags]` runs the viewer. Its path depends on the variant, so every command spells out the variant
  twice: once to build, once to run.

`ngen-build` is bootstrapped by hand once (`c++ … -o _out/ngen-build build/bootstrap.cpp`). It then rebuilds its own helper binaries
(`ngen-build-graph`, `ngen-build-run`) on demand.

## Scope

**In**

- `ngen-cli`, a C++ program in `src/cli/cli.cpp`, and a program target in `build.cpp`. It is built per variant like `ngen-view`, into
  `_out/<platform>/<config>/ngen-cli`.
- `ngen-cli set <platform> <config>`:
  - writes the variant to `_out/set`, as one line: `linux-vulkan/release`
  - points the repository-root symlink `ngen-cli` at that variant's cli
  - `ngen-cli set` with no arguments prints the set variant
- `ngen-cli view [args]` runs the set variant's `ngen-view` with the arguments unchanged. Every platform- and config-dependent tool is looked up
  the same way: `_out/<contents of _out/set>/<tool>`.
- `ngen-cli build [args]` runs `ngen-build`.
  - Platform and config default to the set variant.
  - A `-p`/`--platform` or `-c`/`--config` in the arguments wins over the default, so `ngen-cli build -p linux-vulkan -c debug <target>` is plain
    `ngen-build` with full arguments.
  - Everything else passes through unchanged: targets, `-v`, `--clean`, `format`, and so on.
- `ngen-cli help`, and `ngen-cli` with no command, print the commands and the set variant.
- The forwarded tool's exit code is `ngen-cli`'s exit code, so headless scripts keep working (`--fail-on-validation` exits 2).

**Out**

- Other tools. Tool commands are a table in `cli.cpp`: adding one is a row, command name to binary name.
- Building on demand: `view` does not build first. `ngen-cli build && ngen-cli view …` does.
- Cleaning in the cli. `ngen-cli build --clean` is `ngen-build --clean`: it removes the variant's outputs, the cli included. If that variant is the set
  one, the root link dangles until the cli is rebuilt (`ngen-build -p … -c … ngen-cli`). To drop the set variant, delete `_out/set` and
  the link by hand.
- Shell completion, Windows.

## Decisions

1. **The cli is a project build target.** `cxx::program("ngen-cli")` in `build.cpp`: one source file, the standard library only, no links. The
   bootstrap stays what it is today: `ngen-build` only.
   - **First use on a fresh clone:** bootstrap `ngen-build` as today, then
     `./_out/ngen-build -p linux-vulkan -c debug ngen-cli && ./_out/linux-vulkan/debug/ngen-cli set linux-vulkan debug`. `set` creates the root link,
     and from then on `./ngen-cli` works.
   - **`set` builds the cli in the variant it switches to**, then writes `_out/set` and moves the link: `ngen-build -p <platform> -c <config>
     ngen-cli`. The link never points at a variant without a cli. The cost is a small one-file compile per variant, done once.
   - **The default target stays `ngen-view`.** A plain `ngen-cli build` does not rebuild the cli; after a change to `src/cli/cli.cpp`, run
     `ngen-cli build ngen-cli`. `build` hands over to `ngen-build` with `execv`, so the running cli is gone before its binary is relinked.
   - The root link is not committed. `set` creates it, pointing at `_out/<platform>/<config>/ngen-cli`, and it is listed in `.gitignore`.

2. **`_out/set` holds the variant as a path.** One line, `<platform>/<config>`, relative to `_out/`: the same two components as the variant's output
   directory. Every tool lookup is `_out/` plus that line plus the tool name. `build` splits the line at the `/` for `-p` and `-c`.
   - It sits in `_out/`, so it is per clone and never committed.
   - A plain file that scripts can read too: `$(cat _out/set)`.

3. **`build` fills in only what is missing.** The cli scans the arguments for `-p`/`--platform` and `-c`/`--config`.
   - It prepends `-p <set platform>` only if no platform is given, and the same for the config. So one explicit flag overrides one default:
     `ngen-cli build -c release` builds the set platform in release.
   - With both given, `_out/set` is not read at all, so `build` works before any `set`.
   - With neither given and no `_out/set`, it fails with "run `ngen-cli set <platform> <config>` or pass `-p`/`-c`".
   - The explicit flags do not change `_out/set`; only `set` does.

4. **Names are validated, and a prefix is enough.** `set` checks the platform and config against `ngen-build -l`: it reads the "Platforms:" and
   "Configurations:" sections. A typo fails at `set`, not at the next build.
   - A unique prefix is accepted: `set linux release` resolves to `linux-vulkan`.
   - An ambiguous or unknown name fails with the valid names listed.
   - Relying on the `-l` text layout couples the two tools. If that proves brittle, `ngen-build` can grow a machine-readable listing; nothing needs
     that yet.
   - `build` does not validate: `ngen-build` already reports unknown names.

5. **Forwarding keeps the caller's context.** The cli finds the repository root from its own resolved executable path: `/proc/self/exe` resolves to
   `_out/<platform>/<config>/ngen-cli`, three levels below the root. It does not use the working directory, so `../ngen-cli` works from `src/`.
   - The forwarded tool runs in the caller's working directory with the caller's environment.
   - It is started with `execv`, so it replaces the cli process: signals, the terminal and the exit code are the tool's own.
   - `set` is the exception: it runs `ngen-build` as a child and waits, because it has more to do after the build.

## Steps

1. `src/cli/cli.cpp`, with a header comment on what it is, the first use, and `_out/set`.
   - `repoRoot()`: `std::filesystem::canonical("/proc/self/exe")`, three levels up.
   - `readSet() -> std::optional<Variant>`, where `Variant` has `platform` and `config` strings: reads and splits `_out/set`.
   - `listVariants() -> VariantNames`, the platform and config lists: runs `_out/ngen-build -l` through `popen` and parses the two sections.
   - `resolveName(std::string_view query, std::span<const std::string> names) -> std::expected<std::string, std::string>`: exact match first, then a
     unique prefix.
   - Commands:
     - `set`: resolve the names, run `ngen-build … ngen-cli` and wait, write `_out/set`, and replace the root link atomically (a temporary link, then
       `rename`).
     - `build`: add the missing `-p`/`-c` from `_out/set`, then `execv` `_out/ngen-build`.
     - Tool commands (`view`): `execv` `_out/<set>/<tool>` with the arguments. If it isn't built, say so and name the build command.
     - `help`.
   - Tool table: `{ "view", "ngen-view" }`.
2. `build.cpp`: the `ngen-cli` program, registered with the project. The default target is unchanged.
3. `.gitignore`: `/ngen-cli`.
4. Check that the `format` and `tidy` tools in `build.cpp` cover `src/cli/` like the rest of `src/`.
5. Docs:
   - `AGENTS.md` "Build": the first-use line after the `ngen-build` bootstrap; `ngen-cli set` / `ngen-cli build` / `ngen-cli view` as the everyday
     path, with the `ngen-build` flags still documented underneath.
   - The root `README.md` quick start.
   - The `run-headless` skill: `./ngen-cli view` as the short form.
6. Register this plan in `docs/README.md` under Infrastructure.

## Verification

- First use, with no `_out/set` and no root link:
  - `./_out/ngen-build -p linux-vulkan -c debug ngen-cli` builds `_out/linux-vulkan/debug/ngen-cli`
  - that binary with `set linux-vulkan release`:
    - builds `_out/linux-vulkan/release/ngen-cli`
    - writes `_out/set` containing `linux-vulkan/release`
    - creates `ngen-cli -> _out/linux-vulkan/release/ngen-cli`
- `./ngen-cli set` prints `linux-vulkan/release`. `set linux debug` resolves the prefix, rewrites the file and moves the link.
- `set windows debug` and `set linux nope` fail, list the valid names, and leave `_out/set` and the link unchanged.
- `build` defaults and overrides, with release set:
  - `./ngen-cli build ngen-view`, then `./_out/ngen-build -p linux-vulkan -c release ngen-view`: the second reports nothing to do.
  - `./ngen-cli build -c debug ngen-view` builds `_out/linux-vulkan/debug/ngen-view`, and `_out/set` still says release.
  - `./ngen-cli build -p linux-vulkan -c gamerelease` works with `_out/set` removed.
  - With `_out/set` removed and no flags, `./ngen-cli build` fails with the message from Decision 3.
- `./ngen-cli build` with no targets builds `ngen-view` only. After touching `src/cli/cli.cpp`, `./ngen-cli build ngen-cli` relinks the cli
  through the link it runs from, and the next `./ngen-cli` call runs the new binary.
- `./ngen-cli build examples` and `./ngen-cli build format` pass their arguments through unchanged.
- `SDL_VIDEODRIVER=offscreen ./ngen-cli view assets/three_cubes.usda --frames=30 --screenshot=/tmp/a.png` writes a PNG byte-identical to the one from
  `_out/linux-vulkan/release/ngen-view` with the same flags.
- The exit code passes through: a view run with `--fail-on-validation` that hits a validation error exits 2 through the cli too.
- `cd src && ../ngen-cli set` prints the variant. `cd src && ../ngen-cli view ../assets/three_cubes.usda …` resolves the scene relative to `src/`.

## Deferred / follow-ups

- **Machine-readable listing from `ngen-build`** (Decision 4). Trigger: the `-l` text layout changes, or a second consumer needs it.
- **A variant-independent cli**, installed to one fixed path so `set` doesn't compile a cli per variant. Trigger: the build framework gains host-tool
  outputs, or the per-variant compile becomes a nuisance.
