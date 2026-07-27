#pragma once

#include <shaped-viewer/camera.hh>
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

namespace sv
{
/// The per-view constant block every flat-PBR ray-tracing shader reads at `b0` (the `FrameConstants` cbuffer in
/// shaders/common.hlsli). One upload per view per frame.
///
/// Just the camera basis: the flat path is lit entirely by the view's SH `background` (see background.hh) — the
/// miss reconstructs the environment radiance, the closest-hit shades from its irradiance. Keep this in lockstep
/// with common.hlsli.
struct frame_constants_gpu
{
    camera_gpu camera;

    // Pad the whole block to 256 bytes. A D3D12 constant-buffer view is sized in 256-byte multiples, so the
    // backing buffer must hold a full 256-byte range even though the declared fields above are smaller — and
    // this reserve is where the next per-frame constants (accumulation frame index, jitter, ...) will land.
    f32 _reserved[48] = {};
};

static_assert(sizeof(frame_constants_gpu) == 256, "frame_constants_gpu must be a full 256-byte CBV block");
} // namespace sv
