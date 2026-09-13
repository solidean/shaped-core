#pragma once

#include <clean-core/common/hash.hh>
#include <shaped-graphics/fwd.hh>

#include <concepts>
#include <type_traits>

/// What identifies a routine beyond its type.
///
/// Some routines cannot exist as a single thing: "generate mipmaps" is one unit of GPU work per texture shape, and
/// "blit" one per target format, because each needs its own compiled entry point and its own pipeline.
/// Such a routine is a *schema*, and the unit a caller wants is the schema applied to a concrete argument — so the
/// argument moves out to the acquire call, and the registry holds one instance per distinct value.
///
/// The parameter is a **runtime value**, not a template argument.
/// A format is read off a texture, a swapchain or a plan, so a compile-time parameter would force every caller to
/// switch over an enumeration to pick the instantiation — which is the thing routines exist to avoid.
/// The template names the parameter's TYPE; the value is runtime.
///
/// **A parameter must be drawn from a small, enumerable set.**
/// One instance per distinct value is held until the routine is evicted or the context dies, so a routine parametrized
/// on a texture *size*, or on anything a scene supplies, is an unbounded cache that looks like a design.
/// That is documented rather than enforced: a cap that fires is a routine that silently stops working, which is worse
/// than a leak that shows up in a memory graph.

/// The parameter of an UNPARAMETRIZED routine — one instance per context, which is what every routine was before.
/// Empty, hashable and comparable, so it needs no special case anywhere the general one is handled.
struct sg::routine_no_params
{
    [[nodiscard]] bool operator==(routine_no_params const&) const = default;
};

namespace sg
{

/// What a routine parameter must satisfy.
/// Equality is what makes a hash collision detectable rather than silently served.
template <class P>
concept routine_params = std::equality_comparable<P> && std::is_copy_constructible_v<P>;

namespace impl
{
/// A parameter's hash, without making every parameter enum grow a free function.
///
/// An empty parameter is the unparametrized case and hashes to nothing.
/// An enum hashes through its underlying value, which is what `sg::pixel_format` and friends need.
/// Anything else must supply an ADL `hash(p)`, which is `cc::map`'s requirement anyway.
template <routine_params P>
[[nodiscard]] u64 routine_params_hash(P const& params)
{
    if constexpr (std::is_empty_v<P>)
    {
        (void)params;
        return 0;
    }
    else if constexpr (std::is_enum_v<P>)
    {
        return cc::make_hash(u64(params));
    }
    else
    {
        return u64(hash(params));
    }
}
} // namespace impl
} // namespace sg
