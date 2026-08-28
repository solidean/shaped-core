#pragma once

/// Full umbrella include for shaped-rendering.
///
/// Pulls in the window and input API, and the concrete render routines as they are implemented.
/// The render-routine framework itself (sg::render_routine, ctx.routines) lives in shaped-graphics — include <shaped-graphics/routine/render_routine.hh>.

#include <shaped-rendering/blit_routine.hh>
#include <shaped-rendering/box_filter_mipmap_routine.hh>
#include <shaped-rendering/capture.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/gpu_types.hh>
#include <shaped-rendering/imgui_context.hh>
#include <shaped-rendering/imgui_routine.hh>
#include <shaped-rendering/imgui_style.hh>
#include <shaped-rendering/input.hh>
#include <shaped-rendering/keyed_pipeline_cache.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-rendering/window.hh>
