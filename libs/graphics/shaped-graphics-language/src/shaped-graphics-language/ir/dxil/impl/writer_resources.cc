#include "writer.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/resources.hh>
#include <shaped-graphics-language/emit/impl/plan.hh>

using namespace sgl;
using namespace sgl::ir;
using namespace sgl::ir::dxil::impl;

namespace
{
/// DXIL's `ResourceKind`, by a texture's or an image's axes.
[[nodiscard]] constexpr u32 texture_kind(ir::type const& t)
{
    if (t.dimension == 2)
        return 4;
    if (t.dimension == 3)
        return t.is_arrayed ? 9 : 5;
    if (t.dimension == 0)
        return t.is_arrayed ? 6 : 1;
    if (t.is_multisampled)
        return t.is_arrayed ? 8 : 3;
    return t.is_arrayed ? 7 : 2;
}

constexpr u32 k_kind_raw_buffer = 11;
constexpr u32 k_kind_structured_buffer = 12;
constexpr u32 k_kind_constants = 13;
constexpr u32 k_kind_sampler = 14;

/// What a binding is to DXIL, per `global_kind`; the row of a new kind is what adds it.
struct resource_rule
{
    /// `DXIL::ResourceClass`: 0 SRV, 1 UAV, 2 CBV, 3 sampler.
    u32 cls = 0;
    /// PSV's `PSVResourceType` for that class, read and written.
    u32 psv_read = 0;
    u32 psv_write = 0;
};

[[nodiscard]] constexpr resource_rule rule_of(global_kind kind)
{
    switch (kind)
    {
    case global_kind::constants:
    case global_kind::inline_constants:
        return {.cls = 2, .psv_read = 2, .psv_write = 2};
    case global_kind::buffer:
        return {.cls = 0, .psv_read = 5, .psv_write = 8};
    case global_kind::bytes:
        return {.cls = 0, .psv_read = 4, .psv_write = 7};
    case global_kind::texture:
    case global_kind::image:
        return {.cls = 0, .psv_read = 3, .psv_write = 6};
    case global_kind::sampler:
    case global_kind::file_sampler:
        return {.cls = 3, .psv_read = 1, .psv_write = 1};
    default:
        return {};
    }
}

/// DXIL's `ComponentType` of a scalar.
[[nodiscard]] constexpr u32 component_type(ir::type const& scalar)
{
    if (scalar.kind == type_kind::float_)
        return scalar.width == 16 ? 8 : scalar.width == 64 ? 10 : 9;
    if (scalar.kind == type_kind::int_)
    {
        auto const base = scalar.width == 16 ? 2u : scalar.width == 64 ? 6u : 4u;
        return scalar.is_signed ? base : base + 1;
    }
    return 0;
}

/// How many components a texture's or an image's view declares, which its format says of an image.
[[nodiscard]] constexpr u32 components_of(ir::type const& t)
{
    return t.format >= 0 ? u32(check::k_image_formats[t.format].channels) : 4;
}

} // namespace

void writer::declare_handles()
{
    _handles.resize_to_filled(_p.globals.size(), {});
    // only what the program reaches is declared, which is the footprint sg binds
    auto is_used = cc::vector<bool>::create_filled(_p.globals.size(), false);
    for (auto const b : blocks_in_order(_p))
    {
        if (!is_reachable(b))
            continue;
        auto const& block = _p.blocks[isize(b)];
        for (auto i = block.first; i < block.first + block.count; ++i)
        {
            auto const& inst = _p.instructions[i];
            auto const operands = _p.operands_of(inst);
            for (auto k = isize(0); k < operands.size(); ++k)
                if (operand_role(inst.code, k) == operand::value && is_global(value_ref(operands[k])))
                    is_used[index_of(value_ref(operands[k]))] = true;
        }
    }

    u32 next_id[4] = {};
    auto const props_type = res_props_type();
    auto const bind_type = res_bind_type();
    for (auto g = isize(0); g < _p.globals.size(); ++g)
    {
        auto const& global = _p.globals[g];
        if (global.kind == global_kind::plain || !is_used[g])
            continue;
        auto const& pointer = _p.at(global.type);
        auto const& held = _p.at(pointer.element);
        auto const rule = rule_of(global.kind);
        auto const writes = (global.kind == global_kind::buffer || global.kind == global_kind::bytes
                             || global.kind == global_kind::image)
                         && !global.is_read_only;
        auto const is_storage_image = global.kind == global_kind::image;
        auto const cls = rule.cls == 0 && (writes || is_storage_image) ? 1u : rule.cls;

        auto space = u32(global.set);
        auto lower = u32(global.binding);
        auto kind = u32(0);
        auto word1 = u32(0);
        auto size = u32(0);
        auto stride = u32(0);
        auto component = u32(0);
        switch (global.kind)
        {
        case global_kind::inline_constants:
            space = u32(emit::impl::k_inline_constants_space);
            lower = 0;
            kind = k_kind_constants;
            size = constant_block_size(pointer.element);
            word1 = size;
            break;
        case global_kind::constants:
            kind = k_kind_constants;
            size = constant_block_size(pointer.element);
            word1 = size;
            break;
        case global_kind::buffer:
            kind = k_kind_structured_buffer;
            stride = u32(_p.at(_p.members[held.first_member].type).stride);
            word1 = stride;
            _uses.structured = true;
            break;
        case global_kind::bytes:
            kind = k_kind_raw_buffer;
            _uses.structured = true;
            break;
        case global_kind::texture:
        case global_kind::image:
            kind = texture_kind(held);
            component = component_type(_p.at(held.element));
            word1 = component | (components_of(held) << 8);
            break;
        case global_kind::file_sampler:
            space = u32(emit::impl::k_bound_samplers_space);
            lower = u32(global.binding - 1);
            kind = k_kind_sampler;
            break;
        case global_kind::sampler:
            kind = k_kind_sampler;
            break;
        default:
            break;
        }

        auto const id = next_id[cls]++;
        ll_value const bind_fields[]
            = {_m.constant_i32(lower), _m.constant_i32(lower), _m.constant_i32(space), _m.constant_i8(cls)};
        ll_value const bind_args[]
            = {_m.constant_i32(k_dx_ops[isize(dx_op::create_handle_from_binding)].number),
               _m.constant_struct(bind_type, bind_fields), _m.constant_i32(lower), _m.constant_bool(false)};
        auto const binding = call(dx_op::create_handle_from_binding, _m.void_type(), bind_args);

        // bits 0 to 7 the kind, bit 12 a UAV, bit 14 globally coherent, bit 15 a sampler that compares
        auto const word0 = kind | (cls == 1 ? 1u << 12 : 0u) | (global.is_coherent ? 1u << 14 : 0u)
                         | (global.is_comparison ? 1u << 15 : 0u);
        ll_value const prop_fields[] = {_m.constant_i32(word0), _m.constant_i32(word1)};
        ll_value const annotate_args[] = {_m.constant_i32(k_dx_ops[isize(dx_op::annotate_handle)].number), binding,
                                          _m.constant_struct(props_type, prop_fields)};
        _handles[g] = {.annotated = call(dx_op::annotate_handle, _m.void_type(), annotate_args)};

        _bindings.push_back({.global = u32(g),
                             .cls = cls,
                             .id = id,
                             .space = space,
                             .lower = lower,
                             .kind = kind,
                             .size = size,
                             .stride = stride,
                             .component = component,
                             .psv_type = cls == 1 ? rule.psv_write : rule.psv_read,
                             .is_comparison = global.is_comparison});
    }
}

u32 writer::constant_block_size(type_ref block)
{
    // the end of its last member; a host's block rounds up to rows of 16 bytes, which DXIL states as the block's size
    auto const& info = _p.at(block);
    auto end = u32(0);
    for (auto m = u32(0); m < info.member_count; ++m)
    {
        auto const& held = _p.members[info.first_member + m];
        end = cc::max(end, u32(held.offset) + memory_size(held.type, held.matrix_stride));
    }
    return end;
}

u32 writer::memory_size(type_ref t, i32 matrix_stride)
{
    auto const& info = _p.at(t);
    switch (info.kind)
    {
    case type_kind::vector:
        return info.count * (_p.at(info.element).width / 8);
    case type_kind::matrix:
        return info.count * u32(matrix_stride);
    case type_kind::array:
        return info.count * u32(info.stride);
    case type_kind::structure:
    {
        auto end = u32(0);
        for (auto m = u32(0); m < info.member_count; ++m)
        {
            auto const& held = _p.members[info.first_member + m];
            end = cc::max(end, u32(held.offset) + memory_size(held.type, held.matrix_stride));
        }
        return end;
    }
    default:
        return info.width / 8;
    }
}

ll_value writer::handle_of(value_ref global)
{
    return _handles[index_of(global)].annotated;
}

void writer::sampled(u32 index, instruction const& i, dx_op op, cc::span<ll_value const> tail, isize offsets)
{
    auto const operands = _p.operands_of(i);
    auto const pair = value_ref(operands[0]);
    auto const at = value_ref(operands[1]);
    auto const lanes = count_of(at);
    auto const undefined = _m.constant_undef(_m.int_type());
    auto const undefined_f = _m.constant_undef(_m.float_type());
    auto args = cc::vector<ll_value>();
    args.push_back(_m.constant_i32(k_dx_ops[isize(op)].number));
    args.push_back(leaf_at(pair, 0));
    args.push_back(leaf_at(pair, 1));
    for (auto l = u32(0); l < 4; ++l)
        args.push_back(l < lanes ? leaf_at(at, l) : undefined_f);
    for (auto k = isize(0); k < offsets; ++k)
        args.push_back(undefined);
    args.push_back_range(tail);
    auto const fetched = call(op, scalar_type(scalar_at(i.type, 0)), args);
    _scratch.clear();
    for (auto l = u32(0); l < leaves_of(i.type); ++l)
        _scratch.push_back(_m.extract_value(fetched, l));
    define(index);
}

void writer::texture_op(u32 index, instruction const& i)
{
    auto const operands = _p.operands_of(i);
    auto const undefined = _m.constant_undef(_m.int_type());
    // a coordinate lane no call reads, and a `clamp` none states
    auto const clamp = _m.constant_undef(_m.float_type());
    switch (i.code)
    {
    case op::combine_sampler:
        _scratch.clear();
        _scratch.push_back(leaf_at(value_ref(operands[0])));
        _scratch.push_back(leaf_at(value_ref(operands[1])));
        return define(index);
    case op::image_fetch:
    case op::image_read:
    {
        auto const image = value_ref(operands[0]);
        auto const at = value_ref(operands[1]);
        auto const texel = scalar_type(scalar_at(i.type, 0));
        auto const lanes = count_of(at);
        auto const is_fetch = i.code == op::image_fetch;
        if (!is_fetch && components_of(_p.at(_p.type_of(image))) > 1)
            _uses.typed_uav_load = true;
        ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(dx_op::texture_load)].number),
                                 leaf_at(image),
                                 is_fetch ? leaf_at(value_ref(operands[2])) : undefined,
                                 leaf_at(at, 0),
                                 lanes > 1 ? leaf_at(at, 1) : undefined,
                                 lanes > 2 ? leaf_at(at, 2) : undefined,
                                 undefined,
                                 undefined,
                                 undefined};
        auto const fetched = call(dx_op::texture_load, texel, args, is_fetch);
        _scratch.clear();
        for (auto l = u32(0); l < 4; ++l)
            _scratch.push_back(_m.extract_value(fetched, l));
        return define(index);
    }
    case op::image_write:
    {
        auto const image = value_ref(operands[0]);
        auto const at = value_ref(operands[1]);
        auto const value = value_ref(operands[2]);
        auto const lanes = count_of(at);
        auto const texel = _m.type_of(leaf_at(value, 0));
        ll_value const args[]
            = {_m.constant_i32(k_dx_ops[isize(dx_op::texture_store)].number), leaf_at(image), leaf_at(at, 0),
               lanes > 1 ? leaf_at(at, 1) : undefined, lanes > 2 ? leaf_at(at, 2) : undefined, leaf_at(value, 0),
               leaf_at(value, 1), leaf_at(value, 2), leaf_at(value, 3),
               // DXIL takes a typed store of all four whatever the format holds
               _m.constant_i8(15)};
        (void)call(dx_op::texture_store, texel, args);
        return;
    }
    case op::sample_lod:
    {
        ll_value const tail[] = {leaf_at(value_ref(operands[2]))};
        return sampled(index, i, dx_op::sample_level, tail);
    }
    case op::sample_implicit:
    {
        ll_value const tail[] = {clamp};
        return sampled(index, i, dx_op::sample, tail);
    }
    case op::sample_bias:
    {
        ll_value const tail[] = {leaf_at(value_ref(operands[2])), clamp};
        return sampled(index, i, dx_op::sample_bias, tail);
    }
    case op::sample_grad:
    {
        auto const x = value_ref(operands[2]);
        auto const y = value_ref(operands[3]);
        auto const lanes = count_of(x);
        ll_value const tail[] = {leaf_at(x, 0),
                                 lanes > 1 ? leaf_at(x, 1) : clamp,
                                 lanes > 2 ? leaf_at(x, 2) : clamp,
                                 leaf_at(y, 0),
                                 lanes > 1 ? leaf_at(y, 1) : clamp,
                                 lanes > 2 ? leaf_at(y, 2) : clamp,
                                 clamp};
        return sampled(index, i, dx_op::sample_grad, tail);
    }
    case op::sample_compare:
    {
        ll_value const tail[] = {leaf_at(value_ref(operands[2])), clamp};
        return sampled(index, i, dx_op::sample_cmp, tail);
    }
    case op::sample_compare_zero:
    {
        ll_value const tail[] = {leaf_at(value_ref(operands[2]))};
        return sampled(index, i, dx_op::sample_cmp_level_zero, tail);
    }
    case op::image_gather:
    {
        ll_value const tail[] = {leaf_at(value_ref(operands[2]))};
        return sampled(index, i, dx_op::texture_gather, tail, 2);
    }
    case op::image_gather_compare:
    {
        // the channel comes first, and a compare takes the first
        ll_value const tail[] = {_m.constant_i32(0), leaf_at(value_ref(operands[2]))};
        return sampled(index, i, dx_op::texture_gather_cmp, tail, 2);
    }
    default:
        return unsupported("this texture operation on DXIL");
    }
}

void writer::image_size(u32 index, instruction const& i)
{
    auto const operands = _p.operands_of(i);
    auto const image = leaf_at(value_ref(operands[0]));
    auto const undefined = _m.constant_undef(_m.int_type());
    auto const level = i.code == op::image_size && i.aux != 0 ? leaf_at(value_ref(operands[1])) : undefined;
    ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(dx_op::get_dimensions)].number), image,
                             i.code == op::image_levels ? _m.constant_i32(0) : level};
    auto const dimensions = call(dx_op::get_dimensions, _m.void_type(), args);
    if (i.code == op::image_levels)
        return define_one(index, _m.extract_value(dimensions, 3));
    if (i.code == op::image_samples)
        return unsupported("a texture's sample count on DXIL");
    _scratch.clear();
    for (auto l = u32(0); l < leaves_of(i.type); ++l)
        _scratch.push_back(_m.extract_value(dimensions, l));
    define(index);
}

void writer::array_length(u32 index, instruction const& i)
{
    auto const global = value_ref(_p.operands_of(i)[0]);
    auto const undefined = _m.constant_undef(_m.int_type());
    ll_value const args[]
        = {_m.constant_i32(k_dx_ops[isize(dx_op::get_dimensions)].number), handle_of(global), undefined};
    auto const dimensions = call(dx_op::get_dimensions, _m.void_type(), args);
    auto const width = _m.extract_value(dimensions, 0);
    // a raw buffer states bytes, and the form counts its words
    auto const is_raw = _p.globals[index_of(global)].kind == global_kind::bytes;
    define_one(index, is_raw ? _m.binary(binary_op::lshr, width, _m.constant_i32(2)) : width);
}

ll_value writer::atomic_on_buffer(place const& p, u32 op, ll_value value, ll_type t)
{
    auto const handle = handle_of(global_at(p.root));
    auto const undefined = _m.constant_undef(_m.int_type());
    auto const is_raw = _p.globals[p.root].kind == global_kind::bytes;
    auto const first = is_raw ? _m.binary(binary_op::add, _m.binary(binary_op::mul, p.index, _m.constant_i32(4)),
                                          byte_offset(p, p.offset))
                              : p.index;
    ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(dx_op::atomic_bin_op)].number),
                             handle,
                             _m.constant_i32(op),
                             first,
                             is_raw ? undefined : byte_offset(p, p.offset),
                             undefined,
                             value};
    return call(dx_op::atomic_bin_op, t, args);
}

void writer::atomic_op(u32 index, instruction const& i)
{
    /// DXIL's `AtomicBinOpCode`, and LLVM's own for workgroup memory.
    struct rule
    {
        ir::op code;
        u32 dx;
        rmw_op llvm;
    };
    static constexpr rule k_rules[] = {
        {op::atomic_iadd, 0, rmw_op::add},       {op::atomic_isub, 0, rmw_op::sub},
        {op::atomic_and, 1, rmw_op::bit_and},    {op::atomic_or, 2, rmw_op::bit_or},
        {op::atomic_xor, 3, rmw_op::bit_xor},    {op::atomic_smin, 4, rmw_op::min},
        {op::atomic_smax, 5, rmw_op::max},       {op::atomic_umin, 6, rmw_op::umin},
        {op::atomic_umax, 7, rmw_op::umax},      {op::atomic_exchange, 8, rmw_op::exchange},
        {op::atomic_store, 8, rmw_op::exchange}, {op::atomic_load, 2, rmw_op::bit_or},
    };

    auto const operands = _p.operands_of(i);
    auto const where = place_of(value_ref(operands[0]));
    auto const held = _p.at(_p.type_of(value_ref(operands[0]))).element;
    if (_p.at(held).kind != type_kind::int_ || _p.at(held).width != 32)
        return unsupported("an atomic of anything but a 32-bit integer on DXIL");
    auto const type = _m.int_type();

    if (i.code == op::atomic_compare_exchange)
    {
        auto const desired = leaf_at(value_ref(operands[1]));
        auto const comparator = leaf_at(value_ref(operands[2]));
        if (where.kind == place::kind_t::memory)
        {
            auto const pointer = memory_pointer(where, 0, held);
            return define_one(index, _m.extract_value(_m.atomic_exchange(pointer, comparator, desired), 0));
        }
        auto const undefined = _m.constant_undef(_m.int_type());
        ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(dx_op::atomic_compare_exchange)].number),
                                 handle_of(global_at(where.root)),
                                 where.index,
                                 byte_offset(where, where.offset),
                                 undefined,
                                 comparator,
                                 desired};
        return define_one(index, call(dx_op::atomic_compare_exchange, type, args));
    }

    rule const* found = nullptr;
    for (auto const& r : k_rules)
        if (r.code == i.code)
            found = &r;
    if (found == nullptr)
        return unsupported("this atomic on DXIL");

    auto value = i.code == op::atomic_load ? _m.constant_i32(0) : leaf_at(value_ref(operands[1]));
    if (where.kind == place::kind_t::memory)
    {
        auto const result = _m.atomic_rmw(found->llvm, memory_pointer(where, 0, held), value);
        if (i.code != op::atomic_store)
            define_one(index, result);
        return;
    }
    if (i.code == op::atomic_isub)
        value = _m.binary(binary_op::sub, _m.constant_i32(0), value);
    auto const result = atomic_on_buffer(where, found->dx, value, type);
    if (i.code != op::atomic_store)
        define_one(index, result);
}

void writer::barrier_op(instruction const& i)
{
    // `BarrierMode`: 1 syncs the group, 2 fences the UAVs of the device, 8 fences group-shared memory
    static constexpr u32 k_modes[] = {1 | 8, 1 | 2, 1 | 2};
    ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(dx_op::barrier)].number), _m.constant_i32(k_modes[i.sub])};
    (void)call(dx_op::barrier, _m.void_type(), args);
}

void writer::subgroup_op(u32 index, instruction const& i)
{
    /// `WaveOpKind`: 0 sum, 1 product, 2 min, 3 max; `SignedOpKind`: 0 signed, 1 unsigned.
    struct rule
    {
        subgroup what;
        u8 kind;
        u8 sign;
    };
    static constexpr rule k_folds[] = {
        {subgroup::iadd, 0, 1}, {subgroup::fadd, 0, 0}, {subgroup::imul, 1, 1}, {subgroup::fmul, 1, 0},
        {subgroup::smin, 2, 0}, {subgroup::umin, 2, 1}, {subgroup::fmin, 2, 0}, {subgroup::smax, 3, 0},
        {subgroup::umax, 3, 1}, {subgroup::fmax, 3, 0},
    };
    static constexpr rule k_bits[] = {
        {subgroup::bit_and, 0, 0},
        {subgroup::bit_or, 1, 0},
        {subgroup::bit_xor, 2, 0},
    };

    _uses.waves = true;
    auto const operands = _p.operands_of(i);
    auto const value = value_ref(operands[0]);
    auto const what = subgroup(i.sub);
    auto const number = [&](dx_op op) { return _m.constant_i32(k_dx_ops[isize(op)].number); };

    _scratch.clear();
    for (auto const& r : k_folds)
        if (r.what == what)
        {
            auto const fold = group_fold(i.aux);
            auto const op = fold == group_fold::reduce ? dx_op::wave_active_op : dx_op::wave_prefix_op;
            if (fold == group_fold::inclusive)
                return unsupported("an inclusive subgroup fold on DXIL");
            for (auto l = u32(0); l < count_of(value); ++l)
            {
                auto const lane = leaf_at(value, l);
                ll_value const args[] = {number(op), lane, _m.constant_i8(r.kind), _m.constant_i8(r.sign)};
                _scratch.push_back(call(op, _m.type_of(lane), args));
            }
            return define(index);
        }
    for (auto const& r : k_bits)
        if (r.what == what)
        {
            for (auto l = u32(0); l < count_of(value); ++l)
            {
                auto const lane = leaf_at(value, l);
                ll_value const args[] = {number(dx_op::wave_active_bit), lane, _m.constant_i8(r.kind)};
                _scratch.push_back(call(dx_op::wave_active_bit, _m.type_of(lane), args));
            }
            return define(index);
        }

    switch (what)
    {
    case subgroup::all:
    case subgroup::any:
    {
        auto const op = what == subgroup::all ? dx_op::wave_all_true : dx_op::wave_any_true;
        ll_value const args[] = {number(op), leaf_at(value)};
        return define_one(index, call(op, _m.void_type(), args));
    }
    case subgroup::broadcast:
    {
        auto const id = leaf_at(value_ref(operands[1]));
        for (auto l = u32(0); l < count_of(value); ++l)
        {
            auto const lane = leaf_at(value, l);
            ll_value const args[] = {number(dx_op::wave_read_lane_at), lane, id};
            _scratch.push_back(call(dx_op::wave_read_lane_at, _m.type_of(lane), args));
        }
        return define(index);
    }
    case subgroup::broadcast_first:
        for (auto l = u32(0); l < count_of(value); ++l)
        {
            auto const lane = leaf_at(value, l);
            ll_value const args[] = {number(dx_op::wave_read_lane_first), lane};
            _scratch.push_back(call(dx_op::wave_read_lane_first, _m.type_of(lane), args));
        }
        return define(index);
    default:
        return unsupported("this subgroup operation on DXIL");
    }
}
