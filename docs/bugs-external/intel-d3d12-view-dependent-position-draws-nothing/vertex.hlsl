// A strip covering the target from the vertex index alone.
// With MOVED it is squeezed left of centre in view 0 and right of it in view 1, so the position depends on SV_ViewID.

struct varyings
{
    float4 position : SV_Position;
    nointerpolation int tag : TAG;
};

#ifdef NO_VIEW
varyings main(uint vertex : SV_VertexID)
#else
varyings main(uint vertex : SV_VertexID, uint view : SV_ViewID)
#endif
{
    float x = (vertex & 1) != 0 ? 1.0 : -1.0;
    float y = (vertex & 2) != 0 ? 1.0 : -1.0;
#ifdef MOVED
    x = x * 0.5 + (view == 0 ? -0.5 : 0.5);
#else
    x = x * 0.5;
#endif
    varyings result;
    result.position = float4(x, y, 0.5, 1.0);
    result.tag = 7;
    return result;
}
