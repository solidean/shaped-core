#include <metal_stdlib>
using namespace metal;

// One argument buffer with one readwrite member, the shape ssc::msl reflects as one binding.
struct doubled_bindings
{
    device uint* values [[id(0)]];
};

#pragma sc numthreads 1 1 1
kernel void main_cs(device doubled_bindings& bindings [[buffer(0)]], uint tid [[thread_position_in_grid]])
{
    bindings.values[tid] = bindings.values[tid] * 2u;
}
