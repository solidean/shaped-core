#pragma once

#include <shaped-graphics/fwd.hh> // sg's door to <memory>, which the context's command-list methods need

/// Forward declarations for the WebGPU backend.

namespace sg::backend::webgpu
{
template <class T>
struct wgpu_handle;       // an owning reference to one WebGPU object (see webgpu_common.hh)
struct texel_copy_layout; // how a buffer-to-texture copy places rows (see webgpu_format.hh)
struct webgpu_config;     // device request knobs (see webgpu_context.hh)
class webgpu_context;
/// A backend-typed context handle: an sg::context_handle known to point at a webgpu_context.
using webgpu_context_handle = std::shared_ptr<webgpu_context>;
class webgpu_command_list;
class webgpu_buffer;
using webgpu_buffer_handle = std::shared_ptr<webgpu_buffer const>;
class webgpu_texture;
using webgpu_texture_handle = std::shared_ptr<webgpu_texture const>;
class webgpu_memory_heap; // a heap that places nothing: WebGPU exposes no memory heaps
using webgpu_memory_heap_handle = std::shared_ptr<webgpu_memory_heap const>;
class webgpu_sampler_cache; // WGPUSamplers keyed by sampler identity
class webgpu_binding_group_layout;
using webgpu_binding_group_layout_handle = std::shared_ptr<webgpu_binding_group_layout const>;
class webgpu_pipeline_layout;
using webgpu_pipeline_layout_handle = std::shared_ptr<webgpu_pipeline_layout const>;
class webgpu_binding_group;
using webgpu_binding_group_handle = std::shared_ptr<webgpu_binding_group const>;
class webgpu_compute_pipeline;
using webgpu_compute_pipeline_handle = std::shared_ptr<webgpu_compute_pipeline const>;
class webgpu_raster_pipeline;
using webgpu_raster_pipeline_handle = std::shared_ptr<webgpu_raster_pipeline const>;
class webgpu_swapchain;
using webgpu_swapchain_handle = std::shared_ptr<webgpu_swapchain>;
struct webgpu_upload_span;       // one reservation in the upload ring (see webgpu_upload_ring.hh)
class webgpu_upload_ring;        // the unmapped ring behind cmd.upload
struct webgpu_readback;          // one staged readback awaiting its map (see webgpu_readback.hh)
class webgpu_readback_pool;      // MAP_READ buffers behind every download
struct webgpu_constant_page;     // one uniform page inline constants are written into
class webgpu_constant_pages;     // the dynamic-offset uniform pages behind inline constants
class webgpu_stream_system;      // ctx.stream, windowed from a pump
struct webgpu_query_lease;       // one timestamp query set leased by a command list
class webgpu_query_system;       // the pool of them
struct webgpu_expiring_resource; // a released object awaiting its epoch
struct webgpu_epoch_data;        // what one epoch owns
struct webgpu_epoch_state;       // epoch counters and the in-flight FIFO
struct webgpu_callback_anchor;   // what a WebGPU callback reaches the context through

/// The domain every recording site in the WebGPU backend is attributed to.
/// It shadows sg's, so a backend message is never mistaken for a portable one.
CC_REC_DECLARE_DOMAIN(g_rec_domain);
} // namespace sg::backend::webgpu
