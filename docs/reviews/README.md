# Reviews

Review artifacts: one HTML page per change, written for a reviewer to judge it without reading the diff first. Each is published as a private
claude.ai artifact; the file here is its source, kept with the plan it reviews, and republished from here when it changes.

## Reviews

- [lod_blending_review.html](lod_blending_review.html) — [plan_lod_blending.md](../plan_lod_blending.md): both UsdLod heuristics, distance first,
  blend bands cross-faded with ordered dither (2026-10-25, for a Technical Director)

## Making a review

**When.** On request, usually once a plan has landed and before its change is committed, so the reviewer sees what will go in.

**Where.** `docs/reviews/<topic>_review.html`, snake_case, the topic the plan's (`plan_lod_blending.md` → `lod_blending_review.html`). Add a line
to the list above with the plan, one line on what changed, the date and the audience. Publish the file as an artifact and give its link; the
link is private until its owner shares it.

**Audience.** Say who it is for and write for them. The default is a Technical Director: someone who decides on architecture, cost and risk,
knows rendering and engines, reads code, and has not followed the work.

**Focus.** The review is about the actual changes: the flow through the engine and the code that does each step. The summary, decisions,
evidence and risks frame it; the walkthrough is most of the page.

**Sections, in this order:**

1. **Header.** The change in one or two sentences, its headline result. Meta: the plan (and its status), the state of the change (staged,
   or the commit), its size from `git diff --stat` (files, insertions, deletions), the date.
2. **Summary.** Three or four headline numbers, each with what it measures; then a short paragraph: is it correct, is it contained, and
   where the open questions are.
3. **The flow.** A diagram of the change's path through the engine: by thread or lane (scene thread, render thread, GPU frame), in the
   order data moves, each box naming the functions and data it holds, numbered to match the walkthrough. New behaviour marked apart from
   what only changed shape.
4. **Walkthrough.** One block per stage of the flow, in order, each with:
   - the files it touches;
   - what it does now and what it did before, in a few sentences;
   - what goes in and what comes out (data, structs, buffers);
   - the code that does it: excerpts from the change itself, real identifiers, abridged where long and marked as abridged ("…");
   - before/after layouts where a struct or buffer changed shape (sizes, fields);
   - "Check here": what a reviewer should look at in that stage: invariants, edge cases, costs, deviations from the plan.
5. **Evidence.** Every check, as a table: what was checked, the result (pass, fail, deferred), how. A chart where the result is a curve or a
   series (reference against measured). Deferred checks are listed, with why.
6. **Decisions.** A table: each decision, what was chosen, what was set aside, and the walkthrough stage where it shows. Mark each locked
   (by the user, with the date in the plan) or taken as leaned; name any deviation from the plan.
7. **Risks.** Each as cost, look, behaviour, maintenance or scope, pointing to its stage: what it is, its size where known, and the
   reviewer's choice it leaves.
8. **Runtime access.** The records, RPC methods, script verbs, trace events and windows added or changed, and any gap, as the project's
   rule "Runtime access and visibility" asks (`../../AGENTS.md`).
9. **Decisions for the reviewer.** The open questions as asks, one line each.

**Rules for the content:**

- Numbers, units and timings are written to be understood at first read, without working them out:
  - say what the number means in words next to it ("the same LOD level as OpenUSD at 71 of 71 cameras; the largest gap a millionth of a
    level", not "≤ 1.1e-6");
  - plain decimals or words over scientific notation, and everyday units (ms a frame, MB, metres);
  - give a timing its context (what it is a part of: a 60 fps frame has 16.7 ms) and a change its before and after;
  - count things people recognise (pixels, draw calls, camera positions) rather than internal units (texels, records, slots) unless the
    review is about those.
- Every number comes from a run of this change: say which scene, which tool, what resolution. No estimate is presented as a measurement;
  an estimate says so.
- Name what wasn't checked and why, as plainly as what was.
- Plain language: short sentences, terms of the trade, no marketing. Name things as the code does where the reviewer will search for them
  (`pickLodLevel`, `lodselect.comp`).
- Paths and identifiers in code style; file lists per walkthrough stage.
- Code excerpts are copied from the change, not paraphrased; a compressed or abridged excerpt says so. Comments in excerpts may be the
  code's own or short annotations, set apart as comments.

**The page.** Self-contained HTML: inline CSS and script, fonts from Google Fonts only, no other external loads. Light and dark themes from
colour tokens. It must read at phone width (tables and diagrams scroll inside their own box). Charts drawn to scale from the measured data,
which the page carries inline.
