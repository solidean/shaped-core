#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/hash.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/barrier/access_inference.hh>
#include <shaped-graphics/barrier/footprint.hh>
#include <shaped-graphics/barrier/resource_access_state.hh> // sg::impl::member_track
#include <shaped-graphics/command_list/compute.hh>          // sg::array_buffer_access, sg::array_texture_access
#include <shaped-graphics/context/metrics.hh>               // sg::impl::stat_counts
#include <shaped-graphics/fwd.hh>

/// One bind point's live array declarations, and how each op meets them: the half of sticky declarations every backend shares.
/// See libs/graphics/shaped-graphics/docs/concepts/barriers.md, "Array bindings".
///
/// **A declaration lives until it is withdrawn**, so one bound array meets it at many ops.
/// Planning is per op, exactly as for a one-op declaration: `plan_array_declarations` decides skip, as declared or cover all.
/// What changes is the element work.
/// The elements are checked against a group's array the first time that group meets them, not at every op.
/// A read-only application that is still *current* under a key that covers the op is skipped outright.
///
/// **Current** is what keeps a skip equivalent to declaring again.
/// The list counts `member_writes`: every tracked access landing in the write bucket on a resource some application tracked.
/// An application snapshots that count right after its own tracking, and stays current while the count is unchanged.
/// One where an element's state already held a write or a layout change earlier in its op is never current.
/// A skip is decided before the op's later arrays track, so one whose op moves the count afterwards applies after all.
/// The skipped array may hold the member written, and declaring it again would have combined both uses in one barrier.
///
/// **Every write of an op is tracked before `declare_array_accesses`**, which is what the re-check above relies on.
/// A write tracked after `apply` returns but before the op's barrier flush would leave a skipped element in the wrong state.
/// Whatever a backend tracks between the two must be a read; `array_skip_guard` asserts it at the flush.

/// One array binding of one bound group, as an op meets it.
/// `ArrayBinding` is the backend's: it has `name`, `is_texture`, `binding`, `bound_as` and `elements`, each element `is_vacant()`.
template <class ArrayBinding>
struct sg::impl::array_site
{
    /// The bound group's identity, which must not be reused by another group while the list records.
    void const* group = nullptr;
    int group_index = 0;
    ArrayBinding const* array = nullptr;
};

/// Holds state keyed on group addresses, so it is neither copyable nor movable.
class sg::impl::array_declaration_book
{
public:
    array_declaration_book() = default;
    array_declaration_book(array_declaration_book const&) = delete;
    array_declaration_book(array_declaration_book&&) = delete;
    array_declaration_book& operator=(array_declaration_book const&) = delete;
    array_declaration_book& operator=(array_declaration_book&&) = delete;

    /// Adds live declaration `id` of the buffer array `name`.
    void declare_buffers(u32 id, cc::string_view name, cc::span<array_buffer_access const> elements);

    /// Adds live declaration `id` of the texture array `name`.
    void declare_textures(u32 id, cc::string_view name, cc::span<array_texture_access const> elements);

    /// Ends declaration `id` if it lives here, and says whether it did.
    bool withdraw(u32 id);

    /// Ends every declaration here, as a rendering scope's close does.
    void end_all();

    [[nodiscard]] bool is_empty() const { return _declarations.empty(); }

    /// Meets every array the op's bound groups hold with the live declarations naming it, and tracks what that decides.
    ///
    /// Every array is checked before any is tracked, so a declaration that asserts leaves the op untouched.
    /// `track_buffer(element, stages, access)` and `track_texture(element, stages, access, layout)` track one declared element,
    /// marking its resource an array member and returning its `member_track`.
    /// `cover(array, element, plan)` tracks one element of an array covered whole, which is not an application.
    /// `member_writes` is the list's count, read after each application's tracking.
    /// Returns whether some array of the op was skipped, which the backend arms its `array_skip_guard` with.
    template <class ArrayBinding, class TrackBuffer, class TrackTexture, class Cover>
    [[nodiscard]] bool apply(cc::span<array_site<ArrayBinding> const> sites,
                             pipeline_footprint const* footprint,
                             void const* pipeline,
                             pipeline_stage_flags op_stages,
                             u64 const& member_writes,
                             stat_counts& stats,
                             TrackBuffer&& track_buffer,
                             TrackTexture&& track_texture,
                             Cover&& cover);

private:
    /// What the live declarations naming one array add up to.
    struct name_state
    {
        cc::string name;
        bool is_texture = false;

        /// Bumped whenever a declaration naming it is made or ends, so an application of another set is never current.
        u32 generation = 0;
        int live = 0;
        array_declarations declared;
    };

    struct declaration
    {
        u32 id = 0;
        isize name_index = 0;
        cc::vector<array_buffer_access> buffers;
        cc::vector<array_texture_access> textures;
    };

    /// One array binding of one bound group, which its application is found by.
    struct application_key
    {
        void const* group = nullptr;
        isize binding = -1;

        [[nodiscard]] bool operator==(application_key const&) const = default;

        [[nodiscard]] friend u64 hash(application_key const& k) { return cc::make_hash(k.group, k.binding); }
    };

    /// How one array binding of one group was last checked and applied.
    struct application
    {
        /// The generation its elements were checked against, or none yet.
        cc::optional<u32> checked_generation;

        /// The generation it was applied at; meaningful only while `current`.
        u32 applied_generation = 0;

        /// What it applied since its last invalidation, the union a later op's key must fall within.
        pipeline_stage_flags stages;
        access_flags access;

        /// The list's `member_writes` right after its tracking.
        u64 snapshot = 0;
        bool current = false;

        /// The op that last skipped it, as the book counts ops.
        u64 skipped_op = 0;
    };

    [[nodiscard]] isize find(cc::string_view name, bool is_texture) const;
    [[nodiscard]] isize find_or_add(cc::string_view name, bool is_texture);
    [[nodiscard]] application& application_for(void const* group, isize binding);

    /// How the op meets the array of `site`, under the live declarations of `name_index`, or none for a negative one.
    template <class ArrayBinding>
    [[nodiscard]] array_plan plan_of(array_site<ArrayBinding> const& site,
                                     isize name_index,
                                     pipeline_footprint const* footprint,
                                     void const* pipeline,
                                     pipeline_stage_flags op_stages) const;

    /// Tracks every element the live declarations of `name_index` list in `array`, as `plan` widens them, and records it in `app`.
    /// An application that was not `still_current` starts its union afresh.
    template <class ArrayBinding, class TrackBuffer, class TrackTexture>
    void track_declared(application& app,
                        isize name_index,
                        ArrayBinding const& array,
                        array_plan const& plan,
                        bool still_current,
                        u64 const& member_writes,
                        stat_counts& stats,
                        TrackBuffer& track_buffer,
                        TrackTexture& track_texture);

    void add(declaration d, cc::string_view name, bool is_texture);
    void recount(isize name_index);

    /// Asserts that every element the declarations naming `name_index` list is in range of `elements` and not vacant.
    template <class Elements>
    void check(isize name_index, Elements const& elements) const;

    cc::vector<name_state> _names;
    cc::vector<declaration> _declarations;

    /// A record's address is stable across later inserts.
    cc::map<application_key, application> _applications;

    /// Ops `apply` has met, which tells a skip of this op from an earlier one's.
    u64 _ops = 0;
};

/// What an op that skipped an array leaves its barrier flush to check: that no member was written since `apply`.
/// A backend arms it with what `apply` returned, and checks it at the start of every barrier flush.
class sg::impl::array_skip_guard
{
public:
    /// Records `member_writes` if the op skipped an array, and disarms otherwise.
    void arm(bool skipped, u64 member_writes)
    {
        _armed = skipped;
        _member_writes = member_writes;
    }

    /// Asserts that a skip armed since the last flush saw no member written afterwards, and disarms.
    void check(u64 member_writes)
    {
        CC_ASSERT(!_armed || member_writes == _member_writes,
                  "a member of an array was written after declare_array_accesses skipped an array in the same op; "
                  "every write of an op must be tracked before its arrays are declared");
        _armed = false;
    }

private:
    bool _armed = false;
    u64 _member_writes = 0;
};

template <class Elements>
void sg::impl::array_declaration_book::check(isize name_index, Elements const& elements) const
{
    auto const count = isize(elements.size());
    auto const in_range = [&](int index) { return index >= 0 && index < count && !elements[index].is_vacant(); };
    for (auto const& d : _declarations)
    {
        if (d.name_index != name_index)
            continue;
        for (auto const& e : d.buffers)
        {
            CC_ASSERT(e.index >= 0 && e.index < count, "declared array element index out of range");
            CC_ASSERT(in_range(e.index), "declared array element is vacant (nothing is bound there)");
        }
        for (auto const& e : d.textures)
        {
            CC_ASSERT(e.index >= 0 && e.index < count, "declared array element index out of range");
            CC_ASSERT(in_range(e.index), "declared array element is vacant (nothing is bound there)");
        }
    }
}

template <class ArrayBinding, class TrackBuffer, class TrackTexture, class Cover>
bool sg::impl::array_declaration_book::apply(cc::span<array_site<ArrayBinding> const> sites,
                                             pipeline_footprint const* footprint,
                                             void const* pipeline,
                                             pipeline_stage_flags op_stages,
                                             u64 const& member_writes,
                                             stat_counts& stats,
                                             TrackBuffer&& track_buffer,
                                             TrackTexture&& track_texture,
                                             Cover&& cover)
{
    for (auto const& site : sites)
    {
        auto const name_index = find(site.array->name, site.array->is_texture);
        if (name_index < 0 || _names[name_index].live == 0)
            continue;

        // Checked once per group and declaration set: a different group at the same name checks again.
        auto const generation = _names[name_index].generation;
        auto& app = application_for(site.group, site.array->binding);
        if (app.checked_generation != generation)
        {
            check(name_index, site.array->elements);
            app.checked_generation = generation;
        }
    }

    ++_ops;
    auto const writes_before = member_writes;
    auto skipped = false;
    for (auto const& site : sites)
    {
        auto const& array = *site.array;
        auto const name_index = find(array.name, array.is_texture);
        auto const plan = plan_of(site, name_index, footprint, pipeline, op_stages);

        if (plan.how == array_plan::mode::cover_all)
        {
            auto covered = i64(0);
            for (auto const& element : array.elements)
                if (!element.is_vacant())
                {
                    cover(array, element, plan);
                    ++covered;
                }
            stats.add(stat::array_element_tracks, covered);
            continue;
        }
        if (plan.how != array_plan::mode::as_declared)
            continue;

        auto const& name = _names[name_index];
        auto& app = application_for(site.group, array.binding);
        auto const applied_access = name.declared.access | plan.widen_by;
        auto const still_current
            = app.current && app.applied_generation == name.generation && app.snapshot == member_writes;

        // Covered rather than equal: a read at stages already synced since the last write needs no barrier.
        // A write is never skipped.
        if (!is_unordered_write(applied_access) && still_current && app.stages.has_all(plan.stages)
            && app.access.has_all(applied_access))
        {
            app.skipped_op = _ops;
            skipped = true;
            continue;
        }
        track_declared(app, name_index, array, plan, still_current, member_writes, stats, track_buffer, track_texture);
    }
    if (!skipped || member_writes == writes_before)
        return skipped;

    // A member written after a skip was decided may be one the skipped array holds too, in another layout or at another access.
    // Declaring that array again would have combined both uses, so every skip of this op is applied after all.
    for (auto const& site : sites)
    {
        auto const& array = *site.array;
        auto* const app = _applications.get_ptr(application_key{.group = site.group, .binding = array.binding});
        if (app == nullptr || app->skipped_op != _ops)
            continue;
        app->skipped_op = 0;
        auto const name_index = find(array.name, array.is_texture);
        auto const plan = plan_of(site, name_index, footprint, pipeline, op_stages);
        track_declared(*app, name_index, array, plan, false, member_writes, stats, track_buffer, track_texture);
    }
    return false;
}

template <class ArrayBinding>
sg::impl::array_plan sg::impl::array_declaration_book::plan_of(array_site<ArrayBinding> const& site,
                                                               isize name_index,
                                                               pipeline_footprint const* footprint,
                                                               void const* pipeline,
                                                               pipeline_stage_flags op_stages) const
{
    auto const& array = *site.array;
    auto const declared = name_index >= 0 ? _names[name_index].declared : array_declarations();
    auto use = cc::optional<slot_use>();
    if (footprint != nullptr && footprint->is_known())
        use = footprint->use_of(site.group_index, array.binding);
    return plan_array_declarations(pipeline, array.name, use, array.bound_as, op_stages, declared);
}

template <class ArrayBinding, class TrackBuffer, class TrackTexture>
void sg::impl::array_declaration_book::track_declared(application& app,
                                                      isize name_index,
                                                      ArrayBinding const& array,
                                                      array_plan const& plan,
                                                      bool still_current,
                                                      u64 const& member_writes,
                                                      stat_counts& stats,
                                                      TrackBuffer& track_buffer,
                                                      TrackTexture& track_texture)
{
    if (!still_current)
    {
        app.stages = {};
        app.access = {};
    }

    auto held_write = false;
    auto tracked = i64(0);
    for (auto const& d : _declarations)
    {
        if (d.name_index != name_index)
            continue;
        for (auto const& e : d.buffers)
        {
            held_write
                = track_buffer(array.elements[e.index], plan.stages, e.access | plan.widen_by).held_write || held_write;
            ++tracked;
        }
        for (auto const& e : d.textures)
        {
            held_write = track_texture(array.elements[e.index], plan.stages, e.access | plan.widen_by, e.layout).held_write
                      || held_write;
            ++tracked;
        }
    }
    stats.add(stat::array_element_tracks, tracked);

    auto const& name = _names[name_index];
    auto const applied_access = name.declared.access | plan.widen_by;
    app.applied_generation = name.generation;
    app.stages |= plan.stages;
    app.access |= applied_access;
    app.snapshot = member_writes;
    app.current = !is_unordered_write(applied_access) && !held_write;
}
