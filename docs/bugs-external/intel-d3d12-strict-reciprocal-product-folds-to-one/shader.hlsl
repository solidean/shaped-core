// values[0] is +0.0 and values[1] is +infinity, written by the host so no compiler sees a constant.
// IEEE makes every product below a NaN.
RWStructuredBuffer<float> values : register(u0);

[numthreads(1, 1, 1)]
void cs()
{
    const float zero = values[0];
    const float inf = values[1];
    values[2] = zero * inf;
    values[3] = inf * zero;
    // the infinity computed in the shader, as the SGL probe that found this does
    values[4] = (1.0 / zero) * zero;
}
