#pragma once

#include <shaped-graphics-language/builtins/texture_calls.hh>
#include <shaped-graphics-language/interpret/interpret.hh>

namespace sgl::check::impl
{
/// What a texture call ran into: nothing, a coordinate or a texture that has no texel (EVAL-121), or a malformed call.
struct texture_failure
{
    run_status status = run_status::ok;
    cc::string detail;

    [[nodiscard]] bool is_ok() const { return status == run_status::ok; }
};

/// The derivatives of a sample's coordinate across its quad, which a sample that picks its own level reads (EVAL-144).
struct coordinate_gradients
{
    cc::vector<scalar> x;
    cc::vector<scalar> y;
};

/// Runs `c` on `texture`, the one its first argument names, with `sampler` where it takes one (EVAL-120 to EVAL-126).
/// `args` are the call's arguments in order, the texture's and the sampler's empty; the result's scalars go to `out`.
/// An image store writes `texture`, and sets `stored`.
/// `gradients` are the quad's, for a sample that takes its level from them; null in a run of one lane, whose are 0.
[[nodiscard]] texture_failure run_texture_call(builtins::texture::call const& c,
                                               texture_contents& texture,
                                               sampler_state const* sampler,
                                               cc::span<value const> args,
                                               cc::vector<scalar>& out,
                                               bool& stored,
                                               coordinate_gradients const* gradients = nullptr);

/// The first scalar of level 0's texel at `coord` and `layer`, which a texel atomic updates (EVAL-100).
/// A coordinate or a layer outside the texture has no value (EVAL-121).
[[nodiscard]] texture_failure texel_offset(texture_contents const& texture,
                                           builtins::texture::shape_axes axes,
                                           cc::span<scalar const> coord,
                                           i32 layer,
                                           isize& offset);

/// EVAL-127: writes the texel a `test_texel` call states, growing `texture` to hold it where `may_grow`.
/// A texture the driver bound may not grow, so a texel outside it has no value.
[[nodiscard]] texture_failure state_texel(builtins::texture::call const& c,
                                          texture_contents& texture,
                                          bool may_grow,
                                          cc::span<value const> args);
} // namespace sgl::check::impl
