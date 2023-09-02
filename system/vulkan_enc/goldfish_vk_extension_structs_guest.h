// Copyright (C) 2018 The Android Open Source Project
// Copyright (C) 2018 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Autogenerated module goldfish_vk_extension_structs_guest
//
// (header) generated by codegen/vulkan/scripts/genvk.py -registry codegen/vulkan/xml/vk.xml
// -registryGfxstream codegen/vulkan/xml/vk_gfxstream.xml cereal -o host/vulkan/cereal
//
// Please do not modify directly;
// re-run gfxstream-protocols/scripts/generate-vulkan-sources.sh,
// or directly from Python by defining:
// VULKAN_REGISTRY_XML_DIR : Directory containing vk.xml
// VULKAN_REGISTRY_SCRIPTS_DIR : Directory containing genvk.py
// CEREAL_OUTPUT_DIR: Where to put the generated sources.
//
// python3 $VULKAN_REGISTRY_SCRIPTS_DIR/genvk.py -registry $VULKAN_REGISTRY_XML_DIR/vk.xml cereal -o
// $CEREAL_OUTPUT_DIR
//
#pragma once
#include <vulkan/vulkan.h>

#include "goldfish_vk_private_defs.h"
#include "vk_platform_compat.h"
#include "vulkan_gfxstream.h"
// Stuff we are not going to use but if included,
// will cause compile errors. These are Android Vulkan
// required extensions, but the approach will be to
// implement them completely on the guest side.
#undef VK_KHR_android_surface
#undef VK_ANDROID_external_memory_android_hardware_buffer

namespace gfxstream {
namespace vk {

uint32_t goldfish_vk_struct_type(const void* structExtension);

size_t goldfish_vk_extension_struct_size(VkStructureType rootType, const void* structExtension);

size_t goldfish_vk_extension_struct_size_with_stream_features(uint32_t streamFeatures,
                                                              VkStructureType rootType,
                                                              const void* structExtension);

#ifdef VK_VERSION_1_0
#endif
#ifdef VK_VERSION_1_1
#endif
#ifdef VK_VERSION_1_2
#endif
#ifdef VK_VERSION_1_3
#endif
#ifdef VK_KHR_surface
#endif
#ifdef VK_KHR_swapchain
#endif
#ifdef VK_KHR_display
#endif
#ifdef VK_KHR_display_swapchain
#endif
#ifdef VK_KHR_xlib_surface
#endif
#ifdef VK_KHR_xcb_surface
#endif
#ifdef VK_KHR_wayland_surface
#endif
#ifdef VK_KHR_android_surface
#endif
#ifdef VK_KHR_win32_surface
#endif
#ifdef VK_KHR_sampler_mirror_clamp_to_edge
#endif
#ifdef VK_KHR_video_queue
#endif
#ifdef VK_KHR_video_decode_queue
#endif
#ifdef VK_KHR_video_decode_h264
#endif
#ifdef VK_KHR_dynamic_rendering
#endif
#ifdef VK_KHR_multiview
#endif
#ifdef VK_KHR_get_physical_device_properties2
#endif
#ifdef VK_KHR_device_group
#endif
#ifdef VK_KHR_shader_draw_parameters
#endif
#ifdef VK_KHR_maintenance1
#endif
#ifdef VK_KHR_device_group_creation
#endif
#ifdef VK_KHR_external_memory_capabilities
#endif
#ifdef VK_KHR_external_memory
#endif
#ifdef VK_KHR_external_memory_win32
#endif
#ifdef VK_KHR_external_memory_fd
#endif
#ifdef VK_KHR_win32_keyed_mutex
#endif
#ifdef VK_KHR_external_semaphore_capabilities
#endif
#ifdef VK_KHR_external_semaphore
#endif
#ifdef VK_KHR_external_semaphore_win32
#endif
#ifdef VK_KHR_external_semaphore_fd
#endif
#ifdef VK_KHR_push_descriptor
#endif
#ifdef VK_KHR_shader_float16_int8
#endif
#ifdef VK_KHR_16bit_storage
#endif
#ifdef VK_KHR_incremental_present
#endif
#ifdef VK_KHR_descriptor_update_template
#endif
#ifdef VK_KHR_imageless_framebuffer
#endif
#ifdef VK_KHR_create_renderpass2
#endif
#ifdef VK_KHR_shared_presentable_image
#endif
#ifdef VK_KHR_external_fence_capabilities
#endif
#ifdef VK_KHR_external_fence
#endif
#ifdef VK_KHR_external_fence_win32
#endif
#ifdef VK_KHR_external_fence_fd
#endif
#ifdef VK_KHR_performance_query
#endif
#ifdef VK_KHR_maintenance2
#endif
#ifdef VK_KHR_get_surface_capabilities2
#endif
#ifdef VK_KHR_variable_pointers
#endif
#ifdef VK_KHR_get_display_properties2
#endif
#ifdef VK_KHR_dedicated_allocation
#endif
#ifdef VK_KHR_storage_buffer_storage_class
#endif
#ifdef VK_KHR_relaxed_block_layout
#endif
#ifdef VK_KHR_get_memory_requirements2
#endif
#ifdef VK_KHR_image_format_list
#endif
#ifdef VK_KHR_sampler_ycbcr_conversion
#endif
#ifdef VK_KHR_bind_memory2
#endif
#ifdef VK_KHR_portability_subset
#endif
#ifdef VK_KHR_maintenance3
#endif
#ifdef VK_KHR_draw_indirect_count
#endif
#ifdef VK_KHR_shader_subgroup_extended_types
#endif
#ifdef VK_KHR_8bit_storage
#endif
#ifdef VK_KHR_shader_atomic_int64
#endif
#ifdef VK_KHR_shader_clock
#endif
#ifdef VK_KHR_video_decode_h265
#endif
#ifdef VK_KHR_global_priority
#endif
#ifdef VK_KHR_driver_properties
#endif
#ifdef VK_KHR_shader_float_controls
#endif
#ifdef VK_KHR_depth_stencil_resolve
#endif
#ifdef VK_KHR_swapchain_mutable_format
#endif
#ifdef VK_KHR_timeline_semaphore
#endif
#ifdef VK_KHR_vulkan_memory_model
#endif
#ifdef VK_KHR_shader_terminate_invocation
#endif
#ifdef VK_KHR_fragment_shading_rate
#endif
#ifdef VK_KHR_spirv_1_4
#endif
#ifdef VK_KHR_surface_protected_capabilities
#endif
#ifdef VK_KHR_separate_depth_stencil_layouts
#endif
#ifdef VK_KHR_present_wait
#endif
#ifdef VK_KHR_uniform_buffer_standard_layout
#endif
#ifdef VK_KHR_buffer_device_address
#endif
#ifdef VK_KHR_deferred_host_operations
#endif
#ifdef VK_KHR_pipeline_executable_properties
#endif
#ifdef VK_KHR_map_memory2
#endif
#ifdef VK_KHR_shader_integer_dot_product
#endif
#ifdef VK_KHR_pipeline_library
#endif
#ifdef VK_KHR_shader_non_semantic_info
#endif
#ifdef VK_KHR_present_id
#endif
#ifdef VK_KHR_video_encode_queue
#endif
#ifdef VK_KHR_synchronization2
#endif
#ifdef VK_KHR_fragment_shader_barycentric
#endif
#ifdef VK_KHR_shader_subgroup_uniform_control_flow
#endif
#ifdef VK_KHR_zero_initialize_workgroup_memory
#endif
#ifdef VK_KHR_workgroup_memory_explicit_layout
#endif
#ifdef VK_KHR_copy_commands2
#endif
#ifdef VK_KHR_format_feature_flags2
#endif
#ifdef VK_KHR_ray_tracing_maintenance1
#endif
#ifdef VK_KHR_portability_enumeration
#endif
#ifdef VK_KHR_maintenance4
#endif
#ifdef VK_KHR_ray_tracing_position_fetch
#endif
#ifdef VK_ANDROID_native_buffer
#endif
#ifdef VK_EXT_debug_report
#endif
#ifdef VK_NV_glsl_shader
#endif
#ifdef VK_EXT_depth_range_unrestricted
#endif
#ifdef VK_IMG_filter_cubic
#endif
#ifdef VK_AMD_rasterization_order
#endif
#ifdef VK_AMD_shader_trinary_minmax
#endif
#ifdef VK_AMD_shader_explicit_vertex_parameter
#endif
#ifdef VK_EXT_debug_marker
#endif
#ifdef VK_AMD_gcn_shader
#endif
#ifdef VK_NV_dedicated_allocation
#endif
#ifdef VK_EXT_transform_feedback
#endif
#ifdef VK_NVX_binary_import
#endif
#ifdef VK_NVX_image_view_handle
#endif
#ifdef VK_AMD_draw_indirect_count
#endif
#ifdef VK_AMD_negative_viewport_height
#endif
#ifdef VK_AMD_gpu_shader_half_float
#endif
#ifdef VK_AMD_shader_ballot
#endif
#ifdef VK_EXT_video_encode_h264
#endif
#ifdef VK_EXT_video_encode_h265
#endif
#ifdef VK_AMD_texture_gather_bias_lod
#endif
#ifdef VK_AMD_shader_info
#endif
#ifdef VK_AMD_shader_image_load_store_lod
#endif
#ifdef VK_GGP_stream_descriptor_surface
#endif
#ifdef VK_NV_corner_sampled_image
#endif
#ifdef VK_IMG_format_pvrtc
#endif
#ifdef VK_NV_external_memory_capabilities
#endif
#ifdef VK_NV_external_memory
#endif
#ifdef VK_NV_external_memory_win32
#endif
#ifdef VK_NV_win32_keyed_mutex
#endif
#ifdef VK_EXT_validation_flags
#endif
#ifdef VK_NN_vi_surface
#endif
#ifdef VK_EXT_shader_subgroup_ballot
#endif
#ifdef VK_EXT_shader_subgroup_vote
#endif
#ifdef VK_EXT_texture_compression_astc_hdr
#endif
#ifdef VK_EXT_astc_decode_mode
#endif
#ifdef VK_EXT_pipeline_robustness
#endif
#ifdef VK_EXT_conditional_rendering
#endif
#ifdef VK_NV_clip_space_w_scaling
#endif
#ifdef VK_EXT_direct_mode_display
#endif
#ifdef VK_EXT_acquire_xlib_display
#endif
#ifdef VK_EXT_display_surface_counter
#endif
#ifdef VK_EXT_display_control
#endif
#ifdef VK_GOOGLE_display_timing
#endif
#ifdef VK_NV_sample_mask_override_coverage
#endif
#ifdef VK_NV_geometry_shader_passthrough
#endif
#ifdef VK_NV_viewport_array2
#endif
#ifdef VK_NVX_multiview_per_view_attributes
#endif
#ifdef VK_NV_viewport_swizzle
#endif
#ifdef VK_EXT_discard_rectangles
#endif
#ifdef VK_EXT_conservative_rasterization
#endif
#ifdef VK_EXT_depth_clip_enable
#endif
#ifdef VK_EXT_swapchain_colorspace
#endif
#ifdef VK_EXT_hdr_metadata
#endif
#ifdef VK_MVK_ios_surface
#endif
#ifdef VK_MVK_macos_surface
#endif
#ifdef VK_EXT_external_memory_dma_buf
#endif
#ifdef VK_EXT_queue_family_foreign
#endif
#ifdef VK_EXT_debug_utils
#endif
#ifdef VK_ANDROID_external_memory_android_hardware_buffer
#endif
#ifdef VK_EXT_sampler_filter_minmax
#endif
#ifdef VK_AMD_gpu_shader_int16
#endif
#ifdef VK_AMD_mixed_attachment_samples
#endif
#ifdef VK_AMD_shader_fragment_mask
#endif
#ifdef VK_EXT_inline_uniform_block
#endif
#ifdef VK_EXT_shader_stencil_export
#endif
#ifdef VK_EXT_sample_locations
#endif
#ifdef VK_EXT_blend_operation_advanced
#endif
#ifdef VK_NV_fragment_coverage_to_color
#endif
#ifdef VK_NV_framebuffer_mixed_samples
#endif
#ifdef VK_NV_fill_rectangle
#endif
#ifdef VK_NV_shader_sm_builtins
#endif
#ifdef VK_EXT_post_depth_coverage
#endif
#ifdef VK_EXT_image_drm_format_modifier
#endif
#ifdef VK_EXT_validation_cache
#endif
#ifdef VK_EXT_descriptor_indexing
#endif
#ifdef VK_EXT_shader_viewport_index_layer
#endif
#ifdef VK_NV_shading_rate_image
#endif
#ifdef VK_NV_ray_tracing
#endif
#ifdef VK_NV_representative_fragment_test
#endif
#ifdef VK_EXT_filter_cubic
#endif
#ifdef VK_QCOM_render_pass_shader_resolve
#endif
#ifdef VK_EXT_global_priority
#endif
#ifdef VK_EXT_external_memory_host
#endif
#ifdef VK_AMD_buffer_marker
#endif
#ifdef VK_AMD_pipeline_compiler_control
#endif
#ifdef VK_EXT_calibrated_timestamps
#endif
#ifdef VK_AMD_shader_core_properties
#endif
#ifdef VK_AMD_memory_overallocation_behavior
#endif
#ifdef VK_EXT_vertex_attribute_divisor
#endif
#ifdef VK_GGP_frame_token
#endif
#ifdef VK_EXT_pipeline_creation_feedback
#endif
#ifdef VK_NV_shader_subgroup_partitioned
#endif
#ifdef VK_NV_compute_shader_derivatives
#endif
#ifdef VK_NV_mesh_shader
#endif
#ifdef VK_NV_fragment_shader_barycentric
#endif
#ifdef VK_NV_shader_image_footprint
#endif
#ifdef VK_NV_scissor_exclusive
#endif
#ifdef VK_NV_device_diagnostic_checkpoints
#endif
#ifdef VK_INTEL_shader_integer_functions2
#endif
#ifdef VK_INTEL_performance_query
#endif
#ifdef VK_EXT_pci_bus_info
#endif
#ifdef VK_AMD_display_native_hdr
#endif
#ifdef VK_FUCHSIA_imagepipe_surface
#endif
#ifdef VK_EXT_metal_surface
#endif
#ifdef VK_EXT_fragment_density_map
#endif
#ifdef VK_EXT_scalar_block_layout
#endif
#ifdef VK_GOOGLE_hlsl_functionality1
#endif
#ifdef VK_GOOGLE_decorate_string
#endif
#ifdef VK_EXT_subgroup_size_control
#endif
#ifdef VK_AMD_shader_core_properties2
#endif
#ifdef VK_AMD_device_coherent_memory
#endif
#ifdef VK_EXT_shader_image_atomic_int64
#endif
#ifdef VK_EXT_memory_budget
#endif
#ifdef VK_EXT_memory_priority
#endif
#ifdef VK_NV_dedicated_allocation_image_aliasing
#endif
#ifdef VK_EXT_buffer_device_address
#endif
#ifdef VK_EXT_tooling_info
#endif
#ifdef VK_EXT_separate_stencil_usage
#endif
#ifdef VK_EXT_validation_features
#endif
#ifdef VK_NV_cooperative_matrix
#endif
#ifdef VK_NV_coverage_reduction_mode
#endif
#ifdef VK_EXT_fragment_shader_interlock
#endif
#ifdef VK_EXT_ycbcr_image_arrays
#endif
#ifdef VK_EXT_provoking_vertex
#endif
#ifdef VK_EXT_full_screen_exclusive
#endif
#ifdef VK_EXT_headless_surface
#endif
#ifdef VK_EXT_line_rasterization
#endif
#ifdef VK_EXT_shader_atomic_float
#endif
#ifdef VK_EXT_host_query_reset
#endif
#ifdef VK_EXT_index_type_uint8
#endif
#ifdef VK_EXT_extended_dynamic_state
#endif
#ifdef VK_EXT_shader_atomic_float2
#endif
#ifdef VK_EXT_surface_maintenance1
#endif
#ifdef VK_EXT_swapchain_maintenance1
#endif
#ifdef VK_EXT_shader_demote_to_helper_invocation
#endif
#ifdef VK_NV_device_generated_commands
#endif
#ifdef VK_NV_inherited_viewport_scissor
#endif
#ifdef VK_EXT_texel_buffer_alignment
#endif
#ifdef VK_QCOM_render_pass_transform
#endif
#ifdef VK_EXT_device_memory_report
#endif
#ifdef VK_EXT_acquire_drm_display
#endif
#ifdef VK_EXT_robustness2
#endif
#ifdef VK_EXT_custom_border_color
#endif
#ifdef VK_GOOGLE_user_type
#endif
#ifdef VK_NV_present_barrier
#endif
#ifdef VK_EXT_private_data
#endif
#ifdef VK_EXT_pipeline_creation_cache_control
#endif
#ifdef VK_NV_device_diagnostics_config
#endif
#ifdef VK_QCOM_render_pass_store_ops
#endif
#ifdef VK_NV_low_latency
#endif
#ifdef VK_EXT_metal_objects
#endif
#ifdef VK_EXT_descriptor_buffer
#endif
#ifdef VK_EXT_graphics_pipeline_library
#endif
#ifdef VK_AMD_shader_early_and_late_fragment_tests
#endif
#ifdef VK_NV_fragment_shading_rate_enums
#endif
#ifdef VK_NV_ray_tracing_motion_blur
#endif
#ifdef VK_EXT_ycbcr_2plane_444_formats
#endif
#ifdef VK_EXT_fragment_density_map2
#endif
#ifdef VK_QCOM_rotated_copy_commands
#endif
#ifdef VK_EXT_image_robustness
#endif
#ifdef VK_EXT_image_compression_control
#endif
#ifdef VK_EXT_attachment_feedback_loop_layout
#endif
#ifdef VK_EXT_4444_formats
#endif
#ifdef VK_EXT_device_fault
#endif
#ifdef VK_ARM_rasterization_order_attachment_access
#endif
#ifdef VK_EXT_rgba10x6_formats
#endif
#ifdef VK_NV_acquire_winrt_display
#endif
#ifdef VK_EXT_directfb_surface
#endif
#ifdef VK_VALVE_mutable_descriptor_type
#endif
#ifdef VK_EXT_vertex_input_dynamic_state
#endif
#ifdef VK_EXT_physical_device_drm
#endif
#ifdef VK_EXT_device_address_binding_report
#endif
#ifdef VK_EXT_depth_clip_control
#endif
#ifdef VK_EXT_primitive_topology_list_restart
#endif
#ifdef VK_FUCHSIA_external_memory
#endif
#ifdef VK_FUCHSIA_external_semaphore
#endif
#ifdef VK_FUCHSIA_buffer_collection
#endif
#ifdef VK_HUAWEI_subpass_shading
#endif
#ifdef VK_HUAWEI_invocation_mask
#endif
#ifdef VK_NV_external_memory_rdma
#endif
#ifdef VK_EXT_pipeline_properties
#endif
#ifdef VK_EXT_multisampled_render_to_single_sampled
#endif
#ifdef VK_EXT_extended_dynamic_state2
#endif
#ifdef VK_QNX_screen_surface
#endif
#ifdef VK_EXT_color_write_enable
#endif
#ifdef VK_EXT_primitives_generated_query
#endif
#ifdef VK_GOOGLE_gfxstream
#endif
#ifdef VK_EXT_global_priority_query
#endif
#ifdef VK_EXT_image_view_min_lod
#endif
#ifdef VK_EXT_multi_draw
#endif
#ifdef VK_EXT_image_2d_view_of_3d
#endif
#ifdef VK_EXT_shader_tile_image
#endif
#ifdef VK_EXT_opacity_micromap
#endif
#ifdef VK_NV_displacement_micromap
#endif
#ifdef VK_EXT_load_store_op_none
#endif
#ifdef VK_HUAWEI_cluster_culling_shader
#endif
#ifdef VK_EXT_border_color_swizzle
#endif
#ifdef VK_EXT_pageable_device_local_memory
#endif
#ifdef VK_ARM_shader_core_properties
#endif
#ifdef VK_EXT_image_sliced_view_of_3d
#endif
#ifdef VK_VALVE_descriptor_set_host_mapping
#endif
#ifdef VK_EXT_depth_clamp_zero_one
#endif
#ifdef VK_EXT_non_seamless_cube_map
#endif
#ifdef VK_QCOM_fragment_density_map_offset
#endif
#ifdef VK_NV_copy_memory_indirect
#endif
#ifdef VK_NV_memory_decompression
#endif
#ifdef VK_NV_linear_color_attachment
#endif
#ifdef VK_GOOGLE_surfaceless_query
#endif
#ifdef VK_EXT_image_compression_control_swapchain
#endif
#ifdef VK_QCOM_image_processing
#endif
#ifdef VK_EXT_extended_dynamic_state3
#endif
#ifdef VK_EXT_subpass_merge_feedback
#endif
#ifdef VK_LUNARG_direct_driver_loading
#endif
#ifdef VK_EXT_shader_module_identifier
#endif
#ifdef VK_EXT_rasterization_order_attachment_access
#endif
#ifdef VK_NV_optical_flow
#endif
#ifdef VK_EXT_legacy_dithering
#endif
#ifdef VK_EXT_pipeline_protected_access
#endif
#ifdef VK_EXT_shader_object
#endif
#ifdef VK_QCOM_tile_properties
#endif
#ifdef VK_SEC_amigo_profiling
#endif
#ifdef VK_QCOM_multiview_per_view_viewports
#endif
#ifdef VK_NV_ray_tracing_invocation_reorder
#endif
#ifdef VK_EXT_mutable_descriptor_type
#endif
#ifdef VK_ARM_shader_core_builtins
#endif
#ifdef VK_EXT_pipeline_library_group_handles
#endif
#ifdef VK_QCOM_multiview_per_view_render_areas
#endif
#ifdef VK_EXT_attachment_feedback_loop_dynamic_state
#endif
#ifdef VK_KHR_acceleration_structure
#endif
#ifdef VK_KHR_ray_tracing_pipeline
#endif
#ifdef VK_KHR_ray_query
#endif
#ifdef VK_EXT_mesh_shader
#endif

}  // namespace vk
}  // namespace gfxstream
