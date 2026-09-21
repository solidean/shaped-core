#pragma once

#include <clean-core/common/hash.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-viewer/fwd.hh>

/// Stable identity of an authored thing across frames — `view_id` and `light_id` are the two instances.
///
/// Everything in a frame is re-submitted every frame as a fresh value.
/// A stable id is what ties this frame's value to the *persistent* state kept for it (temporal accumulators, history
/// buffers, a view's camera).
/// Anything not keyed by one is transient and recreated each frame.
///
/// `Tag` only keeps the kinds apart, so a view's id cannot key a light's state.
///
/// Derive one from a human-readable string once and keep reusing it — `from_string` is a plain content hash, so the same
/// string always yields the same id.
/// The optional `seed` is what the frame's id stack pushes, so the same name under two scopes names two things
/// (`frame::push_id`).
///
/// The whole string is hashed, `##` included — that is what makes "angle##0" and "angle##1" two ids sharing one display
/// name (`display_name_of`), the way Dear ImGui spells the same idea.
template <class Tag>
struct sv::stable_id
{
    u64 value = 0;

    [[nodiscard]] static stable_id from_string(cc::string_view name, u64 seed = 0)
    {
        return {cc::make_hash_of_bytes(name.as_bytes(), seed)};
    }

    [[nodiscard]] friend constexpr bool operator==(stable_id, stable_id) = default;

    // ADL hidden friend so an id keys a cc::map / cc::set directly.
    [[nodiscard]] friend u64 hash(stable_id v) { return cc::make_hash(v.value); }
};

namespace sv
{
/// The part of an id a human reads: everything before the first `##`, or the whole string when it holds none.
/// An id that is nothing but a suffix ("##7") has no display name at all, which is the point of writing it that way.
[[nodiscard]] constexpr cc::string_view display_name_of(cc::string_view id)
{
    auto const marker = id.find(cc::string_view("##"));
    return marker < 0 ? id : id.subview({.start = 0, .end = marker});
}

/// Folds `name` into `seed`, giving the seed an inner scope derives its ids from.
/// The same fold `from_string` applies, so `from_string(n, push_id_seed(s, outer))` and pushing then naming agree.
[[nodiscard]] inline u64 push_id_seed(u64 seed, cc::string_view name)
{
    return cc::make_hash_of_bytes(name.as_bytes(), seed);
}

/// Folds a loop index into `seed` — the answer to N things built in a loop under one name.
[[nodiscard]] constexpr u64 push_id_seed(u64 seed, i64 n)
{
    return cc::combine_hash(seed, cc::make_hash(n));
}
} // namespace sv
