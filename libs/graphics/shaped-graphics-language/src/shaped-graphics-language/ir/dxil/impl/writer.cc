#include "writer.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/symbols.hh>

using namespace sgl;
using namespace sgl::ir;
using namespace sgl::ir::dxil::impl;

namespace
{
/// How an operation of the form lowers.
enum class rule_kind : u8
{
    /// No lowering yet: `name` says what it is, for the gap the program reports.
    gap,
    /// Nothing to do: the operands' leaves are the result's.
    identity,
    binary,
    compare,
    cast,
    resize_signed,
    resize_unsigned,
    resize_float,
    bitcast,
    negate,
    bit_not,
    dx_lanes,
    any,
    all,
    select,
    linear_algebra,
    math,
    construct,
    extract,
    insert,
    shuffle,
    load,
    store,
    access_chain,
    texel_pointer,
    array_length,
    stage_load,
    stage_store,
    atomic,
    barrier,
    discard,
    stream,
    set_mesh_counts,
    write_primitive,
    derivative,
    texture,
    image_size,
    subgroup,
    ray_query,
    ray_stage,
    phi,
};

struct op_rule
{
    op code = op::unreachable;
    rule_kind kind = rule_kind::gap;
    /// What the kind wants to know: a `binary_op`, a `predicate`, a `cast_op`, a `dx_op`.
    u8 data = 0;
};

// clang-format off
constexpr op_rule k_rules[] = {
    {op::fadd, rule_kind::binary, u8(binary_op::add)},
    {op::fsub, rule_kind::binary, u8(binary_op::sub)},
    {op::fmul, rule_kind::binary, u8(binary_op::mul)},
    {op::fdiv, rule_kind::binary, u8(binary_op::sdiv)},
    {op::frem, rule_kind::binary, u8(binary_op::srem)},
    {op::fneg, rule_kind::negate},
    {op::iadd, rule_kind::binary, u8(binary_op::add)},
    {op::isub, rule_kind::binary, u8(binary_op::sub)},
    {op::imul, rule_kind::binary, u8(binary_op::mul)},
    {op::sdiv, rule_kind::binary, u8(binary_op::sdiv)},
    {op::udiv, rule_kind::binary, u8(binary_op::udiv)},
    {op::srem, rule_kind::binary, u8(binary_op::srem)},
    {op::urem, rule_kind::binary, u8(binary_op::urem)},
    {op::ineg, rule_kind::negate},
    {op::bit_and, rule_kind::binary, u8(binary_op::bit_and)},
    {op::bit_or, rule_kind::binary, u8(binary_op::bit_or)},
    {op::bit_xor, rule_kind::binary, u8(binary_op::bit_xor)},
    {op::bit_not, rule_kind::bit_not},
    {op::bit_reverse, rule_kind::dx_lanes, u8(dx_op::bfrev)},
    {op::bit_count, rule_kind::dx_lanes, u8(dx_op::countbits)},
    {op::shift_left, rule_kind::binary, u8(binary_op::shl)},
    {op::shift_right_logical, rule_kind::binary, u8(binary_op::lshr)},
    {op::shift_right_arithmetic, rule_kind::binary, u8(binary_op::ashr)},
    {op::feq, rule_kind::compare, u8(predicate::f_oeq)},
    {op::fne, rule_kind::compare, u8(predicate::f_une)},
    {op::flt, rule_kind::compare, u8(predicate::f_olt)},
    {op::fle, rule_kind::compare, u8(predicate::f_ole)},
    {op::fgt, rule_kind::compare, u8(predicate::f_ogt)},
    {op::fge, rule_kind::compare, u8(predicate::f_oge)},
    {op::is_nan, rule_kind::dx_lanes, u8(dx_op::is_nan)},
    {op::is_inf, rule_kind::dx_lanes, u8(dx_op::is_inf)},
    {op::ieq, rule_kind::compare, u8(predicate::i_eq)},
    {op::ine, rule_kind::compare, u8(predicate::i_ne)},
    {op::slt, rule_kind::compare, u8(predicate::i_slt)},
    {op::sle, rule_kind::compare, u8(predicate::i_sle)},
    {op::sgt, rule_kind::compare, u8(predicate::i_sgt)},
    {op::sge, rule_kind::compare, u8(predicate::i_sge)},
    {op::ult, rule_kind::compare, u8(predicate::i_ult)},
    {op::ule, rule_kind::compare, u8(predicate::i_ule)},
    {op::ugt, rule_kind::compare, u8(predicate::i_ugt)},
    {op::uge, rule_kind::compare, u8(predicate::i_uge)},
    {op::logical_and, rule_kind::binary, u8(binary_op::bit_and)},
    {op::logical_or, rule_kind::binary, u8(binary_op::bit_or)},
    {op::logical_not, rule_kind::bit_not},
    {op::logical_eq, rule_kind::compare, u8(predicate::i_eq)},
    {op::logical_ne, rule_kind::compare, u8(predicate::i_ne)},
    {op::any, rule_kind::any},
    {op::all, rule_kind::all},
    {op::select, rule_kind::select},
    {op::f_to_s, rule_kind::cast, u8(cast_op::fptosi)},
    {op::f_to_u, rule_kind::cast, u8(cast_op::fptoui)},
    {op::s_to_f, rule_kind::cast, u8(cast_op::sitofp)},
    {op::u_to_f, rule_kind::cast, u8(cast_op::uitofp)},
    {op::f_resize, rule_kind::resize_float},
    {op::s_resize, rule_kind::resize_signed},
    {op::u_resize, rule_kind::resize_unsigned},
    {op::bitcast, rule_kind::bitcast},
    {op::copy_logical, rule_kind::identity},
    {op::dot, rule_kind::linear_algebra},
    {op::vector_times_scalar, rule_kind::linear_algebra},
    {op::matrix_times_scalar, rule_kind::linear_algebra},
    {op::matrix_times_vector, rule_kind::linear_algebra},
    {op::matrix_times_matrix, rule_kind::linear_algebra},
    {op::transpose, rule_kind::linear_algebra},
    {op::math, rule_kind::math},
    {op::derivative, rule_kind::derivative},
    {op::construct, rule_kind::construct},
    {op::extract, rule_kind::extract},
    {op::insert, rule_kind::insert},
    {op::shuffle, rule_kind::shuffle},
    {op::load, rule_kind::load},
    {op::store, rule_kind::store},
    {op::access_chain, rule_kind::access_chain},
    {op::texel_pointer, rule_kind::texel_pointer},
    {op::array_length, rule_kind::array_length},
    {op::stage_load, rule_kind::stage_load},
    {op::stage_store, rule_kind::stage_store},
    {op::atomic_load, rule_kind::atomic},
    {op::atomic_store, rule_kind::atomic},
    {op::atomic_exchange, rule_kind::atomic},
    {op::atomic_compare_exchange, rule_kind::atomic},
    {op::atomic_iadd, rule_kind::atomic},
    {op::atomic_isub, rule_kind::atomic},
    {op::atomic_smin, rule_kind::atomic},
    {op::atomic_umin, rule_kind::atomic},
    {op::atomic_smax, rule_kind::atomic},
    {op::atomic_umax, rule_kind::atomic},
    {op::atomic_and, rule_kind::atomic},
    {op::atomic_or, rule_kind::atomic},
    {op::atomic_xor, rule_kind::atomic},
    {op::control_barrier, rule_kind::barrier},
    {op::discard, rule_kind::discard},
    {op::emit_vertex, rule_kind::stream, u8(dx_op::emit_stream)},
    {op::end_strip, rule_kind::stream, u8(dx_op::cut_stream)},
    {op::set_mesh_counts, rule_kind::set_mesh_counts},
    {op::write_primitive, rule_kind::write_primitive},
    {op::combine_sampler, rule_kind::texture},
    {op::sample_lod, rule_kind::texture},
    {op::sample_implicit, rule_kind::texture},
    {op::sample_bias, rule_kind::texture},
    {op::sample_grad, rule_kind::texture},
    {op::sample_compare, rule_kind::texture},
    {op::sample_compare_zero, rule_kind::texture},
    {op::image_fetch, rule_kind::texture},
    {op::image_gather, rule_kind::texture},
    {op::image_gather_compare, rule_kind::texture},
    {op::image_read, rule_kind::texture},
    {op::image_write, rule_kind::texture},
    {op::image_size, rule_kind::image_size},
    {op::image_levels, rule_kind::image_size},
    {op::image_samples, rule_kind::image_size},
    {op::subgroup, rule_kind::subgroup},
    {op::trace_ray, rule_kind::ray_stage},
    {op::execute_callable, rule_kind::ray_stage},
    {op::report_hit, rule_kind::ray_stage},
    {op::hit_object_trace, rule_kind::ray_stage},
    {op::trace_reorder_execute, rule_kind::ray_stage},
    {op::hit_object_reorder, rule_kind::ray_stage},
    {op::reorder_thread, rule_kind::ray_stage},
    {op::hit_object_invoke, rule_kind::ray_stage},
    {op::hit_object_read, rule_kind::ray_stage},
    {op::ray_query_begin, rule_kind::ray_query},
    {op::ray_query_trace, rule_kind::ray_query},
    {op::ray_query_proceed, rule_kind::ray_query},
    {op::ray_query_commit_triangle, rule_kind::ray_query},
    {op::ray_query_commit_procedural, rule_kind::ray_query},
    {op::ray_query_abort, rule_kind::ray_query},
    {op::ray_query_read, rule_kind::ray_query},
    {op::phi, rule_kind::phi},
};
// clang-format on

/// Indexed by `op`; an operation with no row stays a gap.
[[nodiscard]] constexpr auto make_rule_table()
{
    struct table
    {
        op_rule rows[isize(op::unreachable) + 1] = {};
    } result;
    for (auto const& r : k_rules)
        result.rows[isize(r.code)] = r;
    return result;
}
constexpr auto k_rule_table = make_rule_table();
} // namespace

void writer::unsupported(cc::string_view what)
{
    if (_failure.empty())
        _failure = cc::string(what);
}

void writer::find_reachable()
{
    _reachable.resize_to_filled(_p.blocks.size(), false);
    auto stack = cc::vector<u32>();
    auto scratch = cc::vector<u32>();
    auto const order = blocks_in_order(_p);
    stack.push_back(u32(order[0]));
    _reachable[isize(order[0])] = true;
    while (!stack.empty())
    {
        auto const& block = _p.blocks[stack.back()];
        stack.pop_back();
        for (auto const to : successors_of(_p, _p.instructions[block.first + block.count - 1], scratch))
            if (!_reachable[to])
            {
                _reachable[to] = true;
                stack.push_back(to);
            }
    }
}

void writer::declare_blocks()
{
    _blocks.resize_to_filled(_p.blocks.size(), ll_block::none);
    for (auto const b : blocks_in_order(_p))
        if (is_reachable(b))
            _blocks[isize(b)] = _m.add_block();
}

void writer::run()
{
    _value_first.resize_to_filled(_p.instructions.size(), 0u);
    _value_count.resize_to_filled(_p.instructions.size(), 0u);
    _constant_first.resize_to_filled(_p.constants.size(), 0xffffffffu);
    _constant_count.resize_to_filled(_p.constants.size(), 0u);
    _places.resize_to_filled(_p.instructions.size(), {});
    _values.reserve(_p.instructions.size() * 2);

    auto const void_function = is_library() ? entry_function_type() : _m.function(_m.void_type(), {});
    auto const is_control = _p.stage == check::stage::tessellation_control;
    auto const attributes = entry_attributes();
    if (is_control)
        _body = _m.add_function(_p.name_at(_p.primitive.body_name_first, _p.primitive.body_name_size), void_function,
                                attributes, false);
    _entry = _m.add_function(_p.entry_name, void_function, attributes, false);
    if (!is_control)
        _body = _entry;

    if (!is_library())
        number_signature();
    find_reachable();
    declare_blocks();
    auto const order = blocks_in_order(_p);
    _m.open_block(_blocks[isize(order[0])]);
    declare_memory();
    if (is_library())
        declare_ray_roots();
    declare_handles();
    for (auto const b : order)
    {
        if (!is_reachable(b) || !_failure.empty())
            continue;
        if (b != order[0])
            _m.open_block(_blocks[isize(b)]);
        lower_block(b);
    }
    if (!_failure.empty())
        return;
    _m.set_location(0);
    fill_phis();
    // sharing first, so two loads of one handle read one handle; and again after, for the lanes merging leaves alike
    _m.share_common_values();
    // skipping this hangs Intel's dx12 device on sv's hybrid lighting; issue: bug-sgl-dxil-hybrid-light-hangs-without-load-merging
    _m.merge_buffer_loads(k_dx_ops[isize(dx_op::raw_buffer_load)].number);
    _m.share_common_values();
    _m.remove_unused();
    if (is_control)
        write_hull_function();
    if (!is_library())
        read_usage();
    write_entry_metadata();
    write_debug_info();
}

void writer::lower_block(block_ref b)
{
    auto const& block = _p.blocks[isize(b)];
    _rows.clear();
    for (auto i = block.first; i + 1 < block.first + block.count && _failure.empty(); ++i)
    {
        locate(_p.instructions[i]);
        operation(i);
    }
    if (_failure.empty())
    {
        locate(_p.instructions[block.first + block.count - 1]);
        terminator(_p.instructions[block.first + block.count - 1]);
    }
}

void writer::terminator(instruction const& i)
{
    auto const operands = _p.operands_of(i);
    switch (i.code)
    {
    case op::branch:
        return _m.branch(_blocks[operands[0]]);
    case op::branch_conditional:
        return _m.branch_if(leaf_at(value_ref(operands[0])), _blocks[operands[1]], _blocks[operands[2]]);
    case op::switch_:
    {
        auto cases = cc::vector<u32>::create_with_capacity(operands.size());
        for (auto k = isize(2); k < operands.size(); k += 2)
        {
            cases.push_back(u32(_m.constant_i32(operands[k])));
            cases.push_back(u32(_blocks[operands[k + 1]]));
        }
        return _m.switch_on(leaf_at(value_ref(operands[0])), _blocks[operands[1]], cases);
    }
    case op::dispatch_mesh:
    {
        ll_value const args[]
            = {_m.constant_i32(k_dx_ops[isize(dx_op::dispatch_mesh)].number), leaf_at(value_ref(operands[0])),
               leaf_at(value_ref(operands[1])), leaf_at(value_ref(operands[2])), _payload};
        (void)call(dx_op::dispatch_mesh, _payload_type, args);
        return _m.return_void();
    }
    case op::ignore_hit:
        return end_of_hit(dx_op::ignore_hit);
    case op::accept_and_end_search:
        return end_of_hit(dx_op::accept_hit_and_end_search);
    case op::return_:
        return _m.return_void();
    default:
        return _m.unreachable();
    }
}

void writer::operation(u32 index)
{
    auto const& i = _p.instructions[index];
    auto const& rule = k_rule_table.rows[isize(i.code)];
    switch (rule.kind)
    {
    case rule_kind::gap:
        return unsupported(cc::format("the operation {} on DXIL", op_name(i.code)));
    case rule_kind::identity:
    {
        auto const source = value_ref(_p.operands_of(i)[0]);
        _scratch.clear();
        for (auto l = u32(0); l < count_of(source); ++l)
            _scratch.push_back(leaf_at(source, l));
        return define(index);
    }
    case rule_kind::binary:
        return binary_lanes(index, i, binary_op(rule.data));
    case rule_kind::compare:
        return compare_lanes(index, i, predicate(rule.data));
    case rule_kind::cast:
        return cast_lanes(index, i, cast_op(rule.data));
    case rule_kind::resize_signed:
        return resize_lanes(index, i, true);
    case rule_kind::resize_unsigned:
        return resize_lanes(index, i, false);
    case rule_kind::resize_float:
        return resize_floats(index, i);
    case rule_kind::bitcast:
        return bitcast_lanes(index, i);
    case rule_kind::negate:
        return negate_lanes(index, i);
    case rule_kind::bit_not:
        return not_lanes(index, i);
    case rule_kind::dx_lanes:
        return dx_lanes(index, i, dx_op(rule.data));
    case rule_kind::any:
        return reduce_bools(index, i, false);
    case rule_kind::all:
        return reduce_bools(index, i, true);
    case rule_kind::select:
        return select_lanes(index, i);
    case rule_kind::linear_algebra:
        return linear_algebra(index, i);
    case rule_kind::math:
        return math_op(index, i);
    case rule_kind::construct:
        return construct(index, i);
    case rule_kind::extract:
        return extract(index, i);
    case rule_kind::insert:
        return insert(index, i);
    case rule_kind::shuffle:
        return shuffle(index, i);
    case rule_kind::load:
        return load(index, i);
    case rule_kind::store:
        return store(i);
    case rule_kind::access_chain:
        return access_chain(index, i);
    case rule_kind::texel_pointer:
        return texel_pointer(index, i);
    case rule_kind::array_length:
        return array_length(index, i);
    case rule_kind::stage_load:
        return stage_load(index, i);
    case rule_kind::discard:
        return discard();
    case rule_kind::stream:
    {
        auto const dx = dx_op(rule.data);
        ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(dx)].number), _m.constant_i8(0)};
        (void)call(dx, _m.void_type(), args);
        return;
    }
    case rule_kind::set_mesh_counts:
        return set_mesh_counts(i);
    case rule_kind::write_primitive:
        return write_primitive(i);
    case rule_kind::derivative:
        return derivative_lanes(index, i);
    case rule_kind::stage_store:
        return stage_store(i);
    case rule_kind::atomic:
        return atomic_op(index, i);
    case rule_kind::barrier:
        return barrier_op(i);
    case rule_kind::texture:
        return texture_op(index, i);
    case rule_kind::image_size:
        return image_size(index, i);
    case rule_kind::subgroup:
        return subgroup_op(index, i);
    case rule_kind::ray_query:
        return ray_query_op(index, i);
    case rule_kind::ray_stage:
        return ray_stage_op(index, i);
    case rule_kind::phi:
        return phi(index, i);
    }
}

// ---- values as their leaves

u32 writer::leaves_of(type_ref t)
{
    if (!is_valid(t))
        return 0;
    if (_leaves.empty())
        _leaves.resize_to_filled(_p.types.size(), 0xffffffffu);
    if (auto const known = _leaves[isize(t)]; known != 0xffffffffu)
        return known;
    auto const& info = _p.at(t);
    auto count = u32(0);
    switch (info.kind)
    {
    case type_kind::void_:
    case type_kind::pointer:
    case type_kind::runtime_array:
        break;
    case type_kind::vector:
    case type_kind::matrix:
    case type_kind::array:
        count = leaves_of(info.element) * info.count;
        break;
    case type_kind::structure:
        for (auto m = u32(0); m < info.member_count; ++m)
            count += leaves_of(_p.members[info.first_member + m].type);
        break;
    case type_kind::sampled_image:
        count = 2;
        break;
    default:
        count = 1;
        break;
    }
    _leaves[isize(t)] = count;
    return count;
}

ll_type writer::scalar_type(type_ref t)
{
    auto const& info = _p.at(t);
    switch (info.kind)
    {
    case type_kind::bool_:
        return _m.bool_type();
    case type_kind::int_:
        _uses.int64 = _uses.int64 || info.width == 64;
        _uses.low_precision = _uses.low_precision || info.width == 16;
        return _m.integer(info.width);
    case type_kind::float_:
        _uses.double_ = _uses.double_ || info.width == 64;
        _uses.low_precision = _uses.low_precision || info.width == 16;
        return _m.floating(info.width);
    default:
        return _m.void_type();
    }
}

type_ref writer::scalar_at(type_ref t, u32 leaf)
{
    auto const& info = _p.at(t);
    switch (info.kind)
    {
    case type_kind::vector:
    case type_kind::matrix:
    case type_kind::array:
        return scalar_at(info.element, leaf % leaves_of(info.element));
    case type_kind::structure:
        for (auto m = u32(0); m < info.member_count; ++m)
        {
            auto const held = _p.members[info.first_member + m].type;
            auto const count = leaves_of(held);
            if (leaf < count)
                return scalar_at(held, leaf);
            leaf -= count;
        }
        return type_ref::none;
    default:
        return t;
    }
}

u32 writer::count_of(value_ref v)
{
    if (is_constant(v))
    {
        (void)first_of_constant(index_of(v));
        return _constant_count[index_of(v)];
    }
    return _value_count[index_of(v)];
}

u32 writer::first_of_constant(u32 index)
{
    if (_constant_first[index] != 0xffffffffu)
        return _constant_first[index];
    auto const& c = _p.constants[index];
    auto const count = leaves_of(c.type);
    auto const first = u32(_values.size());
    for (auto l = u32(0); l < count; ++l)
    {
        auto const scalar = scalar_at(c.type, l);
        auto const type = scalar_type(scalar);
        auto const is_float = _m.is_float(type);
        if (c.is_undefined)
            _values.push_back(_m.constant_undef(type));
        else if (c.is_null)
            _values.push_back(is_float ? _m.constant_float(type, 0) : _m.constant_int(type, 0));
        else
        {
            auto const bits = (u64(c.high) << 32) | c.low;
            _values.push_back(is_float ? _m.constant_float(type, bits) : _m.constant_int(type, bits));
        }
    }
    _constant_first[index] = first;
    _constant_count[index] = count;
    return first;
}

ll_value writer::leaf_at(value_ref v, u32 leaf)
{
    if (is_constant(v))
        return _values[first_of_constant(index_of(v)) + leaf];
    return _values[_value_first[index_of(v)] + leaf];
}

void writer::define(u32 index)
{
    _value_first[index] = u32(_values.size());
    _value_count[index] = u32(_scratch.size());
    for (auto const v : _scratch)
        _values.push_back(v);
}

void writer::define_one(u32 index, ll_value leaf)
{
    _value_first[index] = u32(_values.size());
    _value_count[index] = 1;
    _values.push_back(leaf);
}

u8 writer::flags_of(instruction const& i) const
{
    // the level above `fast` keeps what the flags would give away, as `-Gis` does for text
    if (i.guarantee != check::float_level::fast)
        return 0;
    return fp_flags::unsafe_algebra | fp_flags::no_nans | fp_flags::no_infs | fp_flags::no_signed_zeros
         | fp_flags::allow_reciprocal;
}

// ---- operations with one rule per lane

void writer::binary_lanes(u32 index, instruction const& i, binary_op code)
{
    auto const operands = _p.operands_of(i);
    auto const a = value_ref(operands[0]);
    auto const b = value_ref(operands[1]);
    auto const count = count_of(a);
    auto const type = _m.type_of(leaf_at(a));
    auto const is_float = _m.is_float(type);
    auto const flags = is_float ? flags_of(i) : u8(0);
    if (is_float && code == binary_op::sdiv && _m.bits_of(type) == 64)
        _uses.double_extensions = true;
    _scratch.clear();
    for (auto l = u32(0); l < count; ++l)
        _scratch.push_back(_m.binary(code, leaf_at(a, l), leaf_at(b, l), flags));
    define(index);
}

void writer::compare_lanes(u32 index, instruction const& i, predicate p)
{
    auto const operands = _p.operands_of(i);
    auto const a = value_ref(operands[0]);
    auto const b = value_ref(operands[1]);
    auto const count = count_of(a);
    auto const flags = _m.is_float(_m.type_of(leaf_at(a))) ? flags_of(i) : u8(0);
    _scratch.clear();
    for (auto l = u32(0); l < count; ++l)
        _scratch.push_back(_m.compare(p, leaf_at(a, l), leaf_at(b, l), flags));
    define(index);
}

void writer::cast_lanes(u32 index, instruction const& i, cast_op code)
{
    auto const a = value_ref(_p.operands_of(i)[0]);
    auto const to = scalar_type(scalar_at(i.type, 0));
    auto const from = _m.type_of(leaf_at(a));
    if ((_m.is_float(to) && _m.bits_of(to) == 64) || (_m.is_float(from) && _m.bits_of(from) == 64))
        _uses.double_extensions = true;
    _scratch.clear();
    for (auto l = u32(0); l < count_of(a); ++l)
        _scratch.push_back(_m.cast(code, leaf_at(a, l), to));
    define(index);
}

void writer::resize_lanes(u32 index, instruction const& i, bool is_signed)
{
    auto const a = value_ref(_p.operands_of(i)[0]);
    auto const to = scalar_type(scalar_at(i.type, 0));
    auto const widens = _m.bits_of(to) > _m.bits_of(_m.type_of(leaf_at(a)));
    auto const code = !widens ? cast_op::trunc : is_signed ? cast_op::sext : cast_op::zext;
    _scratch.clear();
    for (auto l = u32(0); l < count_of(a); ++l)
        _scratch.push_back(_m.cast(code, leaf_at(a, l), to));
    define(index);
}

void writer::resize_floats(u32 index, instruction const& i)
{
    auto const a = value_ref(_p.operands_of(i)[0]);
    auto const to = scalar_type(scalar_at(i.type, 0));
    auto const widens = _m.bits_of(to) > _m.bits_of(_m.type_of(leaf_at(a)));
    _scratch.clear();
    for (auto l = u32(0); l < count_of(a); ++l)
        _scratch.push_back(_m.cast(widens ? cast_op::fpext : cast_op::fptrunc, leaf_at(a, l), to));
    define(index);
}

void writer::bitcast_lanes(u32 index, instruction const& i)
{
    auto const a = value_ref(_p.operands_of(i)[0]);
    auto const to = scalar_type(scalar_at(i.type, 0));
    _scratch.clear();
    // EMIT-196: a 64-bit value and its two words, which LLVM's `bitcast` cannot cross
    if (leaves_of(i.type) == 1 && count_of(a) == 2)
    {
        auto const low = leaf_at(a, 0);
        auto const high = leaf_at(a, 1);
        if (_m.is_float(to))
        {
            ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(dx_op::make_double)].number), low, high};
            _scratch.push_back(call(dx_op::make_double, to, args));
            return define(index);
        }
        auto const shifted = _m.binary(binary_op::shl, _m.cast(cast_op::zext, high, to), _m.constant_int(to, 32));
        _scratch.push_back(_m.binary(binary_op::bit_or, _m.cast(cast_op::zext, low, to), shifted));
        return define(index);
    }
    if (leaves_of(i.type) == 2 && count_of(a) == 1)
    {
        auto const held = leaf_at(a, 0);
        if (_m.is_float(_m.type_of(held)))
        {
            ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(dx_op::split_double)].number), held};
            auto const words = call(dx_op::split_double, _m.type_of(held), args);
            _scratch.push_back(_m.extract_value(words, 0));
            _scratch.push_back(_m.extract_value(words, 1));
            return define(index);
        }
        auto const shifted = _m.binary(binary_op::lshr, held, _m.constant_int(_m.type_of(held), 32));
        _scratch.push_back(_m.cast(cast_op::trunc, held, to));
        _scratch.push_back(_m.cast(cast_op::trunc, shifted, to));
        return define(index);
    }
    for (auto l = u32(0); l < count_of(a); ++l)
    {
        auto const held = leaf_at(a, l);
        // a signed and an unsigned value are one LLVM type
        _scratch.push_back(_m.type_of(held) == to ? held : _m.cast(cast_op::bitcast, held, to));
    }
    define(index);
}

void writer::negate_lanes(u32 index, instruction const& i)
{
    auto const a = value_ref(_p.operands_of(i)[0]);
    auto const type = _m.type_of(leaf_at(a));
    auto const is_float = _m.is_float(type);
    auto const bits = _m.bits_of(type);
    // LLVM 3.7 has no negation: a float's is -0.0 - x, which keeps the sign of a zero
    auto const zero = is_float ? _m.constant_float(type, negative_zero_bits(bits)) : _m.constant_int(type, 0);
    auto const flags = is_float ? flags_of(i) : u8(0);
    _scratch.clear();
    for (auto l = u32(0); l < count_of(a); ++l)
        _scratch.push_back(_m.binary(binary_op::sub, zero, leaf_at(a, l), flags));
    define(index);
}

u64 writer::negative_zero_bits(u32 bits)
{
    return u64(1) << (bits - 1);
}

void writer::not_lanes(u32 index, instruction const& i)
{
    auto const a = value_ref(_p.operands_of(i)[0]);
    auto const type = _m.type_of(leaf_at(a));
    auto const ones = _m.constant_int(type, ~u64(0));
    _scratch.clear();
    for (auto l = u32(0); l < count_of(a); ++l)
        _scratch.push_back(_m.binary(binary_op::bit_xor, leaf_at(a, l), ones));
    define(index);
}

void writer::dx_lanes(u32 index, instruction const& i, dx_op op)
{
    auto const a = value_ref(_p.operands_of(i)[0]);
    auto const type = _m.type_of(leaf_at(a));
    auto const number = _m.constant_i32(k_dx_ops[isize(op)].number);
    _scratch.clear();
    for (auto l = u32(0); l < count_of(a); ++l)
    {
        ll_value const args[] = {number, leaf_at(a, l)};
        _scratch.push_back(call(op, type, args));
    }
    define(index);
}

void writer::reduce_bools(u32 index, instruction const& i, bool is_all)
{
    auto const a = value_ref(_p.operands_of(i)[0]);
    auto result = leaf_at(a, 0);
    for (auto l = u32(1); l < count_of(a); ++l)
        result = _m.binary(is_all ? binary_op::bit_and : binary_op::bit_or, result, leaf_at(a, l));
    define_one(index, result);
}

void writer::select_lanes(u32 index, instruction const& i)
{
    auto const operands = _p.operands_of(i);
    auto const condition = value_ref(operands[0]);
    auto const yes = value_ref(operands[1]);
    auto const no = value_ref(operands[2]);
    auto const conditions = count_of(condition);
    _scratch.clear();
    for (auto l = u32(0); l < count_of(yes); ++l)
        _scratch.push_back(_m.select(leaf_at(condition, conditions == 1 ? 0 : l), leaf_at(yes, l), leaf_at(no, l)));
    define(index);
}

void writer::construct(u32 index, instruction const& i)
{
    _scratch.clear();
    for (auto const raw : _p.operands_of(i))
        for (auto l = u32(0); l < count_of(value_ref(raw)); ++l)
            _scratch.push_back(leaf_at(value_ref(raw), l));
    define(index);
}

u32 writer::leaf_offset(type_ref t, cc::span<u32 const> indices, type_ref& landed)
{
    auto offset = u32(0);
    for (auto const k : indices)
    {
        auto const& info = _p.at(t);
        if (info.kind == type_kind::structure)
        {
            for (auto m = u32(0); m < k; ++m)
                offset += leaves_of(_p.members[info.first_member + m].type);
            t = _p.members[info.first_member + k].type;
        }
        else
        {
            offset += k * leaves_of(info.element);
            t = info.element;
        }
    }
    landed = t;
    return offset;
}

void writer::extract(u32 index, instruction const& i)
{
    auto const operands = _p.operands_of(i);
    auto const source = value_ref(operands[0]);
    auto landed = type_ref::none;
    auto const offset = leaf_offset(_p.type_of(source), operands.subspan(1), landed);
    _scratch.clear();
    for (auto l = u32(0); l < leaves_of(landed); ++l)
        _scratch.push_back(leaf_at(source, offset + l));
    define(index);
}

void writer::insert(u32 index, instruction const& i)
{
    auto const operands = _p.operands_of(i);
    auto const part = value_ref(operands[0]);
    auto const whole = value_ref(operands[1]);
    auto landed = type_ref::none;
    auto const offset = leaf_offset(_p.type_of(whole), operands.subspan(2), landed);
    auto const size = leaves_of(landed);
    _scratch.clear();
    for (auto l = u32(0); l < count_of(whole); ++l)
        _scratch.push_back(l >= offset && l < offset + size ? leaf_at(part, l - offset) : leaf_at(whole, l));
    define(index);
}

void writer::shuffle(u32 index, instruction const& i)
{
    auto const operands = _p.operands_of(i);
    auto const a = value_ref(operands[0]);
    auto const b = value_ref(operands[1]);
    auto const first_lanes = count_of(a);
    _scratch.clear();
    for (auto k = isize(2); k < operands.size(); ++k)
        _scratch.push_back(operands[k] < first_lanes ? leaf_at(a, operands[k]) : leaf_at(b, operands[k] - first_lanes));
    define(index);
}

void writer::phi(u32 index, instruction const& i)
{
    auto const operands = _p.operands_of(i);
    auto edges = isize(0);
    for (auto k = isize(1); k < operands.size(); k += 2)
        edges += is_reachable(block_ref(operands[k])) ? 1 : 0;
    _scratch.clear();
    for (auto l = u32(0); l < leaves_of(i.type); ++l)
        _scratch.push_back(_m.phi(scalar_type(scalar_at(i.type, l)), edges));
    _phis.push_back(index);
    define(index);
}

void writer::fill_phis()
{
    for (auto const index : _phis)
    {
        auto const operands = _p.operands_of(_p.instructions[index]);
        auto const first = _value_first[index];
        for (auto l = u32(0); l < _value_count[index]; ++l)
        {
            auto edge = isize(0);
            for (auto k = isize(0); k < operands.size(); k += 2)
            {
                if (!is_reachable(block_ref(operands[k + 1])))
                    continue;
                _m.set_phi_edge(_values[first + l], edge++, leaf_at(value_ref(operands[k]), l), _blocks[operands[k + 1]]);
            }
        }
    }
}

void writer::linear_algebra(u32 index, instruction const& i)
{
    auto const operands = _p.operands_of(i);
    auto const a = value_ref(operands[0]);
    auto const flags = flags_of(i);
    auto const mul = [&](ll_value x, ll_value y) { return _m.binary(binary_op::mul, x, y, flags); };
    auto const add = [&](ll_value x, ll_value y) { return _m.binary(binary_op::add, x, y, flags); };

    if (i.code == op::dot)
    {
        ll_value left[4];
        ll_value right[4];
        auto const b = value_ref(operands[1]);
        auto const lanes = count_of(a);
        for (auto l = u32(0); l < lanes; ++l)
        {
            left[l] = leaf_at(a, l);
            right[l] = leaf_at(b, l);
        }
        return define_one(index,
                          dot_of(cc::span<ll_value const>(left, lanes), cc::span<ll_value const>(right, lanes), flags));
    }

    _scratch.clear();
    auto const& result = _p.at(i.type);
    switch (i.code)
    {
    case op::vector_times_scalar:
    case op::matrix_times_scalar:
    {
        auto const scalar = leaf_at(value_ref(operands[1]));
        for (auto l = u32(0); l < count_of(a); ++l)
            _scratch.push_back(mul(leaf_at(a, l), scalar));
        break;
    }
    case op::matrix_times_vector:
    {
        auto const& matrix = _p.at(_p.type_of(a));
        auto const v = value_ref(operands[1]);
        auto const rows = leaves_of(matrix.element);
        for (auto r = u32(0); r < rows; ++r)
        {
            auto sum = mul(leaf_at(a, r), leaf_at(v, 0));
            for (auto c = u32(1); c < matrix.count; ++c)
                sum = add(sum, mul(leaf_at(a, c * rows + r), leaf_at(v, c)));
            _scratch.push_back(sum);
        }
        break;
    }
    case op::matrix_times_matrix:
    {
        auto const& left = _p.at(_p.type_of(a));
        auto const b = value_ref(operands[1]);
        auto const rows = leaves_of(left.element);
        for (auto column = u32(0); column < result.count; ++column)
            for (auto r = u32(0); r < rows; ++r)
            {
                auto sum = mul(leaf_at(a, r), leaf_at(b, column * rows));
                for (auto c = u32(1); c < left.count; ++c)
                    sum = add(sum, mul(leaf_at(a, c * rows + r), leaf_at(b, column * rows + c)));
                _scratch.push_back(sum);
            }
        break;
    }
    case op::transpose:
    {
        auto const& source = _p.at(_p.type_of(a));
        auto const rows = leaves_of(source.element);
        for (auto r = u32(0); r < rows; ++r)
            for (auto c = u32(0); c < source.count; ++c)
                _scratch.push_back(leaf_at(a, c * rows + r));
        break;
    }
    default:
        return unsupported("a linear-algebra operation");
    }
    define(index);
}
