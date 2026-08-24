#pragma once

#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/binding/bindless_array.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/resources/bindless_tables.hh>
#include <shaped-viewer/resources/instance_data.hh>
#include <shaped-viewer/resources/resource_managers.hh>

/// How much follow-up GPU work the manager may record per epoch.
///
/// This is a *different* budget from the upload one, and conflating them is the usual mistake.
/// Bytes in flight are sg's to schedule (`ctx.stream.set_upload_ratio`, per-handle priorities, aging); what
/// this bounds is the work that runs *after* a resource has landed — mip generation today, compression later.
/// Leaving it unbounded is what produces the microstutter: a frame where forty textures finish at once would
/// otherwise record forty mip chains before it draws anything.
///
/// The unit is dispatches rather than milliseconds because it is the one the manager can count without
/// measuring: a level is one dispatch, and `sr::box_filter_mipmap_routine::level_count` says how many a
/// texture needs before committing to any of them.
struct sv::work_budget
{
    /// Dispatches per epoch; <= 0 disables follow-up work entirely, leaving every texture at its base level.
    i32 max_dispatches_per_epoch = 16;
};

/// What the manager does with a resource once it has landed.
///
/// Generating mips is opt-in per manager rather than per acquire, because a caller who wants none wants none
/// for the whole scene, and one who wants them wants them for everything the budget can reach.
struct sv::texture_policy
{
    /// Fill the levels an acquire did not supply, through `sr::box_filter_mipmap_routine`.
    bool generate_mips = true;
};

/// Per-manager configuration for a whole scene's GPU resources, plus the bindless tables they are bound through.
struct sv::gpu_resource_manager_config
{
    manager_config meshes = {};
    manager_config materials = {};
    manager_config textures = {};
    manager_config attributes = {};
    manager_config instances = {};
    bindless_config bindless = {};
    texture_policy textures_policy = {};
    work_budget work = {};
};

/// A snapshot of the bindless tables, taken for one recording — `gpu_resource_manager::freeze()`'s return value.
///
/// Holding one means the manager is locked, so nothing can mint a descriptor that this snapshot would not
/// contain; it unlocks on destruction.
/// Several may be taken per epoch, one per recording, because sg only ever reclaims an element *not acquired
/// this epoch* — so indices another recording already handed to the GPU survive this one's mints.
///
/// `elements(table)` is what a dispatch needs for `declare_array_texture_access` / `declare_array_buffer_access`:
/// sg asserts that every bound array binding was declared, and the manager is the only thing that knows which
/// elements this epoch actually acquired.
/// Move-only; a moved-from snapshot is disarmed and unlocks nothing.
class sv::bound_resources
{
public:
    ~bound_resources();

    bound_resources(bound_resources&& rhs) noexcept;
    bound_resources(bound_resources const&) = delete;
    bound_resources& operator=(bound_resources const&) = delete;
    bound_resources& operator=(bound_resources&&) = delete;

    /// The immutable binding group to bind, covering every table the manager declares.
    [[nodiscard]] sg::binding_group_handle const& group() const { return _group; }

    /// Every element index acquired in this epoch for `table`, in acquire order — the access declaration's input.
    /// Empty means the table is unused this epoch, which is a perfectly good thing to declare.
    [[nodiscard]] cc::span<u32 const> elements(bindless_table table) const;

private:
    friend class gpu_resource_manager;
    bound_resources(gpu_resource_manager& manager, sg::binding_group_handle group)
      : _manager(&manager), _group(cc::move(group))
    {
    }

    gpu_resource_manager* _manager = nullptr; // null = disarmed (moved-from)
    sg::binding_group_handle _group;
};

/// Where resource management comes together: the mesh, material and texture managers, the staging binding group
/// their views are bound through, and one `sg::bindless_array` per declared table.
///
/// One per scene; passed to `sv::view_renderer::execute` / `sv::viewer_renderer::execute`.
/// Everything shares the context it is created with, which must outlive it.
///
/// **`advance_to` is the caller's to run, and it is keyed on the epoch rather than on the frame.**
/// It is idempotent: a call for an epoch already reached does nothing, so every window's draw path may call it
/// and whichever gets there first pays.
/// That is what makes several windows — drawing at different rates, into one context — correct without any of
/// them owning the tick.
///
/// **The arrays are private, and every acquire goes through here.**
/// That is what makes the lock enforceable at all: an array cannot refuse an acquire on behalf of its siblings,
/// because the invariant spans the whole group.
///
/// **An index that outlives its epoch must be pinned, not acquired.**
/// `acquire_texture` hands out a `sg::bindless_index` good for this epoch only, and the type says so — writing
/// one into a buffer that is cached across epochs does not compile.
/// `pin_texture` is what a material buffer holds, and the element stays resident for as long as the handle does.
///
/// TODO: the lock is sound but conservative, and it is not what makes an index valid.
/// What keeps a live index from being reassigned is sg's reclaim rule — a full array reclaims only indices *not
/// acquired this epoch*, and never a pinned one — which is structural and needs no lock.
/// The lock covers one narrower hazard: binding a snapshot taken before your mint.
/// Two things are known-open, and [docs/TODO.md](../../../docs/TODO.md) carries them:
/// the lock *prohibits* where a mint-generation stamp recorded in `bound_resources` would *verify*;
/// and it is global where the hazard is per-recording, so a routine acquiring mid-recording is refused rather
/// than told to re-snapshot and rebind (which is nearly free, since a clean `snapshot()` is cached).
class sv::gpu_resource_manager
{
public:
    /// Creates the five managers, the staging group over `cfg.bindless`'s layout, and one array per table.
    ///
    /// `cfg.bindless` must declare `textures_2d` and `buffers`, whatever else it declares or omits: the texture manager pins into
    /// the first and the attribute and instance managers pin into the second, so a config without them has no manager to build.
    /// Both assert.
    [[nodiscard]] static gpu_resource_manager create(sg::context& ctx, gpu_resource_manager_config const& cfg = {});

    /// Reclaim and advance to epoch `e`, if not already there.
    ///
    /// Evicts what the budgets say to evict — judged against the just-finished epoch's usage, so the work about
    /// to be recorded keeps its working set — and clears the per-table acquired-element lists.
    /// Must not be called while frozen: an epoch advance under a live snapshot would invalidate the very indices
    /// that snapshot was taken for.
    void advance_to(sg::epoch e);

    [[nodiscard]] sg::epoch current_epoch() const { return _epoch; }

    /// The element index for `view` in `table` **this epoch**, minted on a miss.
    /// The view's dimension must match the table's, and the manager must not be locked.
    /// Never store the result where it outlives the epoch — `pin_texture` is what that needs.
    [[nodiscard]] sg::bindless_index acquire_texture(bindless_table table, sg::raw_view const& view);

    /// The same, for the byte-address buffer table.
    [[nodiscard]] sg::bindless_index acquire_buffer(sg::raw_view const& view);

    /// A shared hold on `view`'s element in `table`, whose index stays true until the last handle dies.
    /// This is the index that may be written into GPU memory outliving the epoch — a material buffer above all.
    /// Pinning does not require the manager to be unlocked: it mints no descriptor a bound snapshot could miss
    /// when the element is already resident, and when it is not, the caller is by definition not recording
    /// against it yet.
    [[nodiscard]] sg::bindless_element_handle pin_texture(bindless_table table, sg::raw_view const& view);

    /// The same, for the byte-address buffer table.
    [[nodiscard]] sg::bindless_element_handle pin_buffer(sg::raw_view const& view);

    /// The texture_id for `texture.hash`, resident from a prior acquire (O(1)), or a freshly uploaded one.
    ///
    /// Prefer this over reaching `textures` directly: it also notes the texture's element for this epoch's
    /// access declaration, which a dispatch reading it through a material buffer would otherwise miss.
    [[nodiscard]] texture_id acquire_texture(texture_data const& texture);

    /// Refuses acquires until `unlock`.
    ///
    /// The pair is deliberately available raw, for a caller whose recording does not nest the way `freeze`'s
    /// scope does; `freeze` is the same thing with the unlock attached.
    void lock();
    void unlock();
    [[nodiscard]] bool is_locked() const { return _locked; }

    /// Locks, snapshots the group, and unlocks when the returned value dies — the form to reach for.
    /// A clean snapshot is the cached handle, so freezing an unchanged working set costs nothing.
    [[nodiscard]] bound_resources freeze();

    /// Records whatever follow-up work this epoch's budget allows, oldest request first.
    ///
    /// Must be called with a command list the caller submits, and before the work that reads those resources —
    /// a texture whose mips are generated after the trace that sampled it gains nothing this frame.
    /// What does not fit stays queued for a later epoch, which is exactly the microstutter guard: the queue
    /// drains at a bounded rate rather than all at once.
    /// Returns how many dispatches it recorded.
    i32 record_pending_work(sg::command_list& cmd);

    /// How many resources are still waiting for their follow-up work.
    [[nodiscard]] isize pending_work_count() const { return _pending.size(); }

    /// The parameter block `r` resolves to, uploaded and pinned — what a generated shader reads per instance.
    ///
    /// `layout` must be the one `generate_material_shader` produced for `r`; the two are filled and read at the same offsets, and
    /// handing them in together is what keeps that from being two independent computations of the same thing.
    ///
    /// This is where the chain finally reaches the GPU: a constant is copied inline, a mesh-sourced attribute is uploaded through
    /// `attributes` and enters as an `sv_attribute_desc`, and a sampled texture enters as the bindless index of an already-acquired
    /// `texture_id`.
    /// Every index written into the block is a PINNED one, since the block outlives the epoch that built it.
    ///
    /// Content-cached on `r.parameter_key`, so re-acquiring an unchanged mesh every frame is a lookup.
    [[nodiscard]] instance_id acquire_instance(resolved_material const& r, material_parameter_layout const& layout);

    /// The bytes of `r`'s parameter block, in `layout`'s order — what `acquire_instance` uploads.
    ///
    /// Public because the layout is a contract rather than an internal detail: a caller packing several blocks into one buffer
    /// itself needs exactly this, and it is what a test can check without reading GPU memory back.
    /// It has the side effect `acquire_instance` has — a mesh-sourced attribute is uploaded and pinned here, since the descriptor
    /// it writes is that upload's index.
    [[nodiscard]] cc::vector<byte> build_instance_parameters(resolved_material const& r,
                                                             material_parameter_layout const& layout);

    /// The GPU record for one scene item: where its material parameters live, and where its geometry does.
    ///
    /// This is what retires the "one mesh per view" seam — a view uploads one of these per item and the closest-hit reaches
    /// everything it needs from `InstanceID()`, rather than the trace binding one mesh's buffers globally.
    /// Both ids must be resident.
    [[nodiscard]] instance_gpu describe_instance(mesh_id mesh, instance_id instance);

    /// Whether `table` was declared at all (a budget of 0 omits it).
    [[nodiscard]] bool has_table(bindless_table table) const;

    /// How many elements `table` holds, or 0 if it was not declared.
    [[nodiscard]] u32 table_capacity(bindless_table table) const;

    mesh_manager meshes;
    material_manager materials;
    texture_manager textures;
    attribute_manager attributes;
    instance_manager instances;

    gpu_resource_manager(gpu_resource_manager&&) noexcept = default;
    gpu_resource_manager(gpu_resource_manager const&) = delete;
    gpu_resource_manager& operator=(gpu_resource_manager const&) = delete;
    gpu_resource_manager& operator=(gpu_resource_manager&&) = delete;

private:
    friend class bound_resources;

    /// One entry per declared table, in table order; a table budgeted at 0 has none.
    /// `_slot_of` maps a table onto its entry, so a caller never indexes this by table.
    struct table_entry
    {
        bindless_table table = bindless_table::textures_2d;
        sg::bindless_array array;
        cc::vector<u32> acquired; ///< the element indices acquired this epoch, for the access declaration
    };

    gpu_resource_manager(mesh_manager meshes,
                         material_manager materials,
                         texture_manager textures,
                         attribute_manager attributes,
                         instance_manager instances,
                         sg::staging_binding_group_handle group,
                         cc::vector<table_entry> tables,
                         texture_policy texture_policy,
                         work_budget work_budget);

    /// The entry for `table` in a freshly built list, before `_slot_of` exists to index it.
    [[nodiscard]] static table_entry const* _find_table(cc::span<table_entry const> tables, bindless_table table);


    /// The position of `table` in `_tables`, asserting that it was declared at all.
    [[nodiscard]] i32 _declared_slot_of(bindless_table table) const;

    /// Whether `id` is already queued for follow-up work, so a re-acquire does not queue it twice.
    [[nodiscard]] bool _is_pending(texture_id id) const;

    /// Notes `index` as in use this epoch, for the access declaration; already-present indices are skipped.
    static void _record(table_entry& t, u32 index);

    /// The array for `table`, or null if the table was not declared.
    [[nodiscard]] sg::bindless_array* _array_of(bindless_table table);
    [[nodiscard]] sg::bindless_array const* _array_of(bindless_table table) const;

    [[nodiscard]] sg::bindless_index _acquire(bindless_table table, sg::raw_view const& view);
    [[nodiscard]] sg::bindless_element_handle _pin(bindless_table table, sg::raw_view const& view);

    sg::staging_binding_group_handle _group;

    cc::vector<table_entry> _tables;

    /// Position of each table in `_tables`, or -1 when it was not declared.
    /// Indexed by `bindless_table`.
    i32 _slot_of[u32(bindless_table::count_)] = {};

    /// One resource waiting for its post-load step, in request order.
    /// A texture whose record is gone by the time its turn comes is skipped: eviction is the answer to
    /// "was this still wanted".
    struct pending_work
    {
        texture_id texture = texture_id::invalid;
        i32 dispatches = 0; ///< what it will cost, counted when it was queued
    };
    cc::vector<pending_work> _pending;

    texture_policy _texture_policy;
    work_budget _work_budget;

    sg::epoch _epoch = sg::epoch(0);
    bool _locked = false;
};
