#pragma once

#include <shaped-graphics/fwd.hh> // sg's door to <memory>, which the context's command-list methods need

/// Forward declarations for the Metal backend.

namespace sg::backend::metal
{
class autorelease_scope; // an NSAutoreleasePool held for a block (see metal_common.hh)
struct metal_config;     // device creation knobs (see metal_context.hh)
class metal_context;
/// A backend-typed context handle: an sg::context_handle known to point at a metal_context.
/// For code already committed to metal, the backend's own tests above all; a caller drives the abstract sg::context_handle.
using metal_context_handle = std::shared_ptr<metal_context>;
class metal_command_list;
class metal_epoch_system;
class metal_staging_ring;  // CPU-visible bytes an inline transfer stages through (see metal_staging_ring.hh)
class metal_residency_set; // what MTL4 requires instead of useResource (see metal_residency.hh)
class metal_feedback_sink; // the detachable end of a commit-feedback handler (see metal_feedback.hh)
struct metal_barrier;      // one MTL4 barrier, as the stage pair an encoder takes (see metal_barrier.hh)
class metal_binding_group;
class metal_binding_group_layout;
class metal_pipeline_layout;
class metal_staging_binding_group;
class metal_sampler_cache; // MTLSamplerStates for bound sampler values (see metal_sampler_cache.hh)
class metal_buffer;
struct metal_buffer_access; // cross-list access tracking for one buffer (see metal_buffer_access.hh)
class metal_memory_heap;

/// Backend-typed resource handles.
/// No command-list handle: a list is move-only, held by std::unique_ptr<metal_command_list>.
using metal_buffer_handle = std::shared_ptr<metal_buffer const>;
using metal_memory_heap_handle = std::shared_ptr<metal_memory_heap const>;
using metal_binding_group_handle = std::shared_ptr<metal_binding_group const>;
using metal_binding_group_layout_handle = std::shared_ptr<metal_binding_group_layout const>;
using metal_pipeline_layout_handle = std::shared_ptr<metal_pipeline_layout const>;

// metal_texture is declared by the milestone that adds it.

/// The domain every recording site in the Metal backend is attributed to.
/// It shadows sg's, so a backend message is never mistaken for a portable one.
CC_REC_DECLARE_DOMAIN(g_rec_domain);
} // namespace sg::backend::metal
