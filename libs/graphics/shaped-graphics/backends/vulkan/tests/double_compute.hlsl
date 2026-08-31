// double_compute.hlsl — minimal compute shader for the vulkan bind-path end-to-end test.
//
// Writes Output[i] = i * 2, seeded purely from the dispatch thread id (no input buffer needed), so
// the test only has to read the result back.
//
// The `[[vk::binding]]` annotation is the difference from dx12's otherwise identical fixture, and it is the whole
// SPIR-V binding story: a Vulkan target states its (binding, set) in the source rather than having DXC derive one
// from the register, because a descriptor set has one binding namespace where HLSL has three.
// Here that is (set 0, index 0), which is what the test's hand-written sg::binding declares.
//
// Compiled to SPIR-V and embedded as double_compute.spirv.h (compilation is not part of the build yet):
//   dxc -T cs_6_0 -E main -spirv -fspv-target-env=vulkan1.3 \
//       -Fh double_compute.spirv.h -Vn double_compute_spirv double_compute.hlsl

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> Output : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    Output[id.x] = id.x * 2;
}
