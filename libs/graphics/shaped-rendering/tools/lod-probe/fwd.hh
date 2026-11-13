#pragma once

#include <clean-core/fwd.hh>

namespace lod_probe
{
using namespace cc::primitive_defines;

struct tile;           // one footprint with a known level (see probe.hh)
enum class path : int; // where a sample's level comes from
struct pass;           // one draw: a sampler and a path over every tile
struct footprint;      // a tile's texel-space derivatives
struct pass_result;    // what one pass read, per tile
struct pass_summary;   // the hardware's level offset and blend width a pass shows
struct context_result; // every pass of one context

struct floor_view;    // one sampler setting rendered on the floor (see floor.hh)
struct floor_capture; // every floor view of one context
} // namespace lod_probe
