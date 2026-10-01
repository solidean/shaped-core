// Stand-in for a header AMD's FSR 3.1 host code includes from an `amdinternal/` folder the public SDK does not carry.
// Upstream it stamps build information into the upscaled image when the MLSR-WATERMARK environment variable is set.
// This one draws nothing, whatever the environment says.
#pragma once

#include <cstdint>

struct FfxInterface;
struct FfxResourceInternal;

struct FfxWatermark
{
    FfxWatermark(FfxInterface* /*backend*/, uint32_t /*effect_context_id*/) {}

    void Dispatch(FfxResourceInternal const& /*target*/, char const* /*message*/) {}
};
