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

// Concrete render routines land here as they are implemented;
// the routine framework itself lives in shaped-graphics (sg::render_routine / ctx.routines).
class blit_routine; // fullscreen-triangle blit of a source texture across an open raster scope (see blit_routine.hh)
class box_filter_mipmap_routine;        // fills a texture's mip chain by 2x2 averaging (box_filter_mipmap_routine.hh)
class raster_box_filter_mipmap_routine; // the same, through the raster pipeline, for formats no typed UAV covers
enum class mipmap_variant : u8;         // which entry point a texture shape mips through (the routine's parameter)
struct mipmap_program;                  // one mipmap variant's group layout + compute pipeline

// Reconstruction (see reconstruct.hh): one front routine over denoise and upscale members.
enum class denoise_method : u8;      // which member runs, or automatic
enum class upscale_method : u8;      // which upscaler runs behind it, or automatic
enum class denoise_quality : u8;     // the coarse knob every member maps
enum class render_scale_preset : u8; // how much smaller than the output the caller traces
enum class reconstruct_guide : u8;   // one guide buffer beside the noisy color
enum class reconstruct_status : u8;  // what one call did
struct reconstruct_settings;         // the knobs shared by every member
struct reconstruct_guides;           // the guide textures and camera values of one call
struct reconstruct_inputs;           // one call's images
struct reconstruct_outcome;          // status + which member + whether history restarted
struct reconstruct_support;          // which members a context can run
class reconstruct_history;           // the caller-owned state of one image stream
class reconstruct_routine;           // the front: resolves the method and forwards
class atrous_denoise_routine;        // the native spatial member (atrous_denoise_routine.hh)
struct atrous_options;               // its own options
class svgf_denoise_routine;          // the native temporal member (svgf_denoise_routine.hh)
struct svgf_options;                 // its own options
class oidn_denoise_routine;          // the OIDN trained member, run as our own shaders (oidn_denoise_routine.hh)
struct oidn_options;                 // its own options
enum class oidn_network_size : u8;   // which of OIDN's trained networks it runs
struct upscale_inputs;               // one upscale call's images
struct upscale_outcome;              // status + whether history restarted
class upscale_history;               // the caller-owned state of one upscaled stream
class fsr_upscale_routine;           // AMD FSR 3.1's upscaler, run through sg (fsr_upscale_routine.hh)
struct fsr_options;                  // its own options

class dlss_rr_routine;     // the NVIDIA Ray Reconstruction member (dlss_rr_routine.hh)
struct dlss_options;       // its own options
class nrd_denoise_routine; // the NRD split-signal member (nrd_denoise_routine.hh)
struct nrd_options;        // its own options

class mix_routine; // one image faded into another, in place (mix_routine.hh)

// Slug: shapes bounded by quadratic curves, covered on the GPU from their outlines (slug_shape.hh, libs/graphics/shaped-rendering/docs/slug.md).
struct slug_curve;               // one quadratic piece of an outline
enum class slug_fill_rule : u8;  // nonzero or even-odd
struct slug_outline;             // closed contours of curves, in the shape's own units
struct slug_contour;             // one contour of a path: where it ends, whether it is closed
struct slug_path;                // contours that may stay open, and shapes; what a stroke follows (slug_path.hh)
enum class stroke_join : u8;     // miter, round or bevel
enum class stroke_cap : u8;      // butt, round or square
struct stroke_style;             // width, join, cap, miter limit, dashes
struct slug_compiled_shape;      // an outline as Slug's curve and band tables, device-free
struct slug_shape_ref;           // where an atlas placed a shape
class slug_atlas;                // the caller-owned curve and band textures many shapes share
struct slug_instance;            // one shape to draw: which, where, what color
enum class slug_visibility : u8; // always, or by a frame's probe into a depth texture
struct slug_frame;               // where a job places a plane of shapes: a position, two axes, a tint, a probe
struct slug_quad;                // one quad of a job: an atlas record under a frame
struct slug_view;                // one draw's transform and knobs
struct slug_pipeline_key;        // the routine's parameter: color and depth format
class slug_routine;              // draws shape instances from an atlas
class slug_font;                 // a font face's glyphs compiled on demand into an atlas, and text set over them
enum class text_align : u8;      // left, center or right within a laid-out box
struct text_style;               // size, line height, wrap width, alignment
struct laid_out_glyph;           // one glyph and its baseline origin
struct text_layout;              // a string set in a face: positioned glyphs and their box

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
