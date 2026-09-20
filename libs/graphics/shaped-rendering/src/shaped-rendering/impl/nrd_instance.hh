#pragma once

#include <clean-core/fwd.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-rendering/fwd.hh>

/// The NRD seam: what `sr::nrd_denoise_routine` needs from NVIDIA's denoiser library, with none of its headers.
///
/// Two implementations, chosen by the build: `nrd_instance.cc` where the sources were fetched, `nrd_null.cc`
/// otherwise — the same shape the DLSS seam takes, and for the same reason.
///
/// **NRD is not a vendor runtime.**
/// It compiles no shaders of its own at run time, owns no device memory and records nothing: it is a library that
/// answers "given these settings, which compute dispatches would denoise this frame, against which resources, with
/// which constants".
/// Everything it asks for is then created and executed through sg, which is why this member needs no native scope and
/// runs on any adapter — WARP included.
namespace sr::impl
{
/// Whether NRD was compiled into this build at all.
///
/// It has no device requirement of its own: its dispatches are ordinary compute, so a `true` here means the member can
/// run wherever sg can.
[[nodiscard]] bool nrd_is_compiled_in();

/// The library's version, as `major.minor.build`, or empty when it is not compiled in.
/// Worth logging once: NRD's resource contract and its settings move between versions, and a mismatch between what a
/// caller packs and what the library expects is a wrong image rather than an error.
[[nodiscard]] cc::string nrd_version();
} // namespace sr::impl
