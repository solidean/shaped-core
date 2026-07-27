#pragma once

#include <clean-core/common/hash.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-viewer/fwd.hh>

namespace sv
{
/// Stable identity of a view across frames.
///
/// A view is re-submitted every frame as a fresh value; its `view_id` is what ties this frame's view to
/// the *persistent* resources it accumulates into (temporal accumulators, history buffers). Everything a
/// view touches that is not keyed by a `view_id` is transient and recreated each frame.
///
/// Derive one from a human-readable string once and keep reusing it — `from_string` is a plain content
/// hash, so the same string always yields the same id.
struct view_id
{
    u64 value = 0;

    [[nodiscard]] static view_id from_string(cc::string_view name) { return {cc::make_hash_of_bytes(name.as_bytes())}; }

    [[nodiscard]] friend constexpr bool operator==(view_id, view_id) = default;

    // ADL hidden friend so a view_id keys a cc::map / cc::set directly.
    [[nodiscard]] friend u64 hash(view_id v) { return cc::make_hash(v.value); }
};
} // namespace sv
