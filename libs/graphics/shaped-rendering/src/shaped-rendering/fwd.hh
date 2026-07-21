#pragma once

#include <clean-core/fwd.hh>
#include <shaped-graphics/fwd.hh>

/// Aggregate forward declarations for shaped-rendering. Include when a forward decl is all you need.

namespace sr
{
// Vocabulary types (i32/u32/f32/isize/...) available bare inside sr, not leaked globally.
using namespace cc::primitive_defines;

// Concrete render routines land here as they are implemented; the routine framework itself lives in
// shaped-graphics (sg::render_routine / ctx.routines).

// OS windows (see window.hh).
// Always declared; SR_HAS_WINDOW says whether a backend was built in, and without one creation fails.
struct window_description;        // value type — input to create_window
struct window_system_description; // value type — input to window_system::create
class window;
class window_system;

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

// Dear ImGui, drawn through sg. imgui_context owns the ImGui context and the frame bracket;
// imgui_routine owns the GPU resources and records the draws.
class imgui_context;
class imgui_routine;

namespace impl
{
class imgui_texture_registry;
}
} // namespace sr
