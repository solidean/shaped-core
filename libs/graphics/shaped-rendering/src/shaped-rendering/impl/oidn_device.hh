#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

/// The OIDN seam: what `sr::oidn_denoise_routine` needs from Intel Open Image Denoise, with none of its headers.
///
/// Two implementations, chosen by the build: `oidn_device.cc` where the release was fetched, `oidn_null.cc`
/// otherwise — the same shape the DLSS and NRD seams take, and for the same reason.
///
/// **OIDN is a CPU denoiser here, and that is a decision rather than a limitation of the library.**
/// It has GPU devices of its own, on CUDA, HIP, SYCL and Metal, which share memory with a renderer through an OS
/// handle — and sg has no exportable memory or shared fence to hand one.
/// So the member downloads the image, filters it on the CPU and uploads the result, which is also why it is the one
/// non-native member that needs no particular vendor's hardware.
///
/// That round trip is what makes it ASYNCHRONOUS in a way no other member is: a filtered image is ready some frames
/// after the frame it came from, so the member reports `pending` until one exists.
namespace sr::impl
{
/// Whether OIDN was compiled into this build at all.
[[nodiscard]] bool oidn_is_compiled_in();

/// The library's version, as `major.minor.patch`, or empty when it is not compiled in.
///
/// Worth logging once: the filter names and the buffer contract move between major versions, and a mismatch between
/// what a caller writes and what the library expects is a wrong image rather than an error.
[[nodiscard]] cc::string oidn_version();

/// Runs OIDN's OWN filter on host memory, which is what our shaders are checked against.
///
/// Not a render path and never called by one: the member runs the network itself, and this exists so a test can ask
/// whether it computes what Intel's implementation computes.
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
} // namespace sr::impl
