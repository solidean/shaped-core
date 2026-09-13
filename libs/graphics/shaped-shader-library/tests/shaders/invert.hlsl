// A compute shader for the codegen test: it only has to be real enough to compile.
#include "util/common.hlsli"

#pragma sc group 0
namespace invert_bindings
{
    RWStructuredBuffer<float> gOutput;
}

[numthreads(SLIB_TEST_GROUP_SIZE, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    invert_bindings::gOutput[tid.x] = slib_test_invert(invert_bindings::gOutput[tid.x]);
}
