#pragma once

#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/check/flat.hh>

/// What a text does where an operation of the program would have no value: the robustness profile of LEGAL-65.
enum class sgl::check::undefined_guard : sgl::u8
{
    /// Nothing is guarded, and such an operation is the target's undefined behaviour, at no cost.
    none,
    /// The invocation ends instead, before the operation runs, and nothing reports it.
    abort,
    /// The operation and its operands are printed as a failed `assert` is, and the invocation ends as for `abort`.
    /// Without diagnostics there is nothing to print into, and it is `abort`.
    report,
    /// The operation runs on a substitute operand that has a value, so it gives a value the rule names and the
    /// invocation goes on, as WGSL defines its integer division (LEGAL-68).
    define,
};

/// Switches that exist for the tests of the legalizer, each one breaking a rule on purpose.
/// A differential test that still passes with one of them set has no teeth.
struct sgl::check::legalize_options
{
    /// Skips the pins of rule E2, so an operand may be evaluated after the block to its right.
    bool skip_pinning = false;
    /// Skips the tests of rule X5, so an exit that crossed a construct ends there.
    bool skip_flag_tests = false;
    /// Keeps the emulated form of every `flat_by_target` rather than the native one: the target has no native form.
    bool is_emulated = false;
    /// Drops every print with its values, as every check is dropped: for a text compiled without diagnostics (LEGAL-63).
    /// The tree then prints nothing, which is the one way it may differ from the input's behaviour.
    bool erases_prints = false;
    /// Keeps every `assert` as `if (!condition) { print; end }` rather than dropping it, for a text with diagnostics (LEGAL-64).
    bool lowers_asserts = false;
    /// Writes every print as flat statements over `sg_debug` (LEGAL-70), which is what a target compiles.
    /// Left off, a print stays a `flat_print`, which only the interpreter runs.
    bool expands_prints = false;
    /// Writes the shadow of workgroup memory and its prologue as flat statements (LEGAL-71), which is what a target compiles.
    /// Left off, a read and a mark of the shadow stay calls, which only the interpreter runs, and there is no prologue.
    bool expands_workgroup = false;
    /// The target's workgroup memory starts at zero, as WebGPU's does, so LEGAL-71 writes no prologue for it.
    bool workgroup_starts_zeroed = false;
    /// The files and their names a site's origin and id read (EMIT-183); empty leaves a site without a place.
    cc::span<module_file const> files;
    cc::span<cc::string_view const> names;
    /// LEGAL-65: what an integer division by zero, or an array index outside the array, does in the text.
    undefined_guard guards_undefined = undefined_guard::none;
    /// A guard also bounds a buffer index by the buffer's length (LEGAL-66); off only to isolate the other guards.
    bool guards_buffers = true;
};

/// The words a mesh workgroup's polyfill writes into `sg_mesh_output`, and the fetch reads back (CHK-524).
/// Workgroup `g` owns `group_words` words from `g * group_words`: its vertex count, its primitive count, `corners`
/// indices for each of `max_primitives` primitives, `vertex_words` words for each of `max_vertices` vertices, then
/// `primitive_words` words for each primitive.
struct sgl::check::mesh_polyfill_layout
{
    /// 3 for triangles, 2 for lines.
    i32 corners = 3;
    i32 max_vertices = 0;
    i32 max_primitives = 0;
    /// One word a scalar leaf of the vertex struct, in field order.
    i32 vertex_words = 0;
    /// One word a scalar leaf of the struct written per primitive, in field order; 0 for none.
    i32 primitive_words = 0;
    i32 group_words = 0;

    [[nodiscard]] i32 first_index_word() const { return 2; }
    [[nodiscard]] i32 first_vertex_word() const { return 2 + corners * max_primitives; }
    [[nodiscard]] i32 first_primitive_word() const { return first_vertex_word() + max_vertices * vertex_words; }

    bool operator==(mesh_polyfill_layout const&) const = default;
};

/// How a pipeline's polyfilled tessellation stages lie in sg's buffers (CHK-527).
/// Patch `t` of every instance's patches owns `patch_words()` words of `sg_tessellation_controls` from `t * patch_words()`:
/// its control points as the vertex stage returned them, then the factors struct as the control stage did, a word a
/// scalar leaf each, in field order.
/// Its record of `sg_tessellation_patches` is 8 words: the tessellator's 6 factors as floats, then its first point and
/// index, and each point it makes is `vertex_words` words of `sg_mesh_output`.
struct sgl::check::tessellation_polyfill_layout
{
    /// 3 for triangles, 4 for quads, 2 for isolines.
    i32 domain = 3;
    i32 control_points = 0;
    i32 control_words = 0;
    i32 factor_words = 0;
    /// What the evaluation stage returns; 0 where the layout was made from the control stage alone.
    i32 vertex_words = 0;
    /// As `prelude/tessellation.sgl` numbers them: 0 integer, 1 fractional even, 2 fractional odd.
    i32 partitioning = 0;
    /// As D3D's output topologies, which the tessellator writes: 0 `triangle_cw`, 1 `triangle_ccw`, 2 lines.
    i32 output = 0;
    /// The control stage's `max_factor`, which a patch's factors are capped at and sg sizes the scratch for.
    i32 max_factor = k_max_tessellation_factor;

    [[nodiscard]] i32 patch_words() const { return control_points * control_words + factor_words; }

    bool operator==(tessellation_polyfill_layout const&) const = default;
};

/// The words of `sg_task_output` a polyfilled draw whose mesh workgroups the GPU counts takes (CHK-524), behind a
/// header of `k_first_record_word` words: the task workgroups, the mesh workgroups launched, the mesh pass's dispatch
/// arguments, the draw's arguments, and the task grid, which sg writes before the draw.
/// Task workgroup `t` owns `record_words()` words from `k_first_record_word + t * record_words()`: the grid of mesh
/// workgroups it launches, the first of them among all launched, then its payload's words.
/// A draw with no task stage is one task workgroup of no payload, whose record sg writes too: an indirect draw.
struct sgl::check::task_polyfill_layout
{
    /// The task workgroups that hold a record, which the scan writes.
    static constexpr i32 k_task_count_word = 0;
    static constexpr i32 k_launched_word = 1;
    /// Three words, `x, 1, 1`, which the mesh pass is dispatched indirectly by.
    static constexpr i32 k_dispatch_word = 2;
    /// Four words, `vertex_count, 1, 0, 0`, which the vertex half is drawn indirectly by.
    static constexpr i32 k_draw_word = 5;
    /// Three words, the draw's task workgroups along each axis, which the task stage numbers its workgroups by.
    static constexpr i32 k_task_grid_word = 9;
    static constexpr i32 k_first_record_word = 12;
    static constexpr i32 k_record_grid = 0;
    static constexpr i32 k_record_first = 3;
    static constexpr i32 k_record_payload = 4;

    /// One word a scalar leaf of the payload, in field order.
    i32 payload_words = 0;
    /// `@task(…, meshes = N)`'s `N`, which a task workgroup's launch is cut to; 0 where it states none.
    i32 max_meshes = 0;

    [[nodiscard]] i32 record_words() const { return k_record_payload + payload_words; }

    bool operator==(task_polyfill_layout const&) const = default;
};

/// What a polyfilled mesh stage hands its pixel stage per primitive: the vertex struct and the primitive struct of a
/// mesh entry point's output, or a pixel entry point's stage struct and the second struct beside it (CHK-489).
/// `primitive` is `none` where there is none.
struct sgl::check::mesh_corner_parts
{
    type_id vertex = type_id::none;
    type_id primitive = type_id::none;
};

namespace sgl::check
{
/// The structured form of `e` as a core tree that behaves the same: same result, same prints in the same order.
///
/// Expressions first, cheapest rule first:
/// - E1: a block expression moves in front of the statement that holds it, and its value is a `var` its leaves assign.
///   A block whose only leave is its last statement needs no `var`: its statements move, and its value stays in place.
/// - E2: before that, every operand to its LEFT is pinned into a `let`, unless it has no effect and the block cannot change it.
/// - E3: `and` / `or` whose right operand moved statements or has an effect is an `if` over a `var`.
/// - E4: a `while` whose condition moved statements is a `loop` that starts with `if not c { leave }`.
/// - The `end` of a `for` is pinned unless it is stable, since a target evaluates it before every iteration.
///
/// Then exits:
/// - X1: `leave $root` is `return`.
/// - X2: a block whose leaves all stand in tail position disappears, and what follows a leaving `if` moves into its `else`.
/// - X3, X4: any other block is a `once`, and a leave of the innermost `once` or loop is `break`.
/// - X5: a leave or a continue that crosses a `once` or a loop sets a flag and breaks; the flag is tested after each crossed construct.
///
/// Last, constants and lets, on a tree that is core already too:
/// - F1: a pure builtin's call, a `not`, an `and` or an `or` over literals is its literal, where it is an int, a uint
///   or a bool.
/// - F2: an `if` on a literal is the side it takes, and a `while` on false is nothing.
/// - L1: a let whose value repeats the value of a let in scope reads that let instead, where the value reads something
///   and is the same wherever the invocation evaluates it.
/// - L2: a let nothing reads is gone, where its value has no effect and always has one.
/// - L3: a let of a block's result `var` is that `var`, under the let's name.
///
/// Every name it introduces comes from the entry point's mint.
/// A tree that is core already comes back meaning the same, and a `once` or a `break` of the input is read as a block and a leave.
/// What comes back is compacted: every node is reachable from the body, and every expression has one parent.
/// Total: a malformed tree gives a tree, which `find_core_violation` may still refuse.
/// `m` must be the module `e` stands in, and it must declare `bool` for a flag to have a type.
[[nodiscard]] flat_entry_point legalize(checked_module const& m,
                                        flat_entry_point const& e,
                                        legalize_options const& options = {});
/// Whether `e` reads the length of an unbounded binding array (CHK-491).
[[nodiscard]] bool reads_array_lengths(checked_module const& m, flat_entry_point const& e);

/// `e` with every such length read from the reserved `sg_array_lengths`, which sg writes at each dispatch or draw, at
/// the array's position among the entry point's unbounded arrays; what dx12 and vulkan read, and metal does not.
[[nodiscard]] flat_entry_point with_reserved_array_lengths(checked_module const& m, flat_entry_point const& e);

/// Whether `e` lists the reserved `sg_dispatch_grid`, which `with_dispatch_grid_guard` adds.
[[nodiscard]] bool guards_dispatch_grid(checked_module const& m, flat_entry_point const& e);

/// A bare @compute's `e` whose threads past the grid return before its body, against the grid sg writes at each
/// dispatch_threads into `sg_dispatch_grid` (CHK-520); what dx12, vulkan and WebGPU read, and metal does not.
[[nodiscard]] flat_entry_point with_dispatch_grid_guard(checked_module const& m, flat_entry_point const& e);

/// The layout a mesh entry point `e` polyfills into, or the reason it has none yet: a vertex, primitive or payload
/// member that is no scalar, vector or array of them, or a payload where no polyfilled task stage launches `e`,
/// `is_launched` (CHK-524).
[[nodiscard]] cc::result<mesh_polyfill_layout, cc::string> mesh_polyfill_layout_of(checked_module const& m,
                                                                                   flat_entry_point const& e,
                                                                                   bool is_launched = false);

/// The vertex buffers a polyfilled geometry stage's vertex stage reads at most, `sg_geometry_vertices0` on (CHK-525).
inline constexpr i32 k_geometry_polyfill_streams = 4;

/// The threads of a polyfilled geometry stage's workgroup, one a primitive (CHK-525).
inline constexpr i32 k_geometry_polyfill_workgroup = 64;

/// The layout a geometry entry point `g` polyfills into, a primitive's slots in place of a workgroup's, or the reason
/// it has none yet: a stream of points, or a vertex member that is no scalar, vector or array of them (CHK-525).
/// A primitive owns `max_vertices` vertex slots and as many primitive slots as one strip of them cuts into.
[[nodiscard]] cc::result<mesh_polyfill_layout, cc::string> geometry_polyfill_layout_of(checked_module const& m,
                                                                                       flat_entry_point const& g);

/// The vertex stage `v` and the geometry stage `g` of one pipeline, both legalized, as one compute entry point that
/// writes each primitive's slots of `sg_mesh_output`, one thread a primitive of an instance (CHK-525).
/// It assembles each primitive from `sg_geometry_indices` and `sg_geometry_vertices0` on, as `sg_geometry_draw` says.
/// What comes back is no core tree: legalize it before a target writes it.
/// An error names what the polyfill does not carry yet: points, a stage input it has no value for, too many buffers.
[[nodiscard]] cc::result<flat_entry_point, cc::string> geometry_as_compute(checked_module const& m,
                                                                           flat_entry_point const& v,
                                                                           flat_entry_point const& g);

/// The vertex entry point that draws what `geometry_as_compute` wrote: `mesh_as_vertex_fetch`'s, over `g`'s layout.
/// `g` must have a layout.
[[nodiscard]] flat_entry_point geometry_as_vertex_fetch(checked_module const& m, flat_entry_point const& g);

/// The threads of a workgroup of a polyfilled tessellation's compute passes (CHK-527).
inline constexpr i32 k_tessellation_polyfill_workgroup = 64;

/// The layout a tessellation control entry point `tc` and the evaluation entry point `te` after it polyfill into, or
/// why they have none yet; `te` may be null, which leaves `vertex_words` 0.
[[nodiscard]] cc::result<tessellation_polyfill_layout, cc::string>
tessellation_polyfill_layout_of(checked_module const& m, flat_entry_point const& tc, flat_entry_point const* te);

/// The vertex stage `v` and the tessellation control stage `tc` as one compute entry point: a thread a patch of every
/// instance runs `v` on each control point and `tc` on the patch, and writes both into the patch's words, and the
/// tessellator's factors into its record (CHK-527).
[[nodiscard]] cc::result<flat_entry_point, cc::string> tessellation_as_compute(checked_module const& m,
                                                                               flat_entry_point const& v,
                                                                               flat_entry_point const& tc);

/// The tessellation evaluation stage `te` as a compute entry point: a thread a point the tessellator made runs `te` at
/// its domain location, and writes the result into the point's words of `sg_mesh_output` (CHK-527).
/// `tc` is the control stage before it, whose layout says where the patch lies.
[[nodiscard]] cc::result<flat_entry_point, cc::string> tessellation_evaluation_as_compute(checked_module const& m,
                                                                                          flat_entry_point const& tc,
                                                                                          flat_entry_point const& te);

/// A native control stage `tc`, a core tree, whose every return caps each factor at its `max_factor`, so a tessellator
/// that takes factors up to 64 cuts a patch as the polyfill does (CHK-304); `tc` itself where it states none.
[[nodiscard]] flat_entry_point with_capped_factors(checked_module const& m, flat_entry_point const& tc);

/// The vertex entry point a polyfilled tessellation draws with: vertex `i` is point `i`, as `te`'s compute form wrote it.
[[nodiscard]] flat_entry_point tessellation_as_vertex_fetch(checked_module const& m, flat_entry_point const& te);

/// A mesh entry point `e` as the compute entry point that writes its workgroup's slots of `sg_mesh_output` instead of
/// handing them to the rasterizer; its grid is the one sg writes into `sg_dispatch_grid`.
/// Where records launch it, `task` is their layout, a polyfilled task stage's or an indirect draw's: workgroup `g` of
/// its indirect dispatch is the `g`-th launched, whose task workgroup, workgroup id and payload it reads from
/// `sg_task_input`.
/// `e` must have a layout.
[[nodiscard]] flat_entry_point mesh_as_compute(checked_module const& m,
                                               flat_entry_point const& e,
                                               task_polyfill_layout const* task = nullptr);

/// The layout a task entry point `t` polyfills into, or the reason it has none yet: a payload member that is no
/// scalar, vector or array of them (CHK-524).
[[nodiscard]] cc::result<task_polyfill_layout, cc::string> task_polyfill_layout_of(checked_module const& m,
                                                                                   flat_entry_point const& t);

/// A task entry point `t` as the compute entry point that writes its workgroup's record of `sg_task_output`, its
/// first invocation's launch in place of `dispatch_mesh`.
/// Its grid is the header's, and a workgroup past the records `sg_dispatch_grid.threads.x` says the draw holds writes
/// none; `t` must have a layout.
[[nodiscard]] flat_entry_point task_as_compute(checked_module const& m, flat_entry_point const& t);

/// The one-invocation compute entry point that runs between a polyfilled task stage and the mesh stage `e` it
/// launches: the task workgroups that hold a record, each record's first launched workgroup, and the count launched.
/// `sg_dispatch_grid.threads` holds the slots and the records the draw holds, which the counts are cut to; from them it
/// writes the mesh pass's dispatch and the draw's arguments.
/// It takes `e`'s header, since the draw's vertex count is `e`'s; `e` must have a layout.
[[nodiscard]] flat_entry_point mesh_task_scan(checked_module const& m,
                                              flat_entry_point const& e,
                                              task_polyfill_layout const& task);

/// The vertex entry point that draws what `mesh_as_compute` wrote, from `sg_mesh_input`, one invocation a corner of
/// every primitive slot; the corners of a slot past its workgroup's count stand outside the clip volume.
/// `e` must have a layout, and `corner` must be `add_mesh_corner`'s struct where `e` writes values per primitive.
[[nodiscard]] flat_entry_point mesh_as_vertex_fetch(checked_module const& m,
                                                    flat_entry_point const& e,
                                                    type_id corner = type_id::none);

[[nodiscard]] mesh_corner_parts mesh_corner_parts_of(checked_module const& m, flat_entry_point const& e);

/// Adds to `m` the struct a polyfilled mesh stage's vertex half returns where it writes values per primitive: the
/// vertex struct's members, then the primitive struct's, flat, and returns it.
/// Its pixel stage takes the same struct, so each primitive value lands at the location CHK-489 gives it.
/// `m` is a copy of the module the entry points checked in, which a compile of the polyfill writes them from.
[[nodiscard]] type_id add_mesh_corner(checked_module& m, mesh_corner_parts parts);

/// A pixel entry point `e` that takes a primitive struct, as the pixel stage of a polyfilled mesh pipeline: it takes
/// `corner` as its stage struct, and makes its two structs from it before the body.
[[nodiscard]] flat_entry_point pixel_after_mesh_polyfill(checked_module const& m,
                                                         flat_entry_point const& e,
                                                         type_id corner);

} // namespace sgl::check
