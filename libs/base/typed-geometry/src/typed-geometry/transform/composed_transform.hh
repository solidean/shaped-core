#pragma once

namespace tg
{
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
struct composed_transform
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

    template <class ObjT>
    [[nodiscard]] constexpr auto transform(ObjT const& obj) const
    {
        return obj.transformed(*this);
    }

    template <class ObjT>
    [[nodiscard]] constexpr auto operator()(ObjT const& obj) const
    {
        return obj.transformed(*this);
    }

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(composed_transform const&, composed_transform const&) = default;
};

} // namespace tg
