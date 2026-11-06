// READS_VIEW reads SV_ViewID here too, in a way the compiler cannot fold away.

struct varyings
{
    float4 position : SV_Position;
    nointerpolation int tag : TAG;
};

#ifdef READS_VIEW
float4 main(varyings v, uint view : SV_ViewID) : SV_Target
{
    return float4(view < 1000 ? float(v.tag) : 0.0, float(view), 0.0, 1.0);
}
#else
float4 main(varyings v) : SV_Target
{
    return float4(float(v.tag), 0.0, 0.0, 1.0);
}
#endif
