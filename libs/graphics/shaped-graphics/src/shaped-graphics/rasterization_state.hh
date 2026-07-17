#pragma once

#include <shaped-graphics/fwd.hh>

/// Fixed-function rasterizer state baked into a raster pipeline: how primitives are filled, which
/// faces are culled, winding, depth clipping, and a static depth bias. Backend-neutral; each field's
/// trailing comment maps to the per-backend equivalent.

namespace sg
{
/// How a triangle is filled.
enum class fill_mode
{
    solid,     // DX12 FILL_MODE_SOLID     / Vk POLYGON_MODE_FILL
    wireframe, // DX12 FILL_MODE_WIREFRAME / Vk POLYGON_MODE_LINE
};

/// Which triangle faces are discarded before rasterization.
enum class cull_mode
{
    none,  // DX12 CULL_MODE_NONE  / Vk CULL_MODE_NONE
    front, // DX12 CULL_MODE_FRONT / Vk CULL_MODE_FRONT_BIT
    back,  // DX12 CULL_MODE_BACK  / Vk CULL_MODE_BACK_BIT
};

/// The winding order that designates a triangle's front face.
enum class front_face
{
    counter_clockwise, // DX12 FrontCounterClockwise = TRUE  / Vk FRONT_FACE_COUNTER_CLOCKWISE
    clockwise,         // DX12 FrontCounterClockwise = FALSE / Vk FRONT_FACE_CLOCKWISE
};

/// The rasterizer configuration of a raster pipeline. Defaults describe the common case: solid
/// back-face culling, counter-clockwise front faces, depth clipping on, no depth bias.
struct rasterization_state
{
    fill_mode fill = fill_mode::solid;
    cull_mode cull = cull_mode::back;
    front_face front = front_face::counter_clockwise;

    /// Clip primitives to the near/far planes (DX12 DepthClipEnable; inverts to Vulkan depthClampEnable).
    bool depth_clip_enabled = true;

    // Static depth bias baked into the pipeline (a dynamic per-draw bias is a future addition).
    float depth_bias = 0.0f;       ///< constant bias (DX12 DepthBias, as a float; Vk depthBiasConstantFactor)
    float depth_bias_slope = 0.0f; ///< slope-scaled bias (DX12 SlopeScaledDepthBias / Vk depthBiasSlopeFactor)
    float depth_bias_clamp = 0.0f; ///< maximum bias magnitude (DX12 DepthBiasClamp / Vk depthBiasClamp)
};
} // namespace sg
