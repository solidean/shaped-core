// Shader package fixture for shaped-graphics-test. Its point is not what it computes but where it is
// declared: in a *consumer* of sg, proving a downstream target can own its shaders while sg itself
// stays independent of the shader library.
#pragma sc group 0
namespace double_values_bindings
{
    RWStructuredBuffer<float> gValues;
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    double_values_bindings::gValues[tid.x] = double_values_bindings::gValues[tid.x] * 2.0f;
}
