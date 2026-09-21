#pragma once

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
} // namespace sg_test
