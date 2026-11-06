// Writes 7 to red, and each way this stage can take its input.
//   VARYING      reads the vertex stage's varying
//   VIEW_INPUT   declares SV_ViewID, and reads it unless VIEW_UNREAD, in a way the compiler cannot fold away

struct inputs
{
    float4 position : SV_Position;
#ifdef VARYING
    nointerpolation int tag : TAG;
#endif
};

#ifdef VIEW_INPUT
float4 main(inputs v, uint view : SV_ViewID) : SV_Target
#else
float4 main(inputs v) : SV_Target
#endif
{
    float red = 7.0;
#ifdef VARYING
    red = float(v.tag);
#endif
#if defined(VIEW_INPUT) && !defined(VIEW_UNREAD)
    red = view < 1000 ? red : 0.0;
#endif
    return float4(red, 0.0, 0.0, 1.0);
}
