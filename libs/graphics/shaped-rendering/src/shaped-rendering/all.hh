#pragma once

/// Full umbrella include for shaped-rendering.
///
/// Pulls in the window API when one was built in (SR_HAS_WINDOW), and the concrete render routines as they
/// are implemented.
/// The render-routine framework itself (sg::render_routine, ctx.routines) lives in shaped-graphics — include
/// <shaped-graphics/render_routine.hh>.

#include <shaped-rendering/fwd.hh>

#if SR_HAS_WINDOW
#include <shaped-rendering/input.hh>
#include <shaped-rendering/window.hh>
#endif
