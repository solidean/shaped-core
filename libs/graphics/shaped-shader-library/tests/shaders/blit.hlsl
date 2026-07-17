// Two entry points in one file, so the package exposes blit.vertex.main_vs and blit.fragment.main_ps.
float4 main_vs(uint id : SV_VertexID) : SV_Position
{
    return float4(float(id & 1u) * 4.0f - 1.0f, float(id >> 1u) * 4.0f - 1.0f, 0.0f, 1.0f);
}

float4 main_ps() : SV_Target
{
    return float4(1.0f, 0.0f, 1.0f, 1.0f);
}
