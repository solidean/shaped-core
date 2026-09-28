#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-shader-library/fwd.hh>

// The shader fixtures this test binary declares, and the one library that serves them.
//
// A slib::shader_library is a process-wide singleton — the generated package symbols it fills in are globals — so a
// test that stands up its own must exclude every other test that would.
// One library for the whole binary removes that constraint rather than scheduling around it, and it also removes the
// repeated work: the compilers are registered once and a shader compiles once, however many tests acquire it.
//
// The sources live beside this header: hlsl/ is the HLSL package (only built where a compiler for it exists) and
// sgl/ is the SGL one, which needs no external compiler to reach a WebGPU context.
// Which packages are in it is a build property, so a test asks the asset and reports what it gets.

namespace sg_test
{
/// The library every test in this binary acquires through, created on first use with every compiler this build has.
/// Never destroyed before the run ends, so a shader compiled for one test is still there for the next.
slib::shader_library& shader_fixtures();

/// Whether any registered compiler connects a fixture package's language to a format `ctx` accepts.
///
/// **A metal context reaches none of them today.** The edges are SGL to WGSL and, where DXC exists, to DXIL and
/// SPIR-V; nothing produces a metallib, so a metal context is offered a format it does not take.
/// A test that needs a shader asks this first and SKIPs, rather than failing on an acquire that cannot succeed —
/// which is what the whole shader-using half of the tier-1 sweep did on a Mac with a Metal 4 device.
///
/// Derived from the library rather than from a backend name, so the day an SGL to metallib edge is registered these
/// tests start running with nothing here to update.
/// libs/graphics/shaped-graphics/docs/TODO.md carries the missing edge.
[[nodiscard]] bool shaders_reach(sg::context const& ctx);
} // namespace sg_test
