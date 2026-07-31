#pragma once

// A rectangular area light the integrator samples for direct lighting, mirroring sv::area_light_gpu (light.hh) lane-for-lane.
// Each float3 sits in its own 16-byte cbuffer lane (the trailing pad scalar), matching the C++ std140-ish layout.
// The rect is already in world space: it spans [center +/- u] x [center +/- v], emitting `emission` radiance out
// of the face `normal` points along.
//
// Room to tighten this later, once the light block is worth 16 bytes: `normal` is redundant (= normalize(cross(u, v)),
// which the integrator can form on the fly), and the five pad scalars are free lanes the remaining fields could fold
// into — 80 bytes down to 64, or fewer if a scalar intensity splits out of `emission`.
// Any change here must land in sv::area_light_gpu in the same edit.
struct AreaLight
{
    float3 center;   float _l0;
    float3 u;        float _l1; // world half-extent spanning the rect's first axis
    float3 v;        float _l2; // world half-extent spanning the rect's second axis
    float3 emission; float _l3;
    float3 normal;   float _l4; // = normalize(cross(u, v)); the emitting face
};
