// A SECOND ray-tracing fixture library, whose only job is to come from a different file than raytrace.metal.
//
// Compiled by hand, and the result checked in as raytrace_second.metallib.h:
//
//   xcrun -sdk macosx metal -O2 -o raytrace_second.metallib raytrace_second.metal
//   xxd -i raytrace_second.metallib > raytrace_second.metallib.h
//
// sg::compiled_shader is single-entry, so a real shader pipeline hands the backend one blob per shader rather than one
// blob with several entry points — which is the shape this exists to test.
// A shader table built from two libraries is the realistic case and the one a single-blob fixture cannot distinguish:
// a backend that loaded one library and found every function in it would pass that test and fail this one.
//
// Hand-written and temporary for the same reason raytrace.metal is; see its comment.

#include <metal_stdlib>
using namespace metal;

/// The same shape as raytrace.metal's miss_marker, with a different value, so the number says which library ran.
[[visible]] void miss_from_second_library(thread float& payload) { payload = -2.0f; }
