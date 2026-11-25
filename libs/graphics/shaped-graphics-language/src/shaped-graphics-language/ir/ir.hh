#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/check/ids.hh>
#include <shaped-graphics-language/check/symbols.hh>
#include <shaped-graphics-language/fwd.hh>

/// One entry point below the flat tree: typed operations in structured blocks, which each native target writes out.
///
/// It stands between legalize and a binary target, so a SPIR-V writer and a later DXIL writer read the same thing, and
/// the optimizing passes, disassembly and simulation have one place to look.
/// Everything is a flat table indexed by a small id, filled once per compile: an operation is 24 bytes, and its
/// operands live in one array beside it, so no node is allocated on its own.
///
/// What a target needs and text never said is explicit here: a pointer, a load, a merge block.
/// The float guarantee stands on each operation rather than on the entry point, so a guarantee can be narrower than a
/// function without a change to this form.

enum class sgl::ir::type_ref : sgl::u32
{
    none = 0xffffffffu
};
/// A value of one of four tables, told apart by its top bits: an operation, a constant, a global or a local variable.
enum class sgl::ir::value_ref : sgl::u32
{
    none = 0xffffffffu
};
enum class sgl::ir::block_ref : sgl::u32
{
    none = 0xffffffffu
};

namespace sgl::ir
{

inline constexpr u32 k_constant_bit = 0x80000000u;
inline constexpr u32 k_global_bit = 0x40000000u;
inline constexpr u32 k_local_bit = 0x20000000u;
inline constexpr u32 k_index_mask = 0x1fffffffu;

[[nodiscard]] constexpr bool is_valid(type_ref t)
{
    return t != type_ref::none;
}
[[nodiscard]] constexpr bool is_valid(value_ref v)
{
    return v != value_ref::none;
}
[[nodiscard]] constexpr bool is_valid(block_ref b)
{
    return b != block_ref::none;
}
[[nodiscard]] constexpr bool is_constant(value_ref v)
{
    return is_valid(v) && (u32(v) & k_constant_bit) != 0;
}
[[nodiscard]] constexpr bool is_global(value_ref v)
{
    return is_valid(v) && (u32(v) & k_constant_bit) == 0 && (u32(v) & k_global_bit) != 0;
}
[[nodiscard]] constexpr bool is_local(value_ref v)
{
    return is_valid(v) && (u32(v) & (k_constant_bit | k_global_bit)) == 0 && (u32(v) & k_local_bit) != 0;
}
/// A value an instruction gives, whichever it is that gives it.
[[nodiscard]] constexpr bool is_operation(value_ref v)
{
    return is_valid(v) && (u32(v) & (k_constant_bit | k_global_bit | k_local_bit)) == 0;
}
[[nodiscard]] constexpr u32 index_of(value_ref v)
{
    return u32(v) & k_index_mask;
}
[[nodiscard]] constexpr value_ref constant_at(u32 i)
{
    return value_ref(i | k_constant_bit);
}
[[nodiscard]] constexpr value_ref global_at(u32 i)
{
    return value_ref(i | k_global_bit);
}
[[nodiscard]] constexpr value_ref local_at(u32 i)
{
    return value_ref(i | k_local_bit);
}

} // namespace sgl::ir

enum class sgl::ir::type_kind : sgl::u8
{
    void_,
    bool_,
    int_,
    float_,
    vector,
    matrix,
    array,
    runtime_array,
    structure,
    pointer,
    image,
    sampler,
    sampled_image,
    /// What a trace runs against, which only a resource holds.
    acceleration_structure,
    /// One trace in flight, which `op::ray_query_begin` makes and no store or phi ever moves.
    ray_query,
    /// A trace whose shaders have not run, which only `op::hit_object_trace` makes.
    /// SPIR-V forbids moving one, so `ir::verify` refuses a phi, a select, a store or a member of it (EMIT-246).
    hit_object,
};

enum class sgl::ir::storage : sgl::u8
{
    function,
    workgroup,
    uniform,
    push_constant,
    storage_buffer,
    /// A texture, an image or a sampler.
    resource,
    /// What a task stage hands its mesh workgroups: one per entry point, which the mesh stage reads.
    task_payload,
    /// The payload a ray-tracing stage is handed, or a callable's parameter: the caller's, written back on return.
    ray_input,
    /// What an intersection reported, which a hit stage reads; a triangle's barycentrics.
    hit_attribute,
    /// What a trace hands over and reads back: one global per type.
    ray_output,
    /// What a call of a callable hands over and reads back: one global per type.
    callable_output,
    /// One texel of a storage image, which only `op::texel_pointer` reaches.
    image,
    /// A table the program states in full, which every invocation reads and none writes: a global with an initializer.
    table,
};

/// One member of a structure; `offset` and `matrix_stride` mean something where it stands in GPU memory.
struct sgl::ir::member
{
    type_ref type = type_ref::none;
    /// -1 for a structure that is no memory.
    i32 offset = -1;
    /// The bytes between two columns of a matrix, the innermost element's for an array of them; 0 for no matrix.
    i32 matrix_stride = 0;
    /// A range of `program::names`.
    u32 name_first = 0;
    u32 name_size = 0;
};

struct sgl::ir::type
{
    type_kind kind = type_kind::void_;
    /// In bits, of an `int_` or a `float_`.
    u8 width = 0;
    bool is_signed = false;
    /// A structure that is a binding's outermost one.
    bool is_block = false;
    storage space = storage::function;
    /// An image's dimension: 0 to 3 for 1D, 2D, 3D and cube.
    u8 dimension = 0;
    bool is_arrayed = false;
    bool is_multisampled = false;
    /// An image the shader stores to or reads as texels, rather than a sampled one.
    bool is_storage_image = false;
    bool is_depth = false;
    /// A storage image's position in `check::k_image_formats`.
    i16 format = -1;
    /// A vector's lanes, a matrix's columns, an array's elements.
    u32 count = 0;
    /// A vector's scalar, a matrix's column, an array's element, a pointer's pointee, an image's sampled scalar.
    type_ref element = type_ref::none;
    /// An array's bytes per element where it stands in GPU memory, and 0 elsewhere.
    i32 stride = 0;
    /// A structure's: a range of `program::members`.
    u32 first_member = 0;
    u32 member_count = 0;
    /// A structure's name, a range of `program::names`.
    u32 name_first = 0;
    u32 name_size = 0;
    /// Named by a debug variable alone, which a writer describes and declares no type for (EMIT-267): a split let's
    /// whole, which no operation holds.
    bool is_debug_only = false;
};

/// A math function, whose name every target knows differently.
enum class sgl::ir::math : sgl::u8
{
    round_even,
    trunc,
    fabs,
    sabs,
    fsign,
    ssign,
    floor,
    ceil,
    fract,
    sin,
    cos,
    tan,
    asin,
    acos,
    atan,
    sinh,
    cosh,
    tanh,
    atan2,
    pow,
    exp,
    log,
    exp2,
    log2,
    sqrt,
    inverse_sqrt,
    determinant,
    fmin,
    umin,
    smin,
    fmax,
    umax,
    smax,
    fclamp,
    uclamp,
    sclamp,
    fmix,
    step,
    smooth_step,
    fma,
    pack_snorm4x8,
    pack_unorm4x8,
    pack_half2x16,
    unpack_half2x16,
    unpack_snorm4x8,
    unpack_unorm4x8,
    length,
    distance,
    cross,
    normalize,
    reflect,
    refract,
    find_ilsb,
    find_smsb,
    find_umsb,
};

/// What a subgroup operation does; the `sub` of `op::subgroup`.
enum class sgl::ir::subgroup : sgl::u8
{
    all,
    any,
    broadcast,
    broadcast_first,
    ballot,
    shuffle,
    shuffle_xor,
    shuffle_up,
    shuffle_down,
    iadd,
    fadd,
    imul,
    fmul,
    smin,
    umin,
    fmin,
    smax,
    umax,
    fmax,
    bit_and,
    bit_or,
    bit_xor,
    quad_broadcast,
    quad_swap,
    quad_any,
    quad_all,
    elect,
};

/// How a subgroup reduction folds: over the subgroup, up to and including this invocation, or up to it alone.
enum class sgl::ir::group_fold : sgl::u8
{
    none,
    reduce,
    inclusive,
    exclusive,
};

/// What a derivative across a pixel quad gives; the `sub` of `op::derivative`.
/// `x`, `y` and `width` are the coarse ones, the same in every lane of the quad (EVAL-144).
enum class sgl::ir::derivative : sgl::u8
{
    x,
    y,
    /// `abs(x) + abs(y)`.
    width,
    /// Along the lane's own row of the quad.
    x_fine,
    /// Down the lane's own column of the quad.
    y_fine,
};

enum class sgl::ir::barrier : sgl::u8
{
    workgroup,
    storage,
    texture,
};

enum class sgl::ir::op : sgl::u16
{
    // ---- arithmetic: operands of one shape, and `guarantee` applies to the float ones
    fadd,
    fsub,
    fmul,
    fdiv,
    frem,
    fneg,
    iadd,
    isub,
    imul,
    sdiv,
    udiv,
    srem,
    urem,
    ineg,
    bit_and,
    bit_or,
    bit_xor,
    bit_not,
    bit_reverse,
    bit_count,
    shift_left,
    shift_right_logical,
    shift_right_arithmetic,
    // ---- comparisons
    feq,
    /// True for a NaN on either side.
    fne,
    flt,
    fle,
    fgt,
    fge,
    is_nan,
    is_inf,
    ieq,
    ine,
    slt,
    sle,
    sgt,
    sge,
    ult,
    ule,
    ugt,
    uge,
    logical_and,
    logical_or,
    logical_not,
    logical_eq,
    logical_ne,
    any,
    all,
    select,
    // ---- conversions
    f_to_s,
    f_to_u,
    s_to_f,
    u_to_f,
    f_resize,
    s_resize,
    u_resize,
    bitcast,
    /// One composite to another of the same shape in another layout, as reading GPU memory needs.
    copy_logical,
    // ---- linear algebra
    dot,
    vector_times_scalar,
    matrix_times_scalar,
    matrix_times_vector,
    matrix_times_matrix,
    transpose,
    /// `sub` is a `math`.
    math,
    /// `sub` is a `derivative`.
    derivative,
    // ---- composites
    construct,
    /// Operands: the composite, then literal indices.
    extract,
    /// Operands: the part, the composite, then literal indices.
    insert,
    /// Operands: two vectors, then literal lanes.
    shuffle,
    // ---- memory
    load,
    /// Operands: pointer, value.
    store,
    access_chain,
    /// Operands: the image's pointer, the coordinate, the sample.
    /// A texel as a place: a pointer of `storage::image` to the image's scalar, which an atomic takes.
    texel_pointer,
    /// Operands: the block's pointer, the position of its runtime array as a literal.
    array_length,
    /// Operands: a position in `program::stage_elements` as a literal, then an index where its `array_size` is set.
    /// A pure value, so `construct_ssa` has nothing to promote and a dead one disappears.
    stage_load,
    /// Operands: a position in `program::stage_elements` as a literal, the value, then an index likewise.
    stage_store,
    /// Operands: a position in `program::debug_variables` as a literal, the value, then the variable's first leaf it
    /// gives as a literal (EMIT-267).
    /// From here on the variable holds the value, at its leaves from that one; a value of `none` is optimized out.
    /// Nothing reads it and its value is no use, `operand::debug`, so no other operation changes for it.
    debug_value,
    /// Operands: a position in `program::debug_variables` as a literal, then the local that holds the variable whole.
    /// The local is no use either, so `construct_ssa` promotes it as it would without, and turns this into values.
    debug_declare,
    // ---- atomics: operands are the pointer, then the values
    atomic_load,
    atomic_store,
    atomic_exchange,
    /// Operands: pointer, value, comparator.
    atomic_compare_exchange,
    atomic_iadd,
    atomic_isub,
    atomic_smin,
    atomic_umin,
    atomic_smax,
    atomic_umax,
    atomic_and,
    atomic_or,
    atomic_xor,
    atomic_fadd,
    /// `sub` is a `barrier`.
    control_barrier,
    /// A pixel is lost, and the invocation goes on as a helper, so a neighbour's derivatives stay whole: no terminator.
    discard,
    /// A geometry stage appends the output elements as they stand, which leaves them undefined.
    emit_vertex,
    /// A geometry stage ends the strip it is appending to.
    end_strip,
    /// Operands: vertices, primitives.
    set_mesh_counts,
    /// Operands: the primitive, then the `uint3` or `uint2` of its vertices.
    write_primitive,
    // ---- ray-tracing stages
    /// Operands: structure, flags, mask, contribution, multiplier, miss, origin, t_min, direction, t_max, then the
    /// global of `storage::ray_output` the payload stands in.
    trace_ray,
    /// Operands: the index, then the global of `storage::callable_output` the parameter stands in.
    execute_callable,
    /// Operands: t, kind, then the global of `storage::hit_attribute` the attributes stand in.
    report_hit,
    /// Operands: as `trace_ray`; gives the hit object, whose shaders have not run.
    hit_object_trace,
    /// Operands: as `trace_ray`, then a hint and its bits where the call gives them.
    /// A trace regrouped by its hit before its shaders run, which SPIR-V says in one instruction.
    trace_reorder_execute,
    /// Operands: hit object, then a hint and its bits where the call gives them.
    hit_object_reorder,
    /// Operands: a hint and its bits; regroups by nothing but them.
    reorder_thread,
    /// Operands: hit object, then the global of `storage::ray_output` the payload stands in.
    hit_object_invoke,
    /// Operands: a `uint2` of a structure's device address, low word first; gives the structure, which a trace takes.
    /// SPIR-V's alone: vulkan traces a structure by its address (libs/graphics/shaped-graphics-language/docs/reserved-bindings.md).
    acceleration_from_address,
    // ---- ray queries: every step changes what the query answers, so none moves past another
    /// Operands: the flags every trace of this query states, a constant.
    ray_query_begin,
    /// Operands: query, structure, origin, direction, t_min, t_max, flags, mask.
    ray_query_trace,
    /// Operands: query; gives whether a candidate waits to be decided.
    ray_query_proceed,
    /// Operands: query.
    ray_query_commit_triangle,
    /// Operands: query, t.
    ray_query_commit_procedural,
    /// Operands: query.
    ray_query_abort,
    /// Operands: query; `sub` is a `ray_query_value`, and `aux` 1 reads the committed hit rather than the candidate.
    /// A dead one goes, but none moves or merges: a read is valid only in the state the query is in where it stands.
    ray_query_read,
    /// Operands: hit object; `sub` is a `hit_object_value`.
    hit_object_read,
    // ---- textures and images
    combine_sampler,
    /// Operands: sampled image, coordinate, level, then an offset as `sample_implicit` takes one.
    sample_lod,
    /// Operands: sampled image, coordinate, then an offset where the call states one.
    /// A pixel stage's alone, whose level comes from its derivatives.
    /// An offset is a `construct` of integer constants, or one constant for a 1D texture (CHK-280).
    sample_implicit,
    /// Operands: sampled image, coordinate, bias.
    sample_bias,
    /// Operands: sampled image, coordinate, the two gradients.
    sample_grad,
    /// Operands: sampled image, coordinate, reference; a pixel stage's alone.
    sample_compare,
    /// Operands: sampled image, coordinate, reference, a float zero: at level 0.
    sample_compare_zero,
    /// Operands: image, coordinate, level, and a sample where `aux` is 1.
    image_fetch,
    /// Operands: sampled image, coordinate, component, then an offset as `sample_implicit` takes one.
    image_gather,
    /// Operands: sampled image, coordinate, reference.
    image_gather_compare,
    /// Operands: sampled image, coordinate, without a layer; gives a float2 of the clamped level and the unclamped one.
    /// A pixel stage's alone, as `sample_implicit` is.
    image_query_lod,
    image_read,
    /// Operands: image, coordinate, texel.
    image_write,
    /// Operands: image, and a level where `aux` is 1.
    image_size,
    image_levels,
    image_samples,
    /// `sub` is a `subgroup`, `aux` a `group_fold`.
    subgroup,
    /// Operands: value and block, as pairs.
    phi,
    // ---- terminators: a block ends in exactly one
    branch,
    /// Operands: condition, true block, false block.
    branch_conditional,
    /// Operands: selector, default block, then literal and block as pairs.
    switch_,
    /// Operands: the three counts, then the global a task stage stored its payload in; it ends the stage on both targets.
    dispatch_mesh,
    /// An any hit turns the candidate down, which ends the stage.
    ignore_hit,
    /// An any hit accepts the candidate and ends the search, which ends the stage.
    accept_and_end_search,
    return_,
    unreachable,
};

/// The `aux` of an `access_chain` into a binding array, and of the `load` and `combine_sampler` it reaches: the
/// index differs between invocations, which each target has to be told.
inline constexpr sgl::u8 k_ir_nonuniform = 1;
/// The `sub` of a narrowing `f_resize` a driver must not fold with the conversion back, EMIT-282's.
inline constexpr sgl::u8 k_ir_kept_rounding = 1;

/// One operation; its position is the value it gives, where it gives one.
struct sgl::ir::instruction
{
    op code = op::unreachable;
    /// What `code` says of it: a `math`, a `subgroup`, a `barrier`.
    u8 sub = 0;
    /// A `group_fold`, or that an optional operand stands.
    u8 aux = 0;
    /// The float guarantee this operation keeps; never weaker than the entry point's today.
    check::float_level guarantee = check::float_level::fast;
    type_ref type = type_ref::none;
    /// A range of `program::operands`.
    u32 first = 0;
    u32 count = 0;
    /// The statement it was lowered from, a position in the flat entry point's `stmts`, which keeps its `.sgl` line.
    u32 statement = 0xffffffffu;
};

/// What a `ray_query_read` asks.
enum class sgl::ir::ray_query_value : sgl::u8
{
    /// 0 for nothing, 1 for a triangle, 2 for a procedural primitive: committed only.
    committed_kind,
    /// 0 for a triangle, 1 for a procedural primitive: candidate only.
    candidate_kind,
    /// Whether the candidate's box is opaque.
    candidate_box_is_opaque,
    t,
    instance_id,
    instance_index,
    geometry_index,
    primitive_index,
    barycentrics,
    is_front_face,
    /// Four columns of three lanes, as SPIR-V hands it over.
    object_to_world,
    world_to_object,
    object_ray_origin,
    object_ray_direction,
    /// The cluster the hit is in, where the device has clustered geometry.
    cluster_id,
};

/// What a `hit_object_read` asks.
enum class sgl::ir::hit_object_value : sgl::u8
{
    is_hit,
    is_miss,
    t,
    instance_id,
    instance_index,
    geometry_index,
    primitive_index,
};

enum class sgl::ir::merge_kind : sgl::u8
{
    none,
    selection,
    loop,
};

struct sgl::ir::block
{
    /// A range of `program::instructions`, whose last one is the terminator.
    u32 first = 0;
    u32 count = 0;
    merge_kind merges = merge_kind::none;
    /// Where its construct ends, and a loop's continue target.
    block_ref merge = block_ref::none;
    block_ref continue_to = block_ref::none;
};

/// A scalar constant, its bits low word first, or zero or nothing of any type.
struct sgl::ir::constant
{
    type_ref type = type_ref::none;
    u32 low = 0;
    u32 high = 0;
    bool is_null = false;
    bool is_undefined = false;
};

/// What a global is to a target that names each kind of binding apart, which its pointer's type does not say.
enum class sgl::ir::global_kind : sgl::u8
{
    /// Workgroup memory.
    plain,
    /// A group's block of constants.
    constants,
    /// The entry point's own constants, which no group holds.
    inline_constants,
    /// A buffer of elements.
    buffer,
    /// A buffer addressed by byte.
    bytes,
    texture,
    image,
    sampler,
    /// A sampler the file declares, which no group holds.
    file_sampler,
    acceleration_structure,
};

/// A value the stage hands an entry point, which each target names its own way.
enum class sgl::ir::built_in : sgl::u8
{
    none,
    thread_id,
    local_thread_id,
    local_thread_index,
    workgroup_id,
    subgroup_size,
    subgroup_invocation_id,
    position,
    /// `@layer`.
    layer,
    /// `@viewport_index`.
    viewport_index,
    vertex_index,
    instance_index,
    first_instance,
    is_front_facing,
    sample_index,
    sample_mask,
    primitive_id,
    view_index,
    domain_location,
    launch_id,
    launch_size,
    world_ray_origin,
    world_ray_direction,
    object_ray_origin,
    object_ray_direction,
    ray_t_min,
    ray_t_current,
    /// The id the host gave the instance.
    ray_instance_id,
    /// The instance's position in its TLAS.
    ray_instance_index,
    ray_geometry_index,
    ray_primitive_index,
    hit_kind,
    /// Four columns of three lanes.
    object_to_world,
    world_to_object,
    /// The cluster a hit is in, where the device has clustered geometry.
    ray_cluster_id,
    depth,
    depth_greater_equal,
    depth_less_equal,
    /// A patch's edge and inside tessellation factors, one element per factor.
    tess_edge,
    tess_inside,
};

/// Which edge of an entry point an element crosses.
enum class sgl::ir::stage_part : sgl::u8
{
    input,
    output,
    /// A tessellation stage's data per patch.
    patch,
    /// What a mesh stage writes per primitive, and a pixel stage reads of it.
    primitive,
};

/// How a value crosses from the stage before; only a pixel stage's input does anything with it.
enum class sgl::ir::interpolation : sgl::u8
{
    /// What no stage interpolates: a vertex input, a render target, a compute stage's input.
    none,
    perspective,
    centroid,
    sample,
    noperspective,
    noperspective_centroid,
    noperspective_sample,
    flat,
};

/// One thing that crosses the entry point's edge, a vector of up to four lanes at most.
///
/// It is target-neutral: it says what crosses and how, SPIR-V reads `location`, and DXIL reads the semantic.
/// `program::stage_elements` holds one per declared piece, in declared order, which is the rule DXIL's packer reads.
struct sgl::ir::stage_element
{
    stage_part part = stage_part::input;
    /// `none` for a link, a vertex attribute or a render target.
    built_in system = built_in::none;
    /// A scalar or a vector, never a matrix, an array or a struct.
    type_ref type = type_ref::none;
    interpolation interpolate = interpolation::none;
    /// -1 for a system element.
    i32 location = -1;
    /// Per control point, per vertex or per primitive; 0 for one value.
    u32 array_size = 0;
    /// HLSL's semantic, which a DXIL signature names it by, parted from its number: `SGL` and 10.
    u32 semantic_first = 0;
    u32 semantic_size = 0;
    u32 semantic_index = 0;
    /// A range of `program::names`, for a reader.
    u32 name_first = 0;
    u32 name_size = 0;
};

/// What a tessellator cuts, which a control stage's factors struct says by its edges (CHK-305).
enum class sgl::ir::tessellation_domain : sgl::u8
{
    none,
    isoline,
    triangle,
    quad,
};

/// What a geometry, a tessellation or a mesh stage states of its entry point beyond its elements.
///
/// A control stage's `program` is what HLSL calls its patch-constant function: the body runs once per patch.
/// Each writer generates the part that hands each control point on, from the elements of `stage_part::output`.
struct sgl::ir::primitive_stage
{
    /// The vertices of an input primitive, or the control points of a patch.
    u32 input_vertices = 0;
    /// `@geometry(max_vertices = N)`'s `N`, or what a mesh workgroup writes at most.
    u32 max_vertices = 0;
    /// A mesh stage's: what a workgroup writes at most, and whether those are lines rather than triangles.
    u32 max_primitives = 0;
    bool writes_lines = false;
    /// A mesh stage's `uint3` or `uint2` of a primitive's vertices, which SPIR-V declares an array of.
    type_ref primitive_indices = type_ref::none;
    /// The vertices of a primitive a geometry stage's stream appends: 1, 2 or 3.
    u32 output_vertices = 0;
    tessellation_domain domain = tessellation_domain::none;
    check::tessellation_partitioning partitioning = check::tessellation_partitioning::integer;
    /// In the domain's own orientation, which is both targets' and mirrors SGL's `winding` (EMIT-134).
    bool is_output_clockwise = false;
    /// A control stage's `max_factor`, which its factors are capped at already.
    u32 max_factor = 64;
    /// A control stage's body as a function of its own, a range of `program::names`.
    u32 body_name_first = 0;
    u32 body_name_size = 0;
};

/// A variable outside the function: a resource, a block or workgroup memory.
struct sgl::ir::global
{
    /// The pointer's type.
    type_ref type = type_ref::none;
    global_kind kind = global_kind::plain;
    /// A descriptor's address, -1 for none.
    i32 set = -1;
    i32 binding = -1;
    bool is_read_only = false;
    bool is_write_only = false;
    bool is_coherent = false;
    /// A sampler that compares, which DXIL states and SPIR-V does not.
    bool is_comparison = false;
    u32 name_first = 0;
    u32 name_size = 0;
    /// A `storage::table`'s value: a range of `program::initializers`, one scalar constant per leaf, depth first.
    u32 initializer_first = 0;
    u32 initializer_count = 0;
};

/// A variable of the function.
struct sgl::ir::local
{
    /// The pointer's type.
    type_ref type = type_ref::none;
    u32 name_first = 0;
    u32 name_size = 0;
};

/// Where one statement came from (EMIT-257): a position in `line_table::files`, the line there, and its calls.
struct sgl::ir::source_place
{
    /// -1 where the statement names no line.
    i32 file = -1;
    i32 line = 0;
    /// A position in `line_table::functions`: the function whose body the line stands in; -1 where it is not known.
    i32 function = -1;
    /// A range of `line_table::calls`: the calls it was inlined through, innermost first.
    u32 calls_first = 0;
    u32 calls_count = 0;
    /// A position in `line_table::blocks`: the innermost one of its function it stands in; -1 for the function's body.
    i32 block = -1;
};

/// A body the source nests in a function's, an `if` arm's or a loop's, which scopes the variables declared in it
/// (EMIT-267).
struct sgl::ir::lexical_block
{
    /// A position in `line_table::blocks`, ahead of this one; -1 where it stands in its function's body.
    i32 parent = -1;
    /// Where the statement that opens it stands.
    i32 file = -1;
    i32 line = 0;
};

/// A local the source wrote, as a debugger lists it (EMIT-267).
struct sgl::ir::debug_variable
{
    /// A range of `program::names`: the name as written.
    u32 name_first = 0;
    u32 name_size = 0;
    /// The type of its value, which a debug value gives whole or from a leaf on.
    type_ref type = type_ref::none;
    /// The statement that declares it, a position in the flat entry point's `stmts`: its line and the calls it was
    /// inlined through name its scope.
    u32 statement = 0xffffffffu;
    /// An entry point's parameter, which a debugger lists apart from its locals.
    bool is_parameter = false;
    /// A `var` or a `for` counter, whose value depends on the path taken; a `let` or a parameter has one value.
    bool is_mutable = false;
};

/// What a writer states of the `.sgl` lines behind its operations; empty unless the compile asked for it (EMIT-257).
struct sgl::ir::line_table
{
    /// What each file is called, and what it holds.
    cc::vector<cc::string> names;
    cc::vector<cc::string> texts;
    /// Parallel to the flat entry point's statements, which an instruction's `statement` indexes.
    cc::vector<source_place> statements;
    /// The call sites of every place, each a place of its own with no calls.
    cc::vector<source_place> calls;
    /// The name of each function a place stands in, each once (EMIT-267).
    cc::vector<cc::string> functions;
    /// Each nested body a place stands in, its parent ahead of it (EMIT-267).
    cc::vector<lexical_block> blocks;

    [[nodiscard]] bool is_empty() const { return statements.empty(); }
    /// The place of statement `s`, or one with no file.
    [[nodiscard]] source_place place_of(u32 s) const
    {
        return s < u32(statements.size()) ? statements[isize(s)] : source_place();
    }
};

/// A ray payload of the entry point, and what each of its fields' shaders do with it (EMIT-137).
/// Each mask is DXIL's: four bits a stage, the caller, closesthit, miss and anyhit, 1 read and 2 written.
struct sgl::ir::payload_annotation
{
    type_ref type = type_ref::none;
    cc::vector<u32> field_masks;
};

/// One entry point, whole.
struct sgl::ir::program
{
    cc::vector<type> types;
    cc::vector<member> members;
    cc::vector<constant> constants;
    cc::vector<global> globals;
    cc::vector<local> locals;
    cc::vector<stage_element> stage_elements;
    cc::vector<instruction> instructions;
    cc::vector<u32> operands;
    /// The leaves of every table's value, each a scalar constant.
    cc::vector<u32> initializers;
    /// In the order a target writes them, each dominated by the ones before it; the first is the entry.
    cc::vector<block> blocks;
    /// Every name a table holds a range of.
    cc::string names;

    cc::string entry_name;
    check::stage stage = check::stage::none;
    i32 workgroup[3] = {1, 1, 1};
    primitive_stage primitive;
    /// EMIT-184: the device has opacity micromaps, so a trace sees them, which each target states.
    bool sees_micromaps = false;
    /// EMIT-265: DXIL writes the hit objects through NVAPI's shader extensions, the `nvapi` fill of shader execution
    /// reordering, rather than as Shader Model 6.9's own.
    bool reorders_through_nvapi = false;
    /// EMIT-128: DXIL reads the draw's start from the block `sgl_draw_start` at `b1, space9` the host writes (EMIT-271),
    /// the `root_constant` fill of `draw_start`, rather than through Shader Model 6.8's operations.
    bool draw_start_in_root_constants = false;
    /// CHK-561: SPIR-V states `MaximallyReconvergesKHR`, `flat_entry_point::maximally_reconverges`; DXIL has no such mode.
    bool maximally_reconverges = false;
    /// The entry point's guarantee: what no single operation can say, its denormals above all.
    check::float_level guarantee = check::float_level::fast;
    /// The `.sgl` lines each writer states beside its operations, where the compile asked for them.
    line_table lines;
    /// The source variables a debugger lists, which `debug_value` and `debug_declare` name; filled for `full` alone (EMIT-267).
    cc::vector<debug_variable> debug_variables;
    /// The compile asked for `full`: lowering describes each local the source wrote.
    bool describes_variables = false;
    /// The payloads it is handed or traces with, each once; DXIL states them, as DXC does for its `[raypayload]`s.
    cc::vector<payload_annotation> payloads;

    [[nodiscard]] cc::string_view name_at(u32 first, u32 size) const
    {
        return cc::string_view(names).subview({.offset = isize(first), .size = isize(size)});
    }
    /// A type that is none reads as a `void`, so a walk that already failed can run to the end of what it was doing.
    [[nodiscard]] type const& at(type_ref t) const { return types[is_valid(t) ? isize(t) : 0]; }
    [[nodiscard]] cc::span<u32 const> operands_of(instruction const& i) const
    {
        return cc::span<u32 const>(operands.data() + i.first, isize(i.count));
    }
    /// The type of `v`, whichever table it stands in.
    [[nodiscard]] type_ref type_of(value_ref v) const
    {
        if (!is_valid(v))
            return type_ref::none;
        if (is_constant(v))
            return constants[index_of(v)].type;
        if (is_global(v))
            return globals[index_of(v)].type;
        if (is_local(v))
            return locals[index_of(v)].type;
        return instructions[index_of(v)].type;
    }
    /// The scalars a value of `t` is made of, which a debug value counts its leaves in (EMIT-267).
    [[nodiscard]] u32 leaves_of(type_ref t) const;
};

namespace sgl::ir
{
/// The name of `code`, for a reader.
[[nodiscard]] cc::string_view op_name(op code);

/// `t` as `dump` spells it, for a reader.
[[nodiscard]] cc::string type_name(program const& p, type_ref t);

} // namespace sgl::ir

/// What one position of an operation's operands holds.
enum class sgl::ir::operand : sgl::u8
{
    value,
    block,
    literal,
    /// A value or a local a debug operation names (EMIT-267): no pass counts it as read, and every rewrite follows it.
    debug,
};

namespace sgl::ir
{

/// What position `at` of an operation of `code` holds, so a pass can rewrite operands it does not otherwise know.
[[nodiscard]] operand operand_role(op code, isize at);

/// Whether an operand of `role` names a value: what a rewrite renumbers or replaces, where a use count reads `value` alone.
[[nodiscard]] constexpr bool names_a_value(operand role)
{
    return role == operand::value || role == operand::debug;
}

/// Whether `code` describes a variable for a debugger and computes nothing (EMIT-267).
[[nodiscard]] constexpr bool is_debug(op code)
{
    return code == op::debug_value || code == op::debug_declare;
}

/// Whether running `code` does something other than give a value, so an unread one still has to run.
[[nodiscard]] bool has_effect(op code);

/// Whether `code` depends on nothing but its operands, so two of the same operands are one value.
/// A load is not, though `share_common_values` proves some are.
[[nodiscard]] bool gives_only_a_value(op code);

/// Whether `code` may run on a path that did not ask for it: it gives only a value, and no operand makes it trap.
[[nodiscard]] bool may_speculate(op code);

/// The blocks a terminator reaches, which `scratch` holds; one block may stand twice.
[[nodiscard]] cc::span<u32 const> successors_of(program const& p, instruction const& terminator, cc::vector<u32>& scratch);

/// Every block by where it opened: a block's instructions are one run, so their position is that order.
[[nodiscard]] cc::vector<block_ref> blocks_in_order(program const& p);

/// The program as text, one operation a line, for a reader and for a test to compare.
[[nodiscard]] cc::string dump(program const& p);
} // namespace sgl::ir
