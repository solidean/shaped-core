// A compute shader for the codegen test: it only has to be real enough to compile.
#include "util/common.hlsli"

RWStructuredBuffer<float> gOutput : register(u0);

[numthreads(SLIB_TEST_GROUP_SIZE, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    gOutput[tid.x] = slib_test_invert(gOutput[tid.x]);
}
