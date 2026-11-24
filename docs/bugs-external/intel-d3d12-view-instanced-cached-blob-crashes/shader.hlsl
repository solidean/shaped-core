// A quad from the vertex index and a constant color: the pipeline's shaders play no part in the trigger.

float4 main_vs(uint id : SV_VertexID) : SV_Position
{
    return float4(float(id & 1) * 2 - 1, float(id >> 1) * 2 - 1, 0, 1);
}

float4 main_ps() : SV_Target
{
    return float4(1, 1, 1, 1);
}
