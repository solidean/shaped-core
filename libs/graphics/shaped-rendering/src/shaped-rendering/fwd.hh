#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/record/domain_fwd.hh>
#include <shaped-graphics/fwd.hh>

/// Aggregate forward declarations for shaped-rendering.
/// Include when a forward decl is all you need.

namespace sr
{
// the capture protocol (see capture.hh)
struct capture_request;

// Vocabulary types (i32/u32/f32/isize/...) available bare inside sr, not leaked globally.
using namespace cc::primitive_defines;

/// A `bool` in the layout a GPU constant buffer expects: one 32-bit lane (see gpu_types.hh).
struct gpu_boolean;

// Concrete render routines land here as they are implemented;
// the routine framework itself lives in shaped-graphics (sg::render_routine / ctx.routines).
class blit_routine; // fullscreen-triangle blit of a source texture across an open raster scope (see blit_routine.hh)
class box_filter_mipmap_routine;        // fills a texture's mip chain by 2x2 averaging (box_filter_mipmap_routine.hh)
class raster_box_filter_mipmap_routine; // the same, through the raster pipeline, for formats no typed UAV covers
enum class mipmap_variant : u8;         // which entry point a texture shape mips through (the routine's parameter)
struct mipmap_program;                  // one mipmap variant's group layout + compute pipeline

// Denoising (see denoise.hh): one front routine over several members.
enum class denoise_method : u8;      // which member runs, or automatic
enum class denoise_quality : u8;     // the coarse knob every member maps
enum class render_scale_preset : u8; // how much smaller than the output the caller traces
enum class denoise_guide : u8;       // one guide buffer beside the noisy color
enum class denoise_status : u8;      // what one call did
struct denoise_settings;             // the knobs shared by every member
struct denoise_guides;               // the guide textures and camera values of one call
struct denoise_inputs;               // one call's images
struct denoise_outcome;              // status + which member + whether history restarted
struct denoise_support;              // which members a context can run
class denoise_history;               // the caller-owned state of one image stream
class denoise_routine;               // the front: resolves the method and forwards
class atrous_denoise_routine;        // the native spatial member (atrous_denoise_routine.hh)
struct atrous_options;               // its own options
class svgf_denoise_routine;          // the native temporal member (svgf_denoise_routine.hh)
struct svgf_options;                 // its own options

class dlss_rr_routine; // the NVIDIA vendor member (dlss_rr_routine.hh)
struct dlss_options;   // its own options

class mix_routine; // one image faded into another, in place (mix_routine.hh)

// Dear ImGui integration (see imgui_context.hh).
struct imgui_context_description; // value type — input to imgui_context


// OS windows (see window.hh).
// Always declared; SR_HAS_WINDOW says whether a backend was built in, and without one creation fails.
struct window_description;        // value type — input to create_window
struct window_system_description; // value type — input to window_system::create
class window;
class window_system;
enum class cursor_shape : u8; // the pointer's shape, set on the system (see window_system::set_cursor)

// Input (see input.hh) — what poll_events collected, drained through window_system::events().
enum class scancode : u16;
enum class mouse_button : u8;
enum class key_modifiers : u8;
struct key_event;
struct text_event;
struct mouse_move_event;
struct mouse_button_event;
struct mouse_wheel_event;
struct input_event;

// Dear ImGui, drawn through sg.
// imgui_context owns the ImGui context and the frame bracket;
// imgui_routine owns the GPU resources and records the draws.
class imgui_context;
class imgui_routine;

namespace impl
{
class imgui_texture_registry;
class imgui_texture_routine; // the routine owning that registry, shared by every imgui_routine parametrization
} // namespace impl

/// The domain every recording site in shaped-rendering is attributed to.
CC_REC_DECLARE_DOMAIN(g_rec_domain);
} // namespace sr
