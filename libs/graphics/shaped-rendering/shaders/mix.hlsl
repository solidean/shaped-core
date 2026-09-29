// `destination = lerp(destination, source, weight)`, per pixel.
// Paired with sr::mix_routine.
//
// The destination is read AND written, which is what lets one extra image carry a crossfade rather than two: a
// per-pixel read-modify-write of a UAV needs no copy and no second target to land in.
// Alpha crosses with the colour, so fading between two images of the same thing leaves the alpha alone.
//
// Every address below is written by slib's binding pass; see shaped-shader-library/docs/binding-preprocessor.md.

struct mix_constants
{
    float weight; // 0 keeps the destination, 1 replaces it with the source
    float3 _pad;
};

#pragma sc push_constants
ConstantBuffer<mix_constants> gConstants;

#pragma sc group 0
namespace mix_bindings
{
    Texture2D<float4> gSource;
    RWTexture2D<float4> gDestination;
}

using namespace mix_bindings;

[numthreads(8, 8, 1)] void main_cs(uint3 id : SV_DispatchThreadID)
{
    uint2 size;
    gDestination.GetDimensions(size.x, size.y);
    if (id.x >= size.x || id.y >= size.y)
        return;

    int2 p = int2(id.xy);
    gDestination[id.xy] = lerp(gDestination[id.xy], gSource.Load(int3(p, 0)), gConstants.weight);
}
