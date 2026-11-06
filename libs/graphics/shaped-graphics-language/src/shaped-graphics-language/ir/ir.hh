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
    input,
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
};

/// How a subgroup reduction folds: over the subgroup, up to and including this invocation, or up to it alone.
enum class sgl::ir::group_fold : sgl::u8
{
    none,
    reduce,
    inclusive,
    exclusive,
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
    /// Operands: the block's pointer, the position of its runtime array as a literal.
    array_length,
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
    // ---- textures and images
    combine_sampler,
    /// Operands: sampled image, coordinate, level, and an offset where `aux` is 1.
    sample_lod,
    /// Operands: image, coordinate, level, and a sample where `aux` is 1.
    image_fetch,
    /// Operands: sampled image, coordinate, component.
    image_gather,
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
    return_,
    unreachable,
};

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

/// A variable outside the function: a resource, a block, workgroup memory or a stage input.
struct sgl::ir::global
{
    /// The pointer's type.
    type_ref type = type_ref::none;
    /// A descriptor's address, -1 for none.
    i32 set = -1;
    i32 binding = -1;
    /// SPIR-V's `BuiltIn` of a stage input, -1 for none.
    i32 built_in = -1;
    bool is_read_only = false;
    bool is_write_only = false;
    bool is_coherent = false;
    u32 name_first = 0;
    u32 name_size = 0;
};

/// A variable of the function.
struct sgl::ir::local
{
    /// The pointer's type.
    type_ref type = type_ref::none;
    u32 name_first = 0;
    u32 name_size = 0;
};

/// One entry point, whole.
struct sgl::ir::program
{
    cc::vector<type> types;
    cc::vector<member> members;
    cc::vector<constant> constants;
    cc::vector<global> globals;
    cc::vector<local> locals;
    cc::vector<instruction> instructions;
    cc::vector<u32> operands;
    /// In the order a target writes them, each dominated by the ones before it; the first is the entry.
    cc::vector<block> blocks;
    /// Every name a table holds a range of.
    cc::string names;

    cc::string entry_name;
    i32 workgroup[3] = {1, 1, 1};
    /// The entry point's guarantee: what no single operation can say, its denormals above all.
    check::float_level guarantee = check::float_level::fast;

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
};

namespace sgl::ir
{
/// The program as text, one operation a line, for a reader and for a test to compare.
[[nodiscard]] cc::string dump(program const& p);
} // namespace sgl::ir
