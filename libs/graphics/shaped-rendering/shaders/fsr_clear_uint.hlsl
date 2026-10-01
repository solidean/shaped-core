// One mip of an unsigned-integer image set to a constant — FSR 3.1's clear job, for sr::fsr_upscale_routine.
//
// FSR clears these with a float, and AMD's own backend writes that float's BITS: its depth images hold `asuint(depth)`,
// so clearing to 1.0 has to store 0x3f800000 rather than 1.
//
// Every address below is written by slib's binding pass; see shaped-shader-library/docs/binding-preprocessor.md.

struct fsr_clear_uint_constants
{
    uint value;
    uint3 _pad;
};

#pragma sc push_constants
ConstantBuffer<fsr_clear_uint_constants> gConstants;

#pragma sc group 0
namespace fsr_clear_uint_bindings
{
    RWTexture2D<uint> gTarget;
}

using namespace fsr_clear_uint_bindings;

[numthreads(8, 8, 1)] void main_cs(uint3 id : SV_DispatchThreadID)
{
    uint2 size;
    gTarget.GetDimensions(size.x, size.y);
    if (id.x >= size.x || id.y >= size.y)
        return;

    gTarget[id.xy] = gConstants.value;
}
