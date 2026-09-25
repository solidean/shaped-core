// Shader fixture for fragment-write-hazard-test.cc: two fragment stages over one read-write buffer, where the second
// draw must see what the first one wrote.
//
// **The dependency is fragment-stage to fragment-stage inside one pass**, which is the case a backend has to order
// without the caller naming anything. It is also the case Metal cannot express as a barrier at all, since a render
// encoder refuses the fragment stage as a barrier source -- so sg's rule is what the three backends are held to here.
//
// The target is k_size square and every fragment owns the slot its own pixel maps to, so a draw that raced ahead reads
// a zero rather than another pixel's value.
#pragma sc group 0
namespace hazard_bindings
{
    RWStructuredBuffer<uint> gResults;
}

static const uint k_size = 4;

struct vs_out
{
    float4 position : SV_Position;
};

// A full-screen triangle from the vertex id alone, so neither draw needs a vertex buffer.
vs_out vs_main(uint vid : SV_VertexID)
{
    const float2 positions[3] = {float2(-1.0, 3.0), float2(-1.0, -1.0), float2(3.0, -1.0)};

    vs_out output;
    output.position = float4(positions[vid], 0.0, 1.0);
    return output;
}

// Draw A: writes a known pattern, one value per pixel.
float4 ps_write(vs_out input) : SV_Target
{
    uint index = uint(input.position.y) * k_size + uint(input.position.x);
    hazard_bindings::gResults[index] = index + 1;
    return float4(0.0, 1.0, 0.0, 1.0);
}

// Draw B: copies what A wrote into the render target, where a read-back compares it against the pattern.
// A value that never arrived reads as 0 and lands as black, which no correct run produces.
float4 ps_read(vs_out input) : SV_Target
{
    uint index = uint(input.position.y) * k_size + uint(input.position.x);
    float v = float(hazard_bindings::gResults[index]) / 255.0;
    return float4(v, v, v, 1.0);
}
