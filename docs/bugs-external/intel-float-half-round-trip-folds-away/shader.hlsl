// The host fills `input` with 256 floats, most of which no half holds, so no compiler sees a constant.
// A float converted to a half and back must come back as a value a half holds, however the conversion rounds.
StructuredBuffer<float> input : register(t0);
RWStructuredBuffer<float> output : register(u1);

[numthreads(64, 1, 1)]
void cs(uint3 thread : SV_DispatchThreadID)
{
    const uint i = thread.x;
    // the round trip as a pair of casts, which is how shaped-core's SGL writes `(x as half) as float`
    output[i] = float(float16_t(input[i]));
    // the same through the intrinsics that pack a half into the low bits of a uint
    output[256 + i] = f16tof32(f32tof16(input[i]));
}
