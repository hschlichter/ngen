# docs index

Plans, design docs, and retrospectives for ngen. Plans follow the format described in `../CLAUDE.md` ("Plan documents"); new plans get a line here when
created, and the annotations below get updated when a plan lands or is superseded.

Annotations: **(landed)** — implemented, code is in `src/` or `build/`; **(in progress)** — current work; **(superseded by X)** — kept as a record, read X
instead; **(historical)** — describes a direction the project moved away from. Docs without an annotation have not been re-checked against the code — update
them here as you touch them.

## Build system

Current truth for the build framework is [`../build/build_system.md`](../build/build_system.md); the plans below are how it got there.

- [plan_build_system.md](plan_build_system.md) — original build-system plan (superseded by v2)
- [plan_build_system_v2.md](plan_build_system_v2.md) — corrective revision (superseded by v3)
- [plan_build_system_v3.md](plan_build_system_v3.md) — Target/Project/Extension restructure (superseded by v4 for structure; bootstrap sections carry over)
- [plan_build_system_v4.md](plan_build_system_v4.md) — core-vs-language-module boundary; current shape of the build description
- [plan_custom_build_backend.md](plan_custom_build_backend.md) — in-process runner backend replacing ninja (landed)
- [plan_unified_runner.md](plan_unified_runner.md) — one runner library for self-build and project build (landed)
- [plan_remove_ninja_from_bootstrap.md](plan_remove_ninja_from_bootstrap.md) — (superseded, not implemented — record of the path not taken)
- [plan_per_source_target.md](plan_per_source_target.md) — per-source compile flags
- [plan_per_source_target_phase4.md](plan_per_source_target_phase4.md) — user-facing API for per-source flags
- [plan_fuzzy_target_matching.md](plan_fuzzy_target_matching.md) — fuzzy CLI target matching (landed)
- [plan_build_documentation.md](plan_build_documentation.md) — plan for writing `build/build_system.md` (landed)
- [build_concept.md](build_concept.md) — early concept sketch (historical)
- [build_system_design_target_graph_project_model.md](build_system_design_target_graph_project_model.md) — concept explainer: Target / Graph / Project model
- [build_system_extension_type.md](build_system_extension_type.md) — concept explainer: the extension type

## Renderer and frame graph

- [frame_graph_implementation_plan.md](frame_graph_implementation_plan.md) — ngen frame graph, phases 1–3 (landed — see phase summaries)
- [frame_graph_phase1_summary.md](frame_graph_phase1_summary.md) — retrospective: dynamic rendering, core types, compilation
- [frame_graph_phase2_summary.md](frame_graph_phase2_summary.md) — retrospective: resource pools, transient textures
- [frame_graph_phase3_summary.md](frame_graph_phase3_summary.md) — retrospective: lifetime tracking, memory aliasing
- [frame_graph_next.md](frame_graph_next.md) — future frame-graph phases with "when needed" trigger conditions
- [basic_lighting_pass_plan.md](basic_lighting_pass_plan.md) — (superseded by v2)
- [basic_lighting_pass_plan_v2.md](basic_lighting_pass_plan_v2.md) — deferred G-buffer + lighting pass (landed)
- [plan_threaded_rendering.md](plan_threaded_rendering.md) — dedicated render thread with snapshots (landed)
- [plan_incremental_gpu_upload.md](plan_incremental_gpu_upload.md) — incremental mesh/instance uploads
- [plan_imgui_integration.md](plan_imgui_integration.md) — ImGui as a frame-graph pass (landed)
- [debug_renderer_implementation_plan.md](debug_renderer_implementation_plan.md) — debug line rendering (landed)

## Scene and USD

- [plan_for_a_layer_aware_usd_scene_system_modern_c_engine.md](plan_for_a_layer_aware_usd_scene_system_modern_c_engine.md) — deep design for the layer-aware USD scene system (Model A: USD is the scene)
- [plan_usd_scene_system_implementation.md](plan_usd_scene_system_implementation.md) — implementation phases (phases 0–4 landed — see status doc)
- [usd_scene_system_status.md](usd_scene_system_status.md) — what is actually built; phase completion table
- [plan_usd_lights.md](plan_usd_lights.md) — multiple USD lights in the deferred pass (in progress)
- [usd_payloads_and_variants.md](usd_payloads_and_variants.md) — reference notes on payloads and variants
- [usd_driven_scene_system_architecture_modern_c_engine.md](usd_driven_scene_system_architecture_modern_c_engine.md) — Model A vs Model B discussion behind the chosen design
- [architecture_preview_vs_authoring.md](architecture_preview_vs_authoring.md) — preview vs authoring edit flow (no layer writes during preview)
- [plan_prim_creation.md](plan_prim_creation.md) — prim creation UI + procedural shape meshes (landed)
- [plan_background_scene_updates.md](plan_background_scene_updates.md) — scene updates off the main thread
- [plan_incremental_transform_updates.md](plan_incremental_transform_updates.md) — fast path for transform-only changes
- [GLTF_PLAN.md](GLTF_PLAN.md) — (historical — glTF was the intermediate format, replaced by USD)
- [usd_scene_format_integration_modern_c_engine.md](usd_scene_format_integration_modern_c_engine.md) — import-only USD model (historical — not the chosen direction)
- [scene_system_architecture_modern_c_engine.md](scene_system_architecture_modern_c_engine.md) — early generic scene-system notes (historical)

## Editor and UI

- [plan_frame_graph_debug_window.md](plan_frame_graph_debug_window.md) — frame-graph pass/resource inspector (landed)
- [plan_frame_graph_node_view.md](plan_frame_graph_node_view.md) — graph node view with live thumbnails (landed)
- [plan_layer_browser.md](plan_layer_browser.md) — USD layer stack window (landed)
- [plan_undo_redo.md](plan_undo_redo.md) — undo stack over USD edits (landed)
- [plan_translate_gizmo.md](plan_translate_gizmo.md) — translate gizmo (landed)
- [plan_rotate_gizmo.md](plan_rotate_gizmo.md) — rotate gizmo (landed)
- [plan_coordinate_gizmo.md](plan_coordinate_gizmo.md) — 3D axis orientation gizmo (landed)

## Infrastructure

- [plan_job_system.md](plan_job_system.md) — minimal thread-pool job system (landed)
- [plan_async_asset_system.md](plan_async_asset_system.md) — cooked-asset cache and async loading
- [plan_observability.md](plan_observability.md) — observation bus design (landed — usage reference is [`../obs.md`](../obs.md))
- [plan_render_observations.md](plan_render_observations.md) — render-category observations
- [plan_rhi_validation.md](plan_rhi_validation.md) — `ngen-test-rhi` RHI validation program: surfaceless device, readback, analytic pixel checks (draft)
- [observability_api_design_engine_agnostic.md](observability_api_design_engine_agnostic.md) — earlier abstract obs design (historical — the concrete design deliberately diverged)

## Generic references (engine-agnostic)

- [frame_graph_generic_guide.md](frame_graph_generic_guide.md) — frame-graph reference for any engine
- [render_graph_architecture_modern_c_engine.md](render_graph_architecture_modern_c_engine.md) — render-graph architecture notes
- [render_graph_best_of_practical_distillation.md](render_graph_best_of_practical_distillation.md) — render-graph practice distillation
- [render_graph_resource_system_modern_c_renderer.md](render_graph_resource_system_modern_c_renderer.md) — render-graph resource system notes
- [debug_renderer_architecture_modern_c_renderer.md](debug_renderer_architecture_modern_c_renderer.md) — debug renderer architecture notes
- [modern_multi_backend_renderer_architecture_c.md](modern_multi_backend_renderer_architecture_c.md) — multi-backend RHI architecture notes
