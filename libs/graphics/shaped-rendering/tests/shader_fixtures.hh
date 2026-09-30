#pragma once

#include <shaped-rendering/denoise.hh>
#include <shaped-shader-library/fwd.hh>

// The one shader library sr's GPU tests acquire through.
//
// A slib::shader_library is a process-wide singleton — the generated package symbols it fills in are globals — so a
// test that stands up its own must exclude every other test that would.
// One library for the whole binary removes that constraint rather than scheduling around it, and it also removes the
// repeated work: sr's shaders compile once for the run instead of once per test.
//
// Compiled wherever a backend exists: which compiler edges the build has decides what a package acquires as, not
// whether this fixture is here.

namespace sr_test
{
/// sr's shader packages, with every compiler this build has, created on first use.
/// Never destroyed before the run ends, so a shader compiled for one test is still there for the next.
slib::shader_library& shader_fixtures();

/// Every guide any member can ask for.
///
/// What a resolution test passes when it means to vary DEVICE support alone: `sr::resolve_denoise_method` skips a
/// member whose required guides the caller cannot supply, so a narrower set here would silently test both at once.
/// A test about the guides names its own set instead.
[[nodiscard]] inline sr::denoise_guide_set every_guide()
{
    using g = sr::denoise_guide;
    return g::albedo | g::specular_albedo | g::normal | g::roughness | g::depth | g::motion | g::hit_distance
         | g::split_diffuse_specular;
}
} // namespace sr_test
