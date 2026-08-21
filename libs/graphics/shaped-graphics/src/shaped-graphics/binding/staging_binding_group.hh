#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/binding_group_layout.hh>
#include <shaped-graphics/binding/sampler.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/views.hh>

/// A binding's identity inside a staging_binding_group, resolved once from its name.
/// Opaque: it is an index into the group's internal slot table, not a descriptor position — that indirection
/// is what carries a binding's heap, its first descriptor and its element count, and it is where every set is bounds-checked.
/// Only meaningful for the group it came from, and `invalid` is what an unknown name resolves to.
enum class sg::binding_slot : sg::u32
{
    invalid = ~sg::u32(0),
};

/// A mutable builder for a binding_group: a CPU-side descriptor image the setters update in place, and `snapshot` mints an immutable binding_group from.
/// This is what makes a large, mostly-stable group — a bindless table of textures and buffers — affordable:
/// a set touches one descriptor instead of rebuilding the group, and an unchanged frame's snapshot does no work at all.
///
/// **The setters name the shape they act on, and never infer it.**
/// `set_binding` is for a scalar binding and rejects an array; the `*_array_*` family is for an array binding and rejects a scalar.
/// Nothing here picks an element for you: an element index is always an argument, and always bounds-checked against the binding's count.
///
/// `slot_of` is the one name lookup, done once.
/// The name-taking setters are the whole-binding ones only — those are the one-shot wiring calls.
/// Per-element and per-range work runs off a resolved `binding_slot`, which is the point of having one.
///
/// **Not thread-safe** — one owner mutates it, and `snapshot()` counts as a mutation because it caches.
///
/// It starts fully vacant: every descriptor is its binding's empty value, so a snapshot is always safe to bind.
///
/// **Every binding must be set at least once before the first snapshot**, the way create_binding_group rejects a binding that was not provided.
/// A binding nobody wires reads zero at runtime and looks like a shader bug, so the demand is that you say what it holds — not that it holds anything.
/// An array element is allowed to stay vacant, and clearing an array counts: `unset_array` and even an empty range say "deliberately empty".
/// The exception is a static sampler, which lives in the root signature and has no descriptor here to set.
///
/// Snapshots are independent of the builder and of each other: mutating after a snapshot only dirties the cache,
/// and each snapshot stays valid — with its resources kept alive — for as long as anyone holds its handle.
/// Held via staging_binding_group_handle.
///
/// Abstract: a backend subclasses it and owns the staging descriptors (dx12 a non-shader-visible heap).
/// See libs/graphics/shaped-graphics/docs/concepts/bindings.md.
class sg::staging_binding_group
{
public:
    virtual ~staging_binding_group();

    [[nodiscard]] binding_group_layout_handle const& layout() const { return _layout; }

    /// Whether the next snapshot has to mint a new group rather than hand back the cached one.
    [[nodiscard]] bool is_dirty() const { return _dirty; }

    // --- name -> slot -------------------------------------------------------------------------------

    /// The slot of binding `name`, or `binding_slot::invalid` if the layout has no binding of that name.
    [[nodiscard]] binding_slot slot_of(cc::string_view name) const;

    /// Whether `slot` addresses an array binding (count > 1) — the family of setters it accepts.
    [[nodiscard]] bool is_array(binding_slot slot) const;

    /// The number of elements `slot` holds; 1 for a scalar binding.
    /// The bound every element index and range is checked against.
    [[nodiscard]] int array_size(binding_slot slot) const;

    // --- one scalar binding -------------------------------------------------------------------------

    /// Binds `view` to the scalar binding at `slot`; the view must satisfy the binding (see sg::accepts).
    /// Asserts on an array binding — an array element is set through the `*_array_*` family.
    ///
    /// There is no `unset_binding`: only an array element can be *absent*, while a scalar is bound for the group's life and merely set to another view.
    /// A deliberately empty scalar is one of those views rather than an absence — sg::tlas_view{} is the null
    /// acceleration structure every ray misses — so it is set like any other.
    void set_binding(binding_slot slot, raw_view const& view);

    // --- one array element --------------------------------------------------------------------------

    /// Binds `view` at element `element` of the array binding at `slot`; `element` must be within the binding's count.
    void set_array_element(binding_slot slot, int element, raw_view const& view);

    /// Returns element `element` to vacant — a null descriptor, and its resource released.
    void unset_array_element(binding_slot slot, int element);

    // --- part of an array ---------------------------------------------------------------------------

    /// Binds `views` at elements [`first_element`, `first_element` + views.size()) and leaves every other element alone.
    /// The run must fit inside the binding's count; an empty run writes nothing, and still counts as having set the binding.
    void set_array_range(binding_slot slot, int first_element, cc::span<raw_view const> views);

    /// Returns elements [`first_element`, `first_element` + `count`) to vacant and leaves every other element alone.
    void unset_array_range(binding_slot slot, int first_element, int count);

    // --- the whole array ----------------------------------------------------------------------------

    /// Replaces the entire array binding at `slot` with `views` at elements [0, views.size()).
    /// Every element the run does not cover is cleared, so the array afterwards holds exactly `views` and nothing else.
    void set_array(binding_slot slot, cc::span<raw_view const> views);

    /// The same replacement, with the run placed at `first_element` instead of at 0.
    /// Elements before it and after it are cleared — this replaces the whole array, not a range of it.
    void set_array(binding_slot slot, int first_element, cc::span<raw_view const> views);

    /// Clears every element of the array binding at `slot`.
    void unset_array(binding_slot slot);

    // --- samplers -----------------------------------------------------------------------------------

    /// Supplies the dynamic sampler at `slot`; the binding must be a sampler, and a dynamic one — a static sampler lives in the root signature.
    /// Like every other binding it must be set once before the first snapshot; `unset_sampler` is how you ask for the default state explicitly.
    void set_sampler(binding_slot slot, sampler const& smp);

    /// Returns the dynamic sampler at `slot` to the default sampler state.
    void unset_sampler(binding_slot slot);

    // --- the same, by name --------------------------------------------------------------------------
    // Whole-binding wiring, where resolving the name every call is not the cost that matters.
    // Each asserts the layout has a binding of that name.

    void set_binding(cc::string_view name, raw_view const& view) { set_binding(checked_slot_of(name), view); }
    void set_array(cc::string_view name, cc::span<raw_view const> views) { set_array(checked_slot_of(name), views); }
    void unset_array(cc::string_view name) { unset_array(checked_slot_of(name)); }
    void set_sampler(cc::string_view name, sampler const& smp) { set_sampler(checked_slot_of(name), smp); }
    void unset_sampler(cc::string_view name) { unset_sampler(checked_slot_of(name)); }

    // --- minting ------------------------------------------------------------------------------------

    /// The current state as an immutable binding_group.
    /// Returns the previous snapshot unchanged while nothing has been set since — so an unchanged frame costs nothing and rebinds nothing.
    /// Throws sg::binding_group_exception when the group cannot be minted (descriptor-heap exhaustion).
    [[nodiscard]] binding_group_handle snapshot();

    [[nodiscard]] cc::result<binding_group_handle> try_snapshot();

protected:
    /// `descriptor_offsets` carries one entry per layout binding, in declaration order: the index of that
    /// binding's first descriptor in its heap — the view heap, or the sampler heap for a sampler binding.
    /// `-1` marks a binding the backend keeps no descriptor for, which is a static sampler.
    /// Every descriptor must already hold its binding's empty value by the time this returns.
    staging_binding_group(binding_group_layout_handle layout, cc::vector<int> descriptor_offsets);

    /// Writes `views` into the view heap at [`first_descriptor`, `first_descriptor` + views.size()), replacing whatever was there — including its resource references.
    /// One call per contiguous run, so filling a range costs one dispatch rather than one per element.
    /// The caller has resolved and bounds-checked the range and validated every view against `b`; none of them is vacant, and the run is never empty.
    virtual void write_view_descriptors(int first_descriptor, binding const& b, cc::span<raw_view const> views) = 0;

    /// Writes `count` of `b`'s empty descriptors into the view heap at `first_descriptor`, releasing the resources they referenced.
    /// The clearing half of the pair above, and likewise one call per run; `count` is never 0.
    virtual void clear_view_descriptors(int first_descriptor, binding const& b, int count) = 0;

    /// Writes the dynamic sampler descriptor at `descriptor_index` in the sampler heap.
    /// Single, not a run: dynamic sampler arrays are unsupported, so a sampler binding is always one descriptor.
    virtual void write_sampler_descriptor(int descriptor_index, sampler const& smp) = 0;

    /// Mints an immutable group from the current descriptors.
    /// Called only when something changed since the last snapshot.
    [[nodiscard]] virtual cc::result<binding_group_handle> mint() = 0;

private:
    /// What a binding_slot indirects to: the binding it names, and where its descriptors live.
    /// Built once at construction, so every set is an index and a bounds check rather than a search.
    struct slot_info
    {
        // The layout's own binding — count, type and texture_dimension all read from here rather than being
        // copied out, so validation always sees exactly what the layout declared.
        // The layout outlives this group, and its binding list is fixed at creation, so the pointer is stable.
        binding const* declared = nullptr;

        int first_descriptor = -1; // in the view heap, or the sampler heap for a sampler binding; -1 = no descriptor
        bool touched = false;      // set at least once — what snapshot() demands of every settable binding
    };

    [[nodiscard]] binding_slot checked_slot_of(cc::string_view name) const;

    // The slot's entry, asserting the slot is one of this group's.
    [[nodiscard]] slot_info const& info_of(binding_slot slot) const;

    // Records that the caller has said what this binding holds — which is all snapshot() asks.
    // Every public setter calls it, including the ones that write no descriptor at all: an empty range is a
    // deliberate "nothing here", and only the binding nobody mentioned is the bug worth catching.
    void touch(binding_slot slot);

    // Validate + write one contiguous run, in a single call down to the backend.
    // The range is checked against the binding's count here, so no caller has to, and an empty run is a no-op
    // that dispatches nothing.
    void write_run(binding_slot slot, int first_element, cc::span<raw_view const> views);
    void clear_run(binding_slot slot, int first_element, int count);

    binding_group_layout_handle _layout;
    cc::map<cc::string, int> _slot_by_name; // binding name -> binding_slot value (int, so a miss can answer -1)
    cc::vector<slot_info> _slots;           // indexed by binding_slot; parallel to _layout->bindings()

    int _untouched = 0; // settable bindings not yet set; snapshot() asserts this is zero

    binding_group_handle _snapshot;
    bool _dirty = true;
};
