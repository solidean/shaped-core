// double_compute.hlsl — minimal compute shader for the dx12 bind-path end-to-end test.
//
// Writes Output[i] = i * 2, seeded purely from the dispatch thread id (no input buffer needed), so
// the test only has to read the result back. `Output` binds to (set 0, index 0) = register(u0, space0)
// as a readwrite_structured_buffer.
//
// Compiled to DXIL and embedded as double_compute.dxil.h (compilation is not part of the build yet):
//   dxc -T cs_6_0 -E main -Fh double_compute.dxil.h -Vn double_compute_dxil double_compute.hlsl

RWStructuredBuffer<uint> Output : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    Output[id.x] = id.x * 2;
}
