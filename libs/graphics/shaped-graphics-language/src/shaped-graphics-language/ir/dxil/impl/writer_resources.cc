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
constexpr u32 k_uavs_of_every_device = 8;
constexpr u32 k_kind_acceleration_structure = 16;
/// `DXIL::ComponentType::I32`, the element DXC states of a structure.
constexpr u32 k_component_i32 = 4;
/// PSV's `UAVStructuredWithCounter`.
constexpr u32 k_psv_uav_with_counter = 9;
/// `NvShaderExtnStruct`, a record of NVAPI's protocol (EMIT-263).
constexpr auto k_nvapi_record_bytes = u32(check::k_nvapi_record_bytes);

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
    case global_kind::acceleration_structure:
        return {.cls = 0, .psv_read = 4, .psv_write = 4};
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
    auto uav_count = u32(0);
    (void)res_props_type();
    (void)res_bind_type();
    for (auto g = isize(0); g < _p.globals.size(); ++g)
    {
        auto const& global = _p.globals[g];
        if (global.kind == global_kind::plain || !is_used[g])
            continue;
        auto const& pointer = _p.at(global.type);
        auto const& outer = _p.at(pointer.element);
        auto const is_array = outer.kind == type_kind::array || outer.kind == type_kind::runtime_array;
        auto const& held = is_array ? _p.at(outer.element) : outer;
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
        case global_kind::acceleration_structure:
            kind = k_kind_acceleration_structure;
            component = k_component_i32;
            break;
        default:
            break;
        }

        auto const has_uavs_anyway = _p.stage == check::stage::pixel || _p.stage == check::stage::compute
                                  || _p.stage == check::stage::mesh || _p.stage == check::stage::task || is_library();
        _uses.uavs_at_every_stage = _uses.uavs_at_every_stage || (cls == 1 && !has_uavs_anyway);
        auto const id = next_id[cls]++;
        // EMIT-210: an unbounded array is a range the pipeline layout leaves open
        auto const range = !is_array ? 1u : outer.kind == type_kind::array ? outer.count : 0xffffffffu;
        auto const upper = range == 0xffffffffu ? range : lower + range - 1;
        if (cls == 1)
        {
            uav_count = range == 0xffffffffu ? range : uav_count + range;
            _uses.many_uavs = _uses.many_uavs || uav_count > k_uavs_of_every_device;
        }
        // bits 0 to 7 the kind, bit 12 a UAV, bit 14 globally coherent, bit 15 a sampler that compares
        auto const word0 = kind | (cls == 1 ? 1u << 12 : 0u) | (global.is_coherent ? 1u << 14 : 0u)
                         | (global.is_comparison ? 1u << 15 : 0u);
        _handles[g] = {.is_array = is_array,
                       .lower = lower,
                       .upper = upper,
                       .space = space,
                       .cls = cls,
                       .word0 = word0,
                       .word1 = word1};
        if (is_library())
        {
            auto& handle = _handles[g];
            handle.variable_type = is_array ? _m.array(handle_type(), range == 0xffffffffu ? 0 : range) : handle_type();
            handle.variable
                = _m.add_external_constant(_p.name_at(global.name_first, global.name_size), handle.variable_type, 4);
        }
        if (!is_array)
            _handles[g].annotated = made_handle(_handles[g], ll_value::none, false);

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
                             .is_comparison = global.is_comparison,
                             .name = _p.name_at(global.name_first, global.name_size),
                             .is_coherent = global.is_coherent,
                             .upper = upper,
                             .range = range});
    }
    // EMIT-263: `RWStructuredBuffer<NvShaderExtnStruct> g_NvidiaExt` at `u0, space13`, as `nvHLSLExtns.h` declares it,
    // through which the program talks to NVIDIA's driver; no global of the program, so its handle stands past theirs
    if (_p.reorders_through_nvapi)
    {
        auto const space = u32(check::k_nvapi_space);
        auto const lower = u32(check::k_nvapi_register);
        auto entry = handle_entry{.lower = lower,
                                  .upper = lower,
                                  .space = space,
                                  .cls = 1,
                                  // bit 15 of a UAV is its counter
                                  .word0 = k_kind_structured_buffer | (1u << 12) | (1u << 15),
                                  .word1 = k_nvapi_record_bytes};
        if (is_library())
        {
            entry.variable_type = handle_type();
            entry.variable = _m.add_external_constant("g_NvidiaExt", entry.variable_type, 4);
        }
        entry.annotated = made_handle(entry, ll_value::none, false);
        _nvapi = entry.annotated;
        _handles.push_back(entry);
        _uses.structured = true;
        uav_count += 1;
        _uses.many_uavs = _uses.many_uavs || uav_count > k_uavs_of_every_device;
        _bindings.push_back({.global = u32(_handles.size() - 1),
                             .cls = 1,
                             .id = next_id[1]++,
                             .space = space,
                             .lower = lower,
                             .kind = k_kind_structured_buffer,
                             .stride = k_nvapi_record_bytes,
                             .psv_type = k_psv_uav_with_counter,
                             .name = "g_NvidiaExt",
                             .has_counter = true,
                             .upper = lower,
                             .range = 1});
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

ll_value writer::handle_at(place const& p)
{
    return p.handle != ll_value::none ? p.handle : handle_of(global_at(p.root));
}

ll_value writer::made_handle(handle_entry const& h, ll_value index, bool is_nonuniform)
{
    if (is_library())
    {
        auto pointer = h.variable;
        if (index != ll_value::none)
        {
            ll_value const path[] = {_m.constant_i32(0), index};
            pointer = _m.element_pointer(h.variable_type, h.variable, path, is_nonuniform);
        }
        auto const loaded = _m.load(handle_type(), pointer, 4);
        ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(dx_op::create_handle_for_lib)].number), loaded};
        auto const library_handle = call(dx_op::create_handle_for_lib, handle_type(), args);
        ll_value const fields[] = {_m.constant_i32(h.word0), _m.constant_i32(h.word1)};
        ll_value const annotate[] = {_m.constant_i32(k_dx_ops[isize(dx_op::annotate_handle)].number), library_handle,
                                     _m.constant_struct(res_props_type(), fields)};
        return call(dx_op::annotate_handle, _m.void_type(), annotate);
    }
    ll_value const bind_fields[]
        = {_m.constant_i32(h.lower), _m.constant_i32(h.upper), _m.constant_i32(h.space), _m.constant_i8(h.cls)};
    auto const number = _m.constant_i32(k_dx_ops[isize(dx_op::create_handle_from_binding)].number);
    auto const bind = _m.constant_struct(res_bind_type(), bind_fields);
    auto const absolute = index == ll_value::none ? _m.constant_i32(h.lower)
                        : h.lower == 0            ? index
                                                  : _m.binary(binary_op::add, index, _m.constant_i32(h.lower));
    ll_value const bind_args[] = {number, bind, absolute, _m.constant_bool(is_nonuniform)};
    auto const binding = call(dx_op::create_handle_from_binding, _m.void_type(), bind_args);
    ll_value const prop_fields[] = {_m.constant_i32(h.word0), _m.constant_i32(h.word1)};
    ll_value const annotate_args[] = {_m.constant_i32(k_dx_ops[isize(dx_op::annotate_handle)].number), binding,
                                      _m.constant_struct(res_props_type(), prop_fields)};
    return call(dx_op::annotate_handle, _m.void_type(), annotate_args);
}

void writer::sampled(u32 index,
                     instruction const& i,
                     dx_op op,
                     cc::function_ref<void(cc::vector<ll_value>&)> tail,
                     isize offsets,
                     value_ref offset)
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
    // a stated offset's lanes are its constants, which the validator wants as immediates; `undef` past them
    auto const offset_lanes = is_valid(offset) ? isize(count_of(offset)) : 0;
    for (auto k = isize(0); k < offsets; ++k)
        args.push_back(k < offset_lanes ? leaf_at(offset, u32(k)) : undefined);
    tail(args);
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
    // what no call states: a gradient's unread lanes, and a `clamp`
    auto const unstated = [&] { return _m.constant_undef(_m.float_type()); };
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
    case op::sample_compare_zero:
        return sampled(index, i, i.code == op::sample_lod ? dx_op::sample_level : dx_op::sample_cmp_level_zero,
                       [&](cc::vector<ll_value>& args) { args.push_back(leaf_at(value_ref(operands[2]))); });
    case op::sample_implicit:
        return sampled(
            index, i, dx_op::sample, [&](cc::vector<ll_value>& args) { args.push_back(unstated()); }, 3,
            operands.size() > 2 ? value_ref(operands[2]) : value_ref::none);
    case op::sample_bias:
    case op::sample_compare:
        return sampled(index, i, i.code == op::sample_bias ? dx_op::sample_bias : dx_op::sample_cmp,
                       [&](cc::vector<ll_value>& args)
                       {
                           args.push_back(leaf_at(value_ref(operands[2])));
                           args.push_back(unstated());
                       });
    case op::sample_grad:
        return sampled(index, i, dx_op::sample_grad,
                       [&](cc::vector<ll_value>& args)
                       {
                           for (auto const gradient : {value_ref(operands[2]), value_ref(operands[3])})
                               for (auto l = u32(0); l < 3; ++l)
                                   args.push_back(l < count_of(gradient) ? leaf_at(gradient, l) : unstated());
                           args.push_back(unstated());
                       });
    case op::image_gather:
        return sampled(
            index, i, dx_op::texture_gather,
            [&](cc::vector<ll_value>& args) { args.push_back(leaf_at(value_ref(operands[2]))); }, 2,
            operands.size() > 3 ? value_ref(operands[3]) : value_ref::none);
    case op::image_gather_compare:
        // the channel comes first, and a compare takes the first
        return sampled(
            index, i, dx_op::texture_gather_cmp,
            [&](cc::vector<ll_value>& args)
            {
                args.push_back(_m.constant_i32(0));
                args.push_back(leaf_at(value_ref(operands[2])));
            },
            2);
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
    auto const where = place_of(value_ref(_p.operands_of(i)[0]));
    auto const undefined = _m.constant_undef(_m.int_type());
    ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(dx_op::get_dimensions)].number), handle_at(where), undefined};
    auto const dimensions = call(dx_op::get_dimensions, _m.void_type(), args);
    auto const width = _m.extract_value(dimensions, 0);
    // a raw buffer states bytes, and the form counts its words
    auto const is_raw = _p.globals[where.root].kind == global_kind::bytes;
    define_one(index, is_raw ? _m.binary(binary_op::lshr, width, _m.constant_i32(2)) : width);
}

void writer::atomic_offsets(place const& p, ll_value (&offsets)[3])
{
    auto const undefined = _m.constant_undef(_m.int_type());
    offsets[0] = offsets[1] = offsets[2] = undefined;
    if (p.kind == place::kind_t::texel)
    {
        for (auto l = u32(0); l < p.path_count && l < 3; ++l)
            offsets[l] = _paths[p.path_first + l];
        return;
    }
    auto const is_raw = _p.globals[p.root].kind == global_kind::bytes;
    offsets[0] = is_raw ? raw_address(p, p.offset) : p.index;
    if (!is_raw)
        offsets[1] = byte_offset(p, p.offset);
}

ll_value writer::atomic_on_buffer(place const& p, u32 op, ll_value value, ll_type t)
{
    ll_value offsets[3];
    atomic_offsets(p, offsets);
    ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(dx_op::atomic_bin_op)].number),
                             handle_at(p),
                             _m.constant_i32(op),
                             offsets[0],
                             offsets[1],
                             offsets[2],
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
    if (_p.at(held).kind != type_kind::int_ || (_p.at(held).width != 32 && _p.at(held).width != 64))
        return unsupported("an atomic of anything but a 32- or 64-bit integer on DXIL");
    auto const type = scalar_type(held);
    // a buffer's 64-bit atomic needs nothing past Int64Ops, and one in workgroup memory states its own flag
    if (_p.at(held).width == 64 && where.kind == place::kind_t::memory)
        _uses.int64_shared_atomics = true;
    if (_p.at(held).width == 64 && where.kind == place::kind_t::buffer)
        for (auto& b : _bindings)
            if (b.global == where.root)
                b.used_by_atomic64 = true;

    if (i.code == op::atomic_compare_exchange)
    {
        auto const desired = leaf_at(value_ref(operands[1]));
        auto const comparator = leaf_at(value_ref(operands[2]));
        if (where.kind == place::kind_t::memory)
        {
            auto const pointer = memory_pointer(where, 0, held);
            return define_one(index, _m.extract_value(_m.atomic_exchange(pointer, comparator, desired), 0));
        }
        ll_value offsets[3];
        atomic_offsets(where, offsets);
        ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(dx_op::atomic_compare_exchange)].number),
                                 handle_at(where),
                                 offsets[0],
                                 offsets[1],
                                 offsets[2],
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

    auto value = i.code == op::atomic_load ? _m.constant_int(type, 0) : leaf_at(value_ref(operands[1]));
    if (where.kind == place::kind_t::memory)
    {
        auto const result = _m.atomic_rmw(found->llvm, memory_pointer(where, 0, held), value);
        if (i.code != op::atomic_store)
            define_one(index, result);
        return;
    }
    if (i.code == op::atomic_isub)
        value = _m.binary(binary_op::sub, _m.constant_int(type, 0), value);
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
            // DXIL's prefix is exclusive alone, and an inclusive one folds this invocation's own value in after it
            if (fold == group_fold::inclusive && r.kind > 1)
                return unsupported("an inclusive subgroup fold but a sum or a product on DXIL");
            for (auto l = u32(0); l < count_of(value); ++l)
            {
                auto const lane = leaf_at(value, l);
                ll_value const args[] = {number(op), lane, _m.constant_i8(r.kind), _m.constant_i8(r.sign)};
                auto folded = call(op, _m.type_of(lane), args);
                if (fold == group_fold::inclusive)
                {
                    auto const flags = _m.is_float(_m.type_of(lane)) ? flags_of(i) : u8(0);
                    folded = _m.binary(r.kind == 0 ? binary_op::add : binary_op::mul, folded, lane, flags);
                }
                _scratch.push_back(folded);
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
    case subgroup::ballot:
    {
        ll_value const args[] = {number(dx_op::wave_active_ballot), leaf_at(value)};
        auto const words = call(dx_op::wave_active_ballot, _m.void_type(), args);
        for (auto l = u32(0); l < 4; ++l)
            _scratch.push_back(_m.extract_value(words, l));
        return define(index);
    }
    case subgroup::shuffle:
    case subgroup::shuffle_xor:
    case subgroup::shuffle_up:
    case subgroup::shuffle_down:
    {
        // the lane an offset names is computed from this invocation's own, as HLSL's text does
        auto lane = leaf_at(value_ref(operands[1]));
        if (what != subgroup::shuffle)
        {
            ll_value const own_args[] = {number(dx_op::wave_get_lane_index)};
            auto const own = call(dx_op::wave_get_lane_index, _m.void_type(), own_args);
            auto const combine = what == subgroup::shuffle_xor ? binary_op::bit_xor
                               : what == subgroup::shuffle_up  ? binary_op::sub
                                                               : binary_op::add;
            lane = _m.binary(combine, own, lane);
        }
        for (auto l = u32(0); l < count_of(value); ++l)
        {
            auto const lane_value = leaf_at(value, l);
            ll_value const args[] = {number(dx_op::wave_read_lane_at), lane_value, lane};
            _scratch.push_back(call(dx_op::wave_read_lane_at, _m.type_of(lane_value), args));
        }
        return define(index);
    }
    case subgroup::quad_broadcast:
    {
        auto const lane = leaf_at(value_ref(operands[1]));
        for (auto l = u32(0); l < count_of(value); ++l)
        {
            auto const lane_value = leaf_at(value, l);
            ll_value const args[] = {number(dx_op::quad_read_lane_at), lane_value, lane};
            _scratch.push_back(call(dx_op::quad_read_lane_at, _m.type_of(lane_value), args));
        }
        return define(index);
    }
    case subgroup::quad_swap:
    {
        // `QuadOpKind`: 0 across x, 1 across y, 2 across the diagonal, which lowering states as a constant
        auto const axis = _p.constants[index_of(value_ref(operands[1]))].low;
        for (auto l = u32(0); l < count_of(value); ++l)
        {
            auto const lane_value = leaf_at(value, l);
            ll_value const args[] = {number(dx_op::quad_op), lane_value, _m.constant_i8(axis)};
            _scratch.push_back(call(dx_op::quad_op, _m.type_of(lane_value), args));
        }
        return define(index);
    }
    default:
        return unsupported("this subgroup operation on DXIL");
    }
}
