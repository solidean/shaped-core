#pragma once

#include <clean-core/common/utility.hh> // cc::start_end
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/function_ref.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource_access_state.hh>

/// Subresource addressing and the covering-partition that tracks per-subresource access state for
/// textures. A texture's subresource domain is the discrete grid (mip level × array slice × aspect
/// plane). Buffers are single-subresource and never use any of this — it is **designed-in for textures**
/// and not exercised until `sg::texture` lands.
///
/// The partition is the improvement over the legacy tracker's flat per-subresource array: state is kept
/// as a set of range-boxes that always exactly tile the whole domain (the *covering invariant*). An
/// access to a specific range splits boxes so it aligns to box boundaries (keeping the tiling exact),
/// then touches only the covered boxes; boxes merge back to one when all their states are equivalent.

namespace sg
{
/// Which plane of a (possibly multi-planar) texture a subresource addresses. Single-plane formats use
/// `color` (or `depth`); depth-stencil and video formats expose several planes.
enum class texture_aspect : u32
{
    color,
    depth,
    stencil,
    plane0,
    plane1,
    plane2,
};

/// The size of a texture's subresource domain along each axis. `aspect_count` is the number of planes
/// (1 for a plain color texture, 2 for depth+stencil, etc.). A buffer is `{1, 1, 1}`.
struct subresource_extent
{
    int mip_count = 1;
    int array_count = 1;
    int aspect_count = 1;

    [[nodiscard]] int total() const { return mip_count * array_count * aspect_count; }
};

/// A half-open box in the subresource grid: a `[start, end)` range on each of the mip, array-slice, and
/// aspect-plane axes. Used to name the range an access covers.
struct subresource_range
{
    cc::start_end mip_range = {.start = 0, .end = 1};
    cc::start_end array_range = {.start = 0, .end = 1};
    cc::start_end aspect_range = {.start = 0, .end = 1};

    [[nodiscard]] static subresource_range whole(subresource_extent e)
    {
        return {.mip_range = {.start = 0, .end = e.mip_count},
                .array_range = {.start = 0, .end = e.array_count},
                .aspect_range = {.start = 0, .end = e.aspect_count}};
    }
    [[nodiscard]] bool is_empty() const
    {
        return mip_range.start >= mip_range.end || array_range.start >= array_range.end
            || aspect_range.start >= aspect_range.end;
    }
};

/// One tile of the covering partition: a range plus the access state that holds over it.
struct subresource_box
{
    subresource_range range;
    resource_access_state state;
};

/// A covering partition of a texture's subresource domain: a set of non-overlapping boxes that always
/// tile the whole `[0,mips)×[0,slices)×[0,planes)` grid. Starts as a single whole-domain box.
struct subresource_partition
{
    explicit subresource_partition(subresource_extent extent = {}) : _extent(extent)
    {
        _boxes.push_back(subresource_box{subresource_range::whole(extent), {}});
    }

    [[nodiscard]] subresource_extent extent() const { return _extent; }
    [[nodiscard]] cc::span<subresource_box const> boxes() const { return _boxes; }
    [[nodiscard]] isize box_count() const { return _boxes.size(); }

    /// Split boxes so `range` aligns to box boundaries, then invoke `fn` on each box's state that lies
    /// within `range`. Preserves the covering invariant. Use to declare an access over a subresource range.
    void for_each_in(subresource_range range, cc::function_ref<void(resource_access_state&)> fn)
    {
        if (range.is_empty())
            return;
        _split_mip(range.mip_range.start);
        _split_mip(range.mip_range.end);
        _split_array(range.array_range.start);
        _split_array(range.array_range.end);
        _split_aspect(range.aspect_range.start);
        _split_aspect(range.aspect_range.end);

        for (auto& box : _boxes)
            if (_contained(box.range, range))
                fn(box.state);
    }

    /// Collapse the partition back to a single whole-domain box iff every box's state is equivalent (same
    /// full-timeline state). No-op otherwise. Call after flushing so an all-uniform texture stops paying
    /// per-box cost.
    void try_merge()
    {
        if (_boxes.size() <= 1)
            return;
        for (isize i = 1; i < _boxes.size(); ++i)
            if (!_states_equal(_boxes[0].state, _boxes[i].state))
                return;
        auto const merged = _boxes[0].state;
        _boxes.clear();
        _boxes.push_back(subresource_box{subresource_range::whole(_extent), merged});
    }

private:
    static bool _contained(subresource_range const& box, subresource_range const& r)
    {
        return box.mip_range.start >= r.mip_range.start && box.mip_range.end <= r.mip_range.end
            && box.array_range.start >= r.array_range.start && box.array_range.end <= r.array_range.end
            && box.aspect_range.start >= r.aspect_range.start && box.aspect_range.end <= r.aspect_range.end;
    }

    // Guillotine-split every box straddling plane `m` on the mip axis, keeping a valid tiling.
    void _split_mip(isize m)
    {
        isize const n = _boxes.size();
        for (isize i = 0; i < n; ++i)
        {
            auto& box = _boxes[i];
            if (box.range.mip_range.start < m && m < box.range.mip_range.end)
            {
                subresource_box upper = box;
                box.range.mip_range.end = m;
                upper.range.mip_range.start = m;
                _boxes.push_back(upper);
            }
        }
    }
    void _split_array(isize a)
    {
        isize const n = _boxes.size();
        for (isize i = 0; i < n; ++i)
        {
            auto& box = _boxes[i];
            if (box.range.array_range.start < a && a < box.range.array_range.end)
            {
                subresource_box upper = box;
                box.range.array_range.end = a;
                upper.range.array_range.start = a;
                _boxes.push_back(upper);
            }
        }
    }
    void _split_aspect(isize p)
    {
        isize const n = _boxes.size();
        for (isize i = 0; i < n; ++i)
        {
            auto& box = _boxes[i];
            if (box.range.aspect_range.start < p && p < box.range.aspect_range.end)
            {
                subresource_box upper = box;
                box.range.aspect_range.end = p;
                upper.range.aspect_range.start = p;
                _boxes.push_back(upper);
            }
        }
    }

    // Whole-timeline equality — two boxes merge only when their states are byte-for-byte the same.
    static bool _states_equal(resource_access_state const& a, resource_access_state const& b)
    {
        return a.curr_read_stages == b.curr_read_stages && a.curr_read_access == b.curr_read_access
            && a.curr_write_stages == b.curr_write_stages && a.curr_write_access == b.curr_write_access
            && a.barriered_read_stages == b.barriered_read_stages && a.barriered_read_access == b.barriered_read_access
            && a.inflight_read_stages == b.inflight_read_stages && a.inflight_read_access == b.inflight_read_access
            && a.inflight_write_stages == b.inflight_write_stages && a.inflight_write_access == b.inflight_write_access
            && a.curr_layout == b.curr_layout && a.prev_layout == b.prev_layout;
    }

    subresource_extent _extent;
    cc::vector<subresource_box> _boxes;
};
} // namespace sg
