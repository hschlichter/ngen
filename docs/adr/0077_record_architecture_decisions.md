# 77. Record architecture decisions

Date: 2026-11-04

## Status

Accepted

## Context

ngen's design history lives in about 120 documents under `docs/`: plans, retrospectives, concept notes and generic references. A plan records the
decisions it made, but alongside its steps, results and deviations, and many plans were revised, superseded or set aside after they were written.
The current truth lives in the library READMEs (`src/*/README.md`), which describe how the code works now but not why it was chosen over the
alternatives, or what was tried and replaced.

To find out why something is the way it is, a reader — human or agent — has to know which plans to read, in which order, and which of their
decisions still hold. Several plans disagree with the code they describe, because the code moved on after the plan landed.

## Decision

Keep architecture decision records in `docs/adr/`, in Michael Nygard's format: a title, a date, Status, Context, Decision and Consequences, one
decision per file, numbered in the order the decisions were made (`NNNN_snake_case_title.md`). Each record ends with a short Verification section:
the plans it came from, where it is in the code, and where the code differs from the plan.

A superseded decision keeps its record, with its status changed to "Superseded by N"; the record that replaces it says "Supersedes N". Decisions
that were planned but never built are mentioned as rejected alternatives in the record of what was built instead, not given records of their own.

The first 76 records were extracted from `docs/` and checked against the code at `d83048c`. From here on, a plan that makes an architecture
decision adds its record when it lands. `docs/README.md` links the log, and `AGENTS.md` states the rule.

## Consequences

- The reasons behind the engine's shape can be read in one place, in order, without reading the plans.
- Reversed decisions stay visible with what replaced them, the same way superseded plans are kept.
- Plans keep their role as the step-by-step record; a record links its plans rather than repeating them.
- The Verification sections are a snapshot. They go stale as the code moves, like the plans did; a later change that alters a decision writes a new
  record rather than editing the old one's Decision.
- Landing a plan has one more step: write the record.

## Verification

- Sources: [docs/README.md](../README.md), the plans linked from each record.
- Code: none — this decision is about the documentation.
