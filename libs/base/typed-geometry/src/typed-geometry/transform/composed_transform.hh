#pragma once

#include <typed-geometry/fwd.hh>

/// Two transforms held side by side, applied inner first, then outer.
///
/// This is what composition falls back to when the two cannot be fused into a single transform —
/// see tg::compose, which reaches for `outer.composed(inner)` first and only nests when that does not exist.
/// It therefore composes ANY two transforms, including ones that know nothing about each other.
///
///     auto const c = tg::compose(a, b);   // c(obj) == a(b(obj))
///
/// The cost of not fusing is that the parts are applied one after the other, so an object pays for both
/// steps and passes through whatever intermediate type the first one produces.
/// Where the transforms CAN fuse, a fused transform is strictly better and tg::compose gives you that instead.
///
/// It answers for every object through `custom_transform`, the first branch of every object's chain.
/// That member is public here on purpose: unlike a transform special-casing a particular object, this one
/// has an answer for anything its two parts have an answer for, and it is the same answer they would give.
template <class TransformOuter, class TransformInner>
struct tg::composed_transform
{
    /// applied second
    TransformOuter outer;
    /// applied first
    TransformInner inner;

    // construction
public:
    composed_transform() = default;

    explicit constexpr composed_transform(TransformOuter const& outer, TransformInner const& inner)
      : outer(outer), inner(inner)
    {
    }

    // application
public:
    /// the image of `obj`, by applying the parts in order.
    ///
    /// The parts decide everything, including the return type: an object may well change type at the inner
    /// step and change again at the outer one.
    template <class ObjT>
    [[nodiscard]] constexpr auto custom_transform(ObjT const& obj) const
    {
        return obj.transformed(inner).transformed(outer);
    }

    /// Applies the parts directly rather than routing back through `obj.transformed(*this)`.
    /// vec, pos and bivec delegate their transformed() straight to this, so routing back would not terminate.
    template <class ObjT>
    [[nodiscard]] constexpr auto transform(ObjT const& obj) const
    {
        return this->custom_transform(obj);
    }

    template <class ObjT>
    [[nodiscard]] constexpr auto operator()(ObjT const& obj) const
    {
        return this->custom_transform(obj);
    }

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(composed_transform const&, composed_transform const&) = default;
};
