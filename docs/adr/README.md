# Architecture decision records

The architecture decisions behind ngen, one per file, in the order they were made. Each record uses Michael Nygard's format — Status, Context,
Decision, Consequences — plus a short Verification section saying where the decision is in the code today and where the code differs from the plan
that made it. See [ADR 77](0077_record_architecture_decisions.md) for why and how.

The plans in `docs/` are how the engine got here, step by step; these records pull out the decisions those plans made, including the ones later
reversed. A superseded record is kept and says what replaced it. The first 76 records were extracted from `docs/` on 2026-11-04 and checked
against the code at `d83048c`; dates are when the decision was made, taken from the git history of the plan that made it.

Status values: **Accepted** — holds in the code; **Accepted, not implemented** — decided, not built yet; **Proposed** — a draft plan's decision, not
locked; **Superseded by N** — replaced, kept as a record.

## Log

| ADR | Date | Decision | Status |
|---|---|---|---|
| [1](0001_gltf_as_first_scene_format.md) | 2026-03-31 | glTF (cgltf) as the first scene format | Superseded by 3, 6 |
| [2](0002_thin_backend_agnostic_rhi.md) | 2026-04-01 | Thin backend-agnostic RHI, backends in sibling folders | Accepted |
| [3](0003_openusd_vendored_not_tinyusdz.md) | 2026-04-02 | Full OpenUSD as a vendored submodule, not tinyusdz | Accepted |
| [4](0004_renderer_takes_engine_native_inputs_no_hydra.md) | 2026-04-02 | The renderer takes engine-native flat inputs, no Hydra | Accepted |
| [5](0005_frame_graph.md) | 2026-04-03 | A frame graph owns pass ordering, culling, barriers and transients | Accepted |
| [6](0006_usd_stage_is_the_scene.md) | 2026-04-06 | The USD stage is the scene in ngen-view (Model A) | Superseded by 52 |
| [7](0007_deferred_shading.md) | 2026-04-12 | Deferred shading: G-buffer and a lighting pass | Accepted |
| [8](0008_render_thread_with_snapshots.md) | 2026-04-12 | A render thread fed by snapshots, one-slot back-pressure | Accepted |
| [9](0009_preview_vs_authoring_edits.md) | 2026-04-13 | Preview vs authoring edits | Accepted |
| [10](0010_observation_bus.md) | 2026-04-22 | An observation bus (JSONL) for agent verification | Superseded by 47, 48 |
| [11](0011_build_described_in_cpp.md) | 2026-04-25 | The build is a C++ program written against a reusable framework | Accepted |
| [12](0012_ninja_executor_and_staged_bootstrap.md) | 2026-04-25 | Ninja as executor behind a multi-stage bootstrap | Superseded by 15, 17 |
| [13](0013_language_agnostic_build_core.md) | 2026-05-02 | A language-agnostic build core with a `build::cxx` extension | Accepted |
| [14](0014_translation_units_as_graph_nodes.md) | 2026-05-03 | Every translation unit is a build graph node | Accepted |
| [15](0015_custom_runner_and_binary_ir.md) | 2026-05-03 | A custom runner fed by a binary IR replaces Ninja | Accepted |
| [16](0016_fuzzy_target_matching.md) | 2026-05-03 | Targets resolved by exact name, then fuzzy substring | Accepted |
| [17](0017_one_runner_one_command_seed.md) | 2026-05-13 | One runner for self-build and project build, one `c++` seed | Accepted |
| [18](0018_no_project_defaults_in_build_system.md) | 2026-05-18 | No project defaults: platform and config always required | Accepted |
| [19](0019_rhi_executes_engine_schedules.md) | 2026-09-19 | The RHI executes, the engine schedules | Accepted |
| [20](0020_no_window_layer_in_rhi.md) | 2026-09-19 | No window layer in the RHI (`RhiWindow`) | Accepted |
| [21](0021_rhi_example_programs.md) | 2026-09-19 | The RHI is verified by example programs with analytic checks | Accepted |
| [22](0022_cpu_frustum_culling.md) | 2026-09-20 | CPU frustum culling on the main thread | Superseded by 29 |
| [23](0023_profiling_markers.md) | 2026-09-20 | Profiling markers as their own system, ready for Tracy | Accepted |
| [24](0024_one_verb_set_for_flags_and_scripts.md) | 2026-09-20 | One verb set for flags and scripts: headless sessions | Accepted |
| [25](0025_vulkan_13_baseline.md) | 2026-09-25 | Vulkan 1.3 baseline with required features, no fallbacks | Accepted |
| [26](0026_gpu_driven_only_path.md) | 2026-09-25 | GPU-driven is the only path | Accepted |
| [27](0027_frame_graph_buffers_imported_only.md) | 2026-09-25 | Frame-graph buffers are imported only | Accepted |
| [28](0028_bindless_sampler_array.md) | 2026-09-25 | Bindless materials as a fixed sampler array | Accepted |
| [29](0029_gpu_culling_ordered_compaction.md) | 2026-09-25 | GPU culling with ordered compaction | Accepted |
| [30](0030_cascaded_shadow_atlas.md) | 2026-09-25 | Cascaded shadows in one atlas with hardware compare | Accepted |
| [31](0031_introspection_principles.md) | 2026-09-26 | Introspection: dump parity, one capture service, own debugger plus RenderDoc | Accepted |
| [32](0032_build_system_in_src_std_only.md) | 2026-09-29 | The build system lives in `src/build/`, standard library only | Accepted |
| [33](0033_ngen_cli_front_door.md) | 2026-09-29 | `ngen-cli` is the front door, the variant in `_out/set` | Accepted |
| [34](0034_process_split.md) | 2026-09-29 | Process split: USD-free view, editor, asset server, introspection tool | Accepted |
| [35](0035_json_rpc_over_loopback_tcp.md) | 2026-09-29 | RPC: JSON-RPC 2.0 over loopback TCP with binary attachments | Accepted |
| [36](0036_one_endpoint_per_process.md) | 2026-09-29 | One endpoint per process, discovery files, no broker | Accepted |
| [37](0037_packing_inside_build_system.md) | 2026-09-29 | Packing inside the build system (`pack_rule`) | Superseded by 42 |
| [38](0038_one_packer_program_per_type.md) | 2026-09-29 | One packer program per asset type, with a fixed contract | Accepted |
| [39](0039_asset_ids_are_paths.md) | 2026-09-29 | Asset ids are paths; the version is the content hash | Accepted |
| [40](0040_program_mains_in_src_apps.md) | 2026-09-29 | Every program's `main` in `src/apps/` | Accepted |
| [41](0041_pipeline_registry.md) | 2026-09-29 | A pipeline registry: nothing waits for a shader | Accepted |
| [42](0042_separate_asset_server.md) | 2026-10-01 | A separate `ngen-asset-server`; the build stays one-shot | Accepted |
| [43](0043_packed_data_streamed_over_rpc.md) | 2026-10-01 | Packed data streamed over RPC; the view needs a server | Accepted |
| [44](0044_no_project_root.md) | 2026-10-02 | No project root: tools are rooted at their working directory | Accepted |
| [45](0045_asset_system_is_a_lower_layer.md) | 2026-10-02 | The asset system is a lower layer, used through `AssetClient` | Accepted |
| [46](0046_records_and_ngen_introspect.md) | 2026-10-03 | Records on every endpoint, and `ngen-introspect` | Accepted |
| [47](0047_trace_library.md) | 2026-10-03 | A trace library: always on, one clock, streamed over RPC | Accepted |
| [48](0048_trace_is_flow_data_is_record.md) | 2026-10-06 | The trace is flow and messages; data is a record | Accepted |
| [49](0049_streaming_and_lod_one_system.md) | 2026-10-11 | Streaming and LOD are one system | Accepted |
| [50](0050_usdlod_only_lod_convention.md) | 2026-10-11 | UsdLod is the only LOD convention | Accepted |
| [51](0051_streaming_structure_authored.md) | 2026-10-12 | The streaming structure is authored, not computed | Accepted |
| [52](0052_view_renders_layer_packs.md) | 2026-10-12 | ngen-view renders layer packs and links no OpenUSD | Accepted |
| [53](0053_worldcomposer_composes_as_pcp.md) | 2026-10-12 | `WorldComposer` composes packs as USD's Pcp does | Accepted |
| [54](0054_async_scene_and_incremental_gpu_scene.md) | 2026-10-13 | A scene thread and an incremental GPU scene: the view never waits | Accepted |
| [55](0055_reversed_z_infinite_far.md) | 2026-10-13 | Reversed-Z with an infinite far plane | Accepted |
| [56](0056_cutout_buckets.md) | 2026-10-13 | Cutout materials in their own buckets and pipelines | Accepted |
| [57](0057_asset_change_notices.md) | 2026-10-13 | Asset change notices: the server notifies, the client requests again | Accepted |
| [58](0058_editor_process_owns_usd.md) | 2026-10-13 | `ngen-editor` owns USD; changes reach views as deltas | Proposed |
| [59](0059_districts_streaming.md) | 2026-10-15 | Districts streaming: kept contents and a whole-world extract | Superseded by 67 |
| [60](0060_time_based_unload_delay.md) | 2026-10-15 | A time-based unload delay | Superseded by 70 |
| [61](0061_switch_when_complete.md) | 2026-10-15 | Switch only when the finer level is complete; no frustum in the heuristic | Accepted |
| [62](0062_runtime_access_and_visibility.md) | 2026-10-15 | Runtime access and visibility as a standing rule | Accepted |
| [63](0063_engine_map_and_scene_flow_windows.md) | 2026-10-15 | Engine Map and Scene Flow windows | Superseded by 62 |
| [64](0064_meshlets_on_mesh_shaders.md) | 2026-10-15 | Meshlets on `VK_EXT_mesh_shader` with occlusion culling | Accepted, not implemented |
| [65](0065_test_world_over_caldera.md) | 2026-10-16 | A test world authored to stream, over fitting Caldera | Accepted |
| [66](0066_variants_and_pointinstancer.md) | 2026-10-16 | Variant sets and PointInstancer in the World | Accepted |
| [67](0067_placement_tree.md) | 2026-10-16 | Streaming v2: a placement tree of immutable contents | Accepted |
| [68](0068_render_lod_on_gpu.md) | 2026-10-19 | Render LOD: inline levels picked on the GPU, payload levels stream | Accepted |
| [69](0069_lod_heuristics_and_dither_fade.md) | 2026-10-25 | Both LOD heuristics; dither cross-fade over the authored band | Accepted |
| [70](0070_residency_from_streaming_radii.md) | 2026-10-30 | Residency from the authored streaming radii, with no time | Accepted |
| [71](0071_requests_never_cancelled_no_budgets.md) | 2026-10-30 | Requests are never cancelled; budgets set aside | Accepted |
| [72](0072_main_camera_decides.md) | 2026-10-31 | The main camera decides; the debug camera observes | Accepted |
| [73](0073_ground_following_naive_ray.md) | 2026-10-31 | Ground following with a naive ray; collision waits for physics | Accepted |
| [74](0074_configuration_values.md) | 2026-10-31 | Configuration: typed values declared by their owner | Accepted |
| [75](0075_engine_layering.md) | 2026-10-31 | Engine layering, the tools apart from the UI | Accepted |
| [76](0076_viewframe_steps_and_hooks.md) | 2026-11-01 | The frame loop as `ViewFrame` steps and `ViewHooks` | Accepted |
| [77](0077_record_architecture_decisions.md) | 2026-11-04 | Record architecture decisions | Accepted |
| [78](0078_render_extensions.md) | 2026-11-05 | Render extensions: what isn't the renderer's core joins its frame | Accepted |
| [79](0079_dome_light_sky_and_irradiance.md) | 2026-11-05 | The dome light as the sky, its irradiance computed when packed | Accepted |

## By area

- **Build:** 11, 12, 13, 14, 15, 16, 17, 18, 32, 33, 40
- **RHI:** 2, 19, 20, 21, 25, 64
- **Renderer:** 5, 7, 8, 22, 26, 27, 28, 29, 30, 41, 55, 56, 68, 78, 79
- **Scene, USD and world:** 1, 3, 4, 6, 9, 52, 53, 54, 58, 66
- **Tools, RPC and assets:** 34, 35, 36, 37, 38, 39, 42, 43, 44, 45, 57
- **Streaming, LOD and camera:** 49, 50, 51, 59, 60, 61, 65, 67, 69, 70, 71, 72, 73
- **Observability, configuration and layering:** 10, 23, 24, 31, 46, 47, 48, 62, 63, 74, 75, 76, 78

## Adding a record

A new architecture decision gets the next number and a file `NNNN_snake_case_title.md` here, plus a row in the log above. Copy the shape of an
existing record. When a decision replaces an earlier one, the new record says "Supersedes N" under its status, and the old record's status becomes
"Superseded by N" — the old record is otherwise left as it was. A plan that makes an architecture decision records it here when it lands.
