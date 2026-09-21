#pragma once

#include <shaped-shader-library/fwd.hh>

// The one shader library sr's GPU tests acquire through.
//
// A slib::shader_library is a process-wide singleton — the generated package symbols it fills in are globals — so a
// test that stands up its own must exclude every other test that would.
// One library for the whole binary removes that constraint rather than scheduling around it, and it also removes the
// repeated work: sr's shaders compile once for the run instead of once per test.
//
// Only compiled where the GPU tests are (WIN32 + DXC), since a build without a compiler has nothing to register.

namespace sr_test
{
/// sr's own shader package, with every compiler this build has, created on first use.
/// Never destroyed before the run ends, so a shader compiled for one test is still there for the next.
slib::shader_library& shader_fixtures();
} // namespace sr_test
