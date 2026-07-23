#pragma once

/// Full umbrella include for shaped-rendering.
///
/// Pulls in the window and input API, and the concrete render routines as they are implemented.
/// The render-routine framework itself (sg::render_routine, ctx.routines) lives in shaped-graphics — include <shaped-graphics/render_routine.hh>.

#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/imgui_context.hh>
#include <shaped-rendering/imgui_routine.hh>
#include <shaped-rendering/input.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-rendering/window.hh>
