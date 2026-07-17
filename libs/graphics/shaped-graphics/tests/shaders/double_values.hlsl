// Shader package fixture for shaped-graphics-test. Its point is not what it computes but where it is
// declared: in a *consumer* of sg, proving a downstream target can own its shaders while sg itself
// stays independent of the shader library.
RWStructuredBuffer<float> gValues : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    gValues[tid.x] = gValues[tid.x] * 2.0f;
}
