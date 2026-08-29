#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/container/small_vector.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/barrier/command_list_slot.hh>
#include <shaped-graphics/barrier/subresource_state.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/texture_descriptions.hh>

/// Per-texture, per-command-list layout and access tracking.
///
/// Unlike the buffer tracker beside it, this is not a divergence from dx12 — it is the *same* covering-partition and
/// slot model, because textures already needed between-lists state on both APIs.
/// A layout is physical: whatever a list leaves a subresource in is what the next list finds, so it has to be tracked
/// across lists on any backend.
///
/// Each open command list keys its own covering partition by its `command_list_slot`, seeded on first touch from
/// `canonical`, the between-lists state.
/// The finalize that drops the active-slot count to zero — the *last* list using the texture — promotes its partition
/// into canonical, which is the only case that may leave the texture in a new layout.
/// Every earlier finalize reverts the texture to the canonical layout for the lists still using it.
///
/// Pure logic with no Vulkan objects, so it is unit-testable without a device.

namespace sg::backend::vulkan
{
/// The extent a texture's subresource domain spans: mips x array layers (cube faces included) x aspects.
[[nodiscard]] inline sg::subresource_extent subresource_extent_of(sg::texture_description const& d)
{
    int const layers = d.array_layers.value_or(1) * (d.is_cube ? 6 : 1);
    return sg::subresource_extent{
        .mip_count = d.mip_levels,
        .array_count = layers,
        .aspect_count = sg::format_aspect_count(d.format),
    };
}
} // namespace sg::backend::vulkan

/// One barrier, scoped to the subresource range it applies to.
struct sg::backend::vulkan::vulkan_subresource_barrier
{
    sg::subresource_range range;
    sg::access_barrier barrier;
};

/// How two layouts required for one subresource within one op combined.
enum class sg::backend::vulkan::layout_combine
{
    ok,       ///< a single layout serves both, or they were equal
    degraded, ///< no specialized layout serves both, so GENERAL is used — correct, and slower to sample from
    conflict, ///< the two accesses cannot coexist in one op — caller error
};

struct sg::backend::vulkan::combined_layout
{
    sg::texture_layout layout;
    layout_combine result;
};

namespace sg::backend::vulkan
{
/// Combine two layouts one subresource is required to be in within a single operation — a texture bound as more than
/// one view.
///
/// The only mismatch a binding group can legitimately produce is a texture bound as both a sampled and a storage view.
/// Vulkan has no layout serving both, so it falls back to GENERAL and reports `degraded`: correct, but sampling from
/// GENERAL forgoes whatever optimization SHADER_READ_ONLY_OPTIMAL buys on the device.
/// `general` already serves any access, so combining with it is free.
/// Anything else — a copy, render-target or depth layout mixed with a different one — is a real hazard.
[[nodiscard]] inline combined_layout combine_layouts(sg::texture_layout a, sg::texture_layout b)
{
    if (a == b)
        return {a, layout_combine::ok};

    auto const is_shader = [](sg::texture_layout l)
    { return l == sg::texture_layout::shader_readonly || l == sg::texture_layout::shader_readwrite; };
    if (is_shader(a) && is_shader(b))
        return {sg::texture_layout::general, layout_combine::degraded};

    if (a == sg::texture_layout::general || b == sg::texture_layout::general)
        return {sg::texture_layout::general, layout_combine::ok};

    return {sg::texture_layout::general, layout_combine::conflict};
}
} // namespace sg::backend::vulkan

class sg::backend::vulkan::vulkan_texture_access
{
public:
    explicit vulkan_texture_access(sg::subresource_extent extent) : _canonical(extent)
    {
        // Seed canonical as `undefined`, which is where vkCreateImage actually leaves an image — initialLayout may
        // only be UNDEFINED or PREINITIALIZED, and vulkan_texture uses the former.
        //
        // **This differs from dx12 and the difference is load-bearing.**
        // A D3D12 resource is created in COMMON, which is sg's `general`, so dx12 can take the default state as-is.
        // Doing the same here would have the first barrier declare oldLayout = GENERAL for an image that is really
        // UNDEFINED, which Vulkan rejects: an old layout must either match the current one or be UNDEFINED.
        //
        // Starting undefined is also what makes the first transition a discard rather than a preserve, which is
        // correct — there are no contents to keep.
        _canonical.for_each_in(sg::subresource_range::whole(extent),
                               [](sg::resource_access_state& state)
                               {
                                   state.curr_layout = sg::texture_layout::undefined;
                                   state.prev_layout = sg::texture_layout::undefined;
                               });
    }

    /// Accumulate one declared access over `range` for `slot`, seeding from canonical on first touch.
    /// Call once per binding; several declares of one box merge into a single barrier at flush.
    ///
    /// A box already declared for this op with a different layout is combined via `combine_layouts`, which may fall
    /// back to GENERAL with a one-time warning, and asserts on a genuine conflict.
    void declare(sg::command_list_slot slot,
                 sg::subresource_range range,
                 sg::pipeline_stage_flags stages,
                 sg::access_flags access,
                 sg::texture_layout layout)
    {
        auto& s = slot_for(slot);
        bool warned = false;
        s.partition.for_each_box_in(range,
                                    [&](sg::subresource_range const&, sg::resource_access_state& state)
                                    {
                                        sg::texture_layout target = layout;
                                        bool const touched
                                            = state.has_pending_declares() || state.has_pending_layout_change();
                                        if (touched && state.curr_layout != layout)
                                        {
                                            combined_layout const c = combine_layouts(state.curr_layout, layout);
                                            CC_ASSERT(c.result != layout_combine::conflict,
                                                      "a texture subresource is bound with conflicting layouts in one "
                                                      "operation");
                                            if (c.result == layout_combine::degraded && !warned)
                                            {
                                                CC_LOG_WARNING("a texture is bound as both a sampled and a storage "
                                                               "view in one operation, so it is transitioned to "
                                                               "GENERAL for both");
                                                warned = true;
                                            }
                                            target = c.layout;
                                        }
                                        state.declare(stages, access, target);
                                    });
    }

    /// Test-and-set the per-op pending flag; true only the first time since the last flush.
    [[nodiscard]] bool mark_pending_barrier(sg::command_list_slot slot)
    {
        int const i = int(slot);
        CC_ASSERT(i < _slots.size() && _slots[i].active, "mark_pending_barrier before declare");
        auto& s = _slots[i];
        if (s.pending_barrier)
            return false;
        s.pending_barrier = true;
        return true;
    }

    /// Test-and-set the finalize-set flag; true only the first time for this slot.
    [[nodiscard]] bool mark_recorded(sg::command_list_slot slot)
    {
        int const i = int(slot);
        CC_ASSERT(i >= 0, "mark_recorded with an invalid command_list_slot");
        while (_slots.size() <= i)
            _slots.push_back(slot_state{});
        auto& s = _slots[i];
        if (s.recorded)
            return false;
        s.recorded = true;
        return true;
    }

    /// The per-box barriers satisfying everything declared for `slot` since the last flush; empty means all freebies.
    [[nodiscard]] cc::small_vector<vulkan_subresource_barrier, 4> flush(sg::command_list_slot slot)
    {
        int const i = int(slot);
        CC_ASSERT(i < _slots.size() && _slots[i].active, "flush of a texture this list never declared");
        auto& s = _slots[i];
        s.pending_barrier = false;
        cc::small_vector<vulkan_subresource_barrier, 4> out;
        s.partition.for_each_box_in(sg::subresource_range::whole(s.partition.extent()),
                                    [&](sg::subresource_range const& box_range, sg::resource_access_state& state)
                                    {
                                        if (!state.has_pending_declares() && !state.has_pending_layout_change())
                                            return;
                                        auto const b = state.flush();
                                        if (b.needed)
                                            out.push_back({box_range, b});
                                    });
        s.partition.try_merge();
        return out;
    }

    /// `slot`'s list was submitted.
    /// The last list using the texture commits its layout as the new canonical; any earlier one hands the texture
    /// back in the canonical layout, so the lists still open find it as they left it.
    [[nodiscard]] cc::small_vector<vulkan_subresource_barrier, 4> finalize(sg::command_list_slot slot)
    {
        int const i = int(slot);
        CC_ASSERT(i < _slots.size() && _slots[i].active, "finalize of a texture this list never touched");
        auto& s = _slots[i];
        CC_ASSERT(!has_pending_declares(s.partition), "a declared texture access was never flushed by a GPU op");
        CC_ASSERT(_active_slot_count > 0, "finalize of a texture with no active slots");

        cc::small_vector<vulkan_subresource_barrier, 4> out;
        bool const was_last = --_active_slot_count == 0;
        if (was_last)
            _canonical = s.partition;
        else
            out = revert_to_canonical(s);
        s = slot_state{};
        return out;
    }

    /// `slot`'s list was dropped: its work never runs, so the canonical layout is untouched.
    void discard(sg::command_list_slot slot)
    {
        int const i = int(slot);
        CC_ASSERT(i < _slots.size() && _slots[i].active, "discard of a texture this list never touched");
        CC_ASSERT(_active_slot_count > 0, "discard of a texture with no active slots");
        --_active_slot_count;
        _slots[i] = slot_state{};
    }

    [[nodiscard]] int active_slot_count() const { return _active_slot_count; }

private:
    struct slot_state
    {
        // subresource_partition's default ctor is explicit, so the initializer names the type.
        // The extent is set when the slot is seeded.
        sg::subresource_partition partition = sg::subresource_partition();
        bool active = false;
        bool recorded = false;
        bool pending_barrier = false;
    };

    slot_state& slot_for(sg::command_list_slot slot)
    {
        int const i = int(slot);
        CC_ASSERT(i >= 0, "declare with an invalid command_list_slot");
        while (_slots.size() <= i)
            _slots.push_back(slot_state{});
        auto& s = _slots[i];
        if (!s.active)
        {
            s.active = true;
            s.partition = _canonical;
            ++_active_slot_count;
        }
        return s;
    }

    // Transition every diverged box back to the canonical layout.
    // While a list uses the texture the active-slot count is >= 1, so no other list can promote it: canonical is
    // stable across that list's lifetime, which makes "the layout it entered with" and "the canonical layout" the same.
    cc::small_vector<vulkan_subresource_barrier, 4> revert_to_canonical(slot_state& s)
    {
        cc::small_vector<vulkan_subresource_barrier, 4> out;
        bool warned = false;
        for (auto const& cbox : _canonical.boxes())
        {
            sg::texture_layout const canonical_layout = cbox.state.prev_layout;
            s.partition.for_each_box_in(cbox.range,
                                        [&](sg::subresource_range const& box_range, sg::resource_access_state& state)
                                        {
                                            if (state.prev_layout == canonical_layout)
                                                return;
                                            state.declare({}, {}, canonical_layout);
                                            auto const b = state.flush();
                                            if (b.needed)
                                            {
                                                out.push_back({box_range, b});
                                                if (!warned)
                                                {
                                                    CC_LOG_WARNING("reverting a texture to its canonical layout at "
                                                                   "submit because other command lists are still open "
                                                                   "(a hidden cost of concurrent recording)");
                                                    warned = true;
                                                }
                                            }
                                        });
        }
        return out;
    }

    static bool has_pending_declares(sg::subresource_partition const& p)
    {
        for (auto const& box : p.boxes())
            if (box.state.has_pending_declares())
                return true;
        return false;
    }

    cc::small_vector<slot_state, 4> _slots; // indexed by command_list_slot
    sg::subresource_partition _canonical;   // the between-lists state
    int _active_slot_count = 0;             // how many open lists are using this texture
};
