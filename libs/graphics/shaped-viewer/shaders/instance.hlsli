#pragma once

// One scene item as the GPU reads it.
//
// Its own file because two very different readers need it: the path tracer's shared binding group declares the
// table (pt_common.hlsli), and the material runtime is what decodes an entry (material_runtime.hlsli).
// The raygen must see this type to declare the buffer, and must not pull in the OpenPBR runtime to get it.

namespace sv
{
/// One scene item, as a closest-hit reads it by `InstanceID()` — mirrors sv::instance_gpu (resources/instance_data.hh).
///
/// Keep the two in lockstep: this is a byte layout, not a description of one.
struct instance
{
    uint param_buffer;
    uint param_offset;
    uint vertices;
    uint indices;
    uint is_indexed;
    uint3 _padding;
};
} // namespace sv
