// One mip of a float or normalized image set to a constant — FSR 3.1's clear job, for sr::fsr_upscale_routine.
// fsr_clear_uint.hlsl is the twin for the unsigned-integer images.
//
// Every address below is written by slib's binding pass; see shaped-shader-library/docs/binding-preprocessor.md.

struct fsr_clear_float_constants
{
    float4 value;
};

#pragma sc push_constants
ConstantBuffer<fsr_clear_float_constants> gConstants;

#pragma sc group 0
namespace fsr_clear_float_bindings
{
    RWTexture2D<float4> gTarget;
}

using namespace fsr_clear_float_bindings;

[numthreads(8, 8, 1)] void main_cs(uint3 id : SV_DispatchThreadID)
{
    uint2 size;
    gTarget.GetDimensions(size.x, size.y);
    if (id.x >= size.x || id.y >= size.y)
        return;

    gTarget[id.xy] = gConstants.value;
}
