---
name: new-plan
description: Scaffold a new plan document in docs/ following the project plan format and register it in the docs index. Also use when landing or superseding an existing plan (flips the status header and updates the index annotation).
---

# Create, land, or supersede a plan document

The plan format itself is specified in `CLAUDE.md` ("Plan documents") — follow it exactly. This skill covers
the file mechanics around it.

## Creating a plan

1. One plan per feature. If the request covers several separable features, write one
   `docs/plan_<topic>.md` per feature (snake_case) and cross-reference shared work between them.
2. Draft the content through back-and-forth, not as a finished handoff: present decision forks (options,
   trade-offs, recommendation) to the user before locking them into the doc. Genuinely unresolved items go
   in an "Open questions" section.
3. Sections in order, per CLAUDE.md: status header (new plans start as `**Status. Draft.**`), current
   state, scope with explicit In/Out, concrete steps naming real files and signatures, verification with
   observable binary criteria (obs-bus events — see the `run-headless` skill — byte-identical outputs, what
   a test scene should show), then "Deferred / follow-ups" with trigger conditions.
4. Register it in `docs/README.md`: add a link line with a one-line description under the matching section.

## Landing a plan

1. Flip the status header to `**Status. Landed.**`.
2. Update its annotation in `docs/README.md` to `(landed)`.
3. For large multi-phase efforts, a retrospective summary doc (the `frame_graph_phaseN_summary.md`
   pattern: past tense, "what changed", mirrors the plan's structure) may be worth writing — ask the user
   before adding one.

## Superseding a plan

1. Keep the old doc; set its header to `**Status. Superseded by <new doc>.**` — it is a record of the path
   not taken.
2. Update both docs' annotations in `docs/README.md`.
