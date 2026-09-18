// The tier-2 compute fixture, kept as source next to the blob compiled from it.
//
// Compiled by hand, and the result checked in as double_compute.metallib.h:
//
//   xcrun -sdk macosx metal -O2 -o double_compute.metallib double_compute.metal
//   xxd -i double_compute.metallib > double_compute.metallib.h
//
// That keeps the tier-2 binary free of the shader library and the compiler toolchain, and makes the fixture
// reproducible by hand — the same reason dx12 checks in double_compute.dxil.h and vulkan double_compute.spirv.h.
//
// The binding shape is written by hand in the test beside it, so the test states what it means rather than inheriting
// whatever a reflector produced.
//
// One argument buffer per binding group, member [[id(n)]] = sg::binding::index, bound at buffer index = group index.
// That is the layout sg's metal backend encodes and the one SPIRV-Cross emits for a descriptor set.

#include <metal_stdlib>
using namespace metal;

struct frame_bindings
{
    device uint* values [[id(0)]];
};

kernel void main0(device frame_bindings& bindings [[buffer(0)]], uint tid [[thread_position_in_grid]])
{
    bindings.values[tid] = bindings.values[tid] * 2u;
}
