# 33. `ngen-cli` is the front door, the variant in `_out/set`

Date: 2026-09-29

## Status

Accepted

## Context

`ngen-build` requires a platform and a configuration on every call (ADR 18), and each variant's binaries live under a path that depends on the
variant (`_out/<platform>/<config>/`). Humans and agents spelled the variant out twice for every build-and-run, and the build system is not allowed
to grow project defaults to fix that.

## Decision

`ngen-cli` is an ordinary per-variant program — one file, standard library only — that sits above the build system:

- `set <platform> <config>` validates the names against `ngen-build -l` (a unique prefix is accepted), builds that variant's cli first, writes
  `<platform>/<config>` to `_out/set`, and repoints the root symlink `./ngen-cli` with one atomic rename.
- `build` fills in only the missing `-p`/`-c` and `execv`s `ngen-build`; explicit flags win.
- Tool commands — `view`, `introspect`, `asset-server`, `pack` — `execv` the set variant's binary in the caller's working directory, so its exit code
  and signals are the tool's own.
- The repository root comes from `/proc/self/exe`, not the working directory.
- It is the entry point for humans and agents alike, so the same flow is exercised on every run. Agents don't change the set variant unless asked,
  because `_out/set` is shared with the human's session.

Rejected: putting the cli in the bootstrap (the bootstrap stays `ngen-build` only), building on demand in `view`. Deferred: a machine-readable target
listing and a variant-independent cli.

## Consequences

- Easier: one short command per action, with exit codes passed straight through.
- Easier: agents and humans run the same commands, so the tool flow is tested by use.
- Harder: `set` parses the text layout of `ngen-build -l`.
- Harder: a cli is compiled per variant, and cleaning the set variant leaves the root link dangling.
- Harder: Linux-only, through `/proc/self/exe`.

## Verification

- Sources: [plan_ngen_cli.md](../plan_ngen_cli.md), `AGENTS.md` "Build".
- Code: `src/apps/cli.cpp:62`–`64` (`introspect`, `asset-server`, `pack`), `:69` (root from `/proc/self/exe`), `:81` and `:94` (`_out/set`), `:180`
  (`execv`), `:217`–`232` (link replaced in one rename); `build.cpp:506`.
- Differs from plan: the plan's tool table had only `view` and placed the source in `src/cli/cli.cpp`. Its Results first said agents call
  `ngen-build` directly; that was reversed the same day.
