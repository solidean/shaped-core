// One annotated binding group, for the generated-struct half of the codegen test.
//
// Registered on its own as `frame_bindings.hlsli:binding:frame_bindings`, not reached through the shader
// that includes it: a binding entry generates from the named file alone, or every shader including this
// header would generate the same struct again.
#pragma once

#pragma sc group 0
namespace frame_bindings
{
    Texture2D<float4> albedo;
#pragma sc static address=clamp_edge
    SamplerState linear_sampler;
    RWStructuredBuffer<float> histogram;
}
