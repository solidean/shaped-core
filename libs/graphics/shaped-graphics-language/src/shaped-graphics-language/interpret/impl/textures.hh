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

/// Runs `c` on `texture`, the one its first argument names, with `sampler` where it takes one (EVAL-120 to EVAL-126).
/// `args` are the call's arguments in order, the texture's and the sampler's empty; the result's scalars go to `out`.
/// An image store writes `texture`, and sets `stored`.
[[nodiscard]] texture_failure run_texture_call(builtins::texture::call const& c,
                                               texture_contents& texture,
                                               sampler_state const* sampler,
                                               cc::span<value const> args,
                                               cc::vector<scalar>& out,
                                               bool& stored);
} // namespace sgl::check::impl
