#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

/// Intel's own Open Image Denoise filter, as the oracle the OIDN member is measured against.
///
/// The member runs Intel's trained weights in our own compute shaders, so nothing at render time touches the library.
/// It is linked into this test binary alone, and only where it was fetched on request (`uv run extern/oidn/fetch-oidn.py`).
/// Two implementations, chosen by the build: `oidn_reference.cc` over the library, `oidn_reference_null.cc` without it.
/// The seam keeps OIDN's headers out of every test file that asks the question.
namespace sr_test
{
/// Whether OIDN was compiled into this build at all.
[[nodiscard]] bool oidn_is_compiled_in();

/// The library's version, as `major.minor.patch`, or empty when it is not compiled in.
///
/// The filter names and the buffer contract move between major versions, and a mismatch between what a caller writes
/// and what the library expects is a wrong image rather than an error.
[[nodiscard]] cc::string oidn_version();

/// Runs OIDN's OWN filter on host memory, which is what our shaders are checked against.
///
/// Every choice that could silently differ — the transfer curve, the weight layout, the padding, the order of the
/// layers — shows up as a difference here and nowhere else.
///
/// Configured to match what the member does: HDR radiance with an albedo and a normal, an input scale of one, and
/// the quality whose weights are the ones we load.
/// `color`, `albedo` and `normal` are `extent`-sized and row-major; `out` is written the same way.
/// False when OIDN is not compiled in, or when it refused, which is logged.
[[nodiscard]] bool oidn_filter_reference(cc::span<tg::vec3f const> color,
                                         cc::span<tg::vec3f const> albedo,
                                         cc::span<tg::vec3f const> normal,
                                         tg::vec2i extent,
                                         cc::span<tg::vec3f> out);

/// Whether a CPU device can actually be created on this machine.
///
/// Separate from being compiled in, because the facade library loads its core and its device module by name out of
/// its own directory: a binary that was built against OIDN but staged without them links, runs, and fails here.
[[nodiscard]] bool oidn_has_device();
} // namespace sr_test
