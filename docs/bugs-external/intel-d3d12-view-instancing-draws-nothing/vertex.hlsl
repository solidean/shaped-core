// A strip from the vertex index alone, and each way an output can depend on the view.
//   POSITION_BY_VIEW   moves the strip left of centre in view 0 and right of it in view 1
//   VARYING            adds a varying, constant unless one of the next two says otherwise
//   VARYING_BY_VIEW    makes it depend on the view
//   VARYING_BY_VERTEX  makes it depend on the vertex

struct varyings
{
    float4 position : SV_Position;
#ifdef VARYING
    nointerpolation int tag : TAG;
#endif
};

varyings main(uint vertex : SV_VertexID, uint view : SV_ViewID)
{
    float x = (vertex & 1) != 0 ? 1.0 : -1.0;
    float y = (vertex & 2) != 0 ? 1.0 : -1.0;
#ifdef POSITION_BY_VIEW
    x = x * 0.5 + (view == 0 ? -0.5 : 0.5);
#else
    x = x * 0.5;
#endif
    varyings result;
    result.position = float4(x, y, 0.5, 1.0);
#if defined(VARYING_BY_VERTEX)
    result.tag = 7 + (vertex > 1000 ? 1 : 0);
#elif defined(VARYING_BY_VIEW)
    result.tag = 7 + (view > 1000 ? 1 : 0);
#elif defined(VARYING)
    result.tag = 7;
#endif
    return result;
}
