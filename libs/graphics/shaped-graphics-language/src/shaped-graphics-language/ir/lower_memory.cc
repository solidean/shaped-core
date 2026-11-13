#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/texture_calls.hh>
#include <shaped-graphics-language/check/resources.hh>
#include <shaped-graphics-language/ir/impl/lowering.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::ir;
using namespace sgl::ir::impl;
using emit::impl::address_space;

namespace
{
constexpr auto k_storage_classes = isize(storage::image) + 1;

/// Whether two descriptions are one type, for what is interned: a scalar, a vector, an array, an image, a sampler.
[[nodiscard]] bool is_same(ir::type const& a, ir::type const& b)
{
    return a.kind == b.kind && a.width == b.width && a.is_signed == b.is_signed && a.count == b.count
        && a.element == b.element && a.stride == b.stride && a.space == b.space && a.dimension == b.dimension
        && a.is_arrayed == b.is_arrayed && a.is_multisampled == b.is_multisampled
        && a.is_storage_image == b.is_storage_image && a.is_depth == b.is_depth && a.format == b.format;
}
} // namespace

type_ref lowering::intern(ir::type const& t)
{
    for (auto i = isize(0); i < _out.types.size(); ++i)
        if (_out.types[i].kind != type_kind::structure && is_same(_out.types[i], t))
            return type_ref(i);
    _out.types.push_back(t);
    return type_ref(_out.types.size() - 1);
}

type_ref lowering::scalar_type(value_kind kind)
{
    switch (kind)
    {
    case value_kind::scalar_float:
        return _float;
    case value_kind::scalar_int:
        return _int;
    case value_kind::scalar_uint:
        return _uint;
    case value_kind::boolean:
        return _bool;
    case value_kind::scalar_half:
        return intern({.kind = type_kind::float_, .width = 16});
    case value_kind::scalar_double:
        return intern({.kind = type_kind::float_, .width = 64});
    case value_kind::scalar_short:
        return intern({.kind = type_kind::int_, .width = 16, .is_signed = true});
    case value_kind::scalar_ushort:
        return intern({.kind = type_kind::int_, .width = 16});
    case value_kind::scalar_long:
        return intern({.kind = type_kind::int_, .width = 64, .is_signed = true});
    case value_kind::scalar_ulong:
        return intern({.kind = type_kind::int_, .width = 64});
    case value_kind::none:
        break;
    }
    return type_ref::none;
}

type_ref lowering::vector_type(type_ref element, u32 lanes)
{
    return lanes == 1 ? element : intern({.kind = type_kind::vector, .count = lanes, .element = element});
}

type_ref lowering::pointer_type(type_ref element, storage space)
{
    auto const at = isize(element) * k_storage_classes + isize(space);
    if (at >= _pointers.size())
        _pointers.resize_to_defaulted(at + k_storage_classes * 8);
    // a defaulted entry is zero, which is `void` and never a pointer
    if (auto const known = _pointers[at]; known != type_ref(0))
        return known;
    _out.types.push_back({.kind = type_kind::pointer, .space = space, .element = element});
    auto const result = type_ref(_out.types.size() - 1);
    _pointers[at] = result;
    return result;
}

type_ref lowering::array_type(type_ref element, u32 count, i32 stride)
{
    return intern({.kind = type_kind::array, .count = count, .element = element, .stride = stride});
}

type_ref lowering::runtime_array_type(type_ref element, i32 stride)
{
    return intern({.kind = type_kind::runtime_array, .element = element, .stride = stride});
}

type_ref lowering::image_type(check::type_info const& t)
{
    auto const axes = builtins::texture::axes_of(t.shape);
    auto description = ir::type{.kind = type_kind::image,
                                .dimension = u8(axes.is_cube ? 3 : axes.dim - 1),
                                .is_arrayed = axes.is_array,
                                .is_multisampled = axes.is_ms,
                                .is_depth = t.is_depth};
    if (t.kind == check::type_kind::image)
    {
        auto const& format = k_image_formats[t.format];
        if (format.spirv.empty())
        {
            unsupported(cc::format("the image format {}, which SPIR-V does not name", format.name));
            return type_ref::none;
        }
        description.is_storage_image = true;
        description.format = i16(t.format);
        description.element = scalar_type(format.component);
    }
    else if (t.is_depth)
        description.element = _float;
    else if (auto const* const texel = _m.builtin_type_of(t.element))
        description.element = scalar_type(texel->leaf_kind);
    return intern(description);
}

type_ref lowering::value_type(type_id t)
{
    if (t == checked_module::void_type)
        return _void;
    if (!check::is_valid(t))
        return type_ref::none;
    auto& memo = _value_types[index_of(t)];
    if (ir::is_valid(memo))
        return memo;

    auto result = type_ref::none;
    auto const& info = _m.at(t);
    if (auto const* const record = _m.builtin_type_of(t))
    {
        auto const scalar = scalar_type(record->leaf_kind);
        auto const lanes = record->leaf_count;
        if (record->name == "ray_query")
            result = intern({.kind = type_kind::ray_query});
        else if (record->name == "hit_object")
            result = intern({.kind = type_kind::hit_object});
        // three rows, held as the columns of a matrix, so a row is a column and nothing transposes
        else if (record->name == "affine3")
            result = intern({.kind = type_kind::matrix, .count = 3, .element = vector_type(_float, 4)});
        else if (record->name.starts_with("mat") && ir::is_valid(scalar))
        {
            auto const columns = record->name == "mat2" ? 2u : record->name == "mat3" ? 3u : 4u;
            result = intern({.kind = type_kind::matrix, .count = columns, .element = vector_type(scalar, columns)});
        }
        // a bool as GPU memory holds one: a `uint` of 0 or 1, which only `as` makes or reads
        else if (record->name == "bool32")
            result = _uint;
        else if (ir::is_valid(scalar) && lanes >= 1 && lanes <= 4)
            result = vector_type(scalar, u32(lanes));
        else
            unsupported(cc::format("the type {}", record->name));
    }
    else
        switch (info.kind)
        {
        case check::type_kind::enumeration:
            result = _int;
            break;
        case check::type_kind::array:
            if (auto const element = value_type(info.element); ir::is_valid(element))
                result = array_type(element, u32(info.count), 0);
            break;
        case check::type_kind::atomic:
            result = value_type(info.element);
            if (result == _float && !_plan.native_float_atomics)
                result = _uint;
            break;
        case check::type_kind::structure:
        {
            if (info.is_text_type || info.is_opaque)
            {
                unsupported(cc::format("the type {}", _m.name_of(t)));
                break;
            }
            auto const declared
                = cc::vector<member_info>::create_copy_of(cc::span<member_info const>(_m.at(info.members)));
            auto parts = cc::vector<ir::member>();
            for (auto const& m : declared)
            {
                if (m.type == checked_module::void_type)
                    continue;
                auto const name_first = add_name(m.name);
                parts.push_back({.type = value_type(m.type), .name_first = name_first, .name_size = u32(m.name.size())});
            }
            auto const first = u32(_out.members.size());
            for (auto const& part : parts)
                _out.members.push_back(part);
            auto const name = _m.name_of(t);
            auto const name_first = add_name(name);
            _out.types.push_back({.kind = type_kind::structure,
                                  .first_member = first,
                                  .member_count = u32(parts.size()),
                                  .name_first = name_first,
                                  .name_size = u32(name.size())});
            result = type_ref(_out.types.size() - 1);
            break;
        }
        default:
            unsupported(cc::format("the type {}", _m.name_of(t)));
            break;
        }
    _value_types[index_of(t)] = result;
    return result;
}

i32 lowering::column_stride(type_id t, address_space space)
{
    auto at = t;
    while (check::is_valid(at) && _m.builtin_type_of(at) == nullptr && _m.at(at).kind == check::type_kind::array)
        at = _m.at(at).element;
    auto const* const record = check::is_valid(at) ? _m.builtin_type_of(at) : nullptr;
    // CHK-518: an affine3's rows, held as the columns of its value, stand 16 bytes apart
    if (record != nullptr && record->name == "affine3")
        return 16;
    if (record == nullptr || !record->name.starts_with("mat"))
        return 0;
    auto const columns = record->name == "mat2" ? 2 : record->name == "mat3" ? 3 : 4;
    return emit::impl::memory_size_of(*record, space) / columns;
}

type_ref lowering::memory_type(type_id t, address_space space)
{
    if (_m.builtin_type_of(t) != nullptr)
        return value_type(t);
    auto const& info = _m.at(t);
    if (info.kind == check::type_kind::atomic || info.kind == check::type_kind::enumeration)
        return value_type(t);
    for (auto const& known : _memory_types)
        if (known.type == t && known.space == space)
            return known.ir;

    auto result = type_ref::none;
    if (info.kind == check::type_kind::array)
    {
        auto const element = memory_type(info.element, space);
        result = array_type(element, u32(info.count), emit::impl::element_stride(_m, info.element, space));
    }
    else if (info.kind == check::type_kind::structure)
    {
        auto const declared = cc::vector<member_info>::create_copy_of(cc::span<member_info const>(_m.at(info.members)));
        auto const placed = emit::impl::place_struct(_m, t, space);
        auto parts = cc::vector<ir::member>();
        for (auto i = isize(0); i < declared.size(); ++i)
        {
            auto const& m = declared[i];
            if (m.type == checked_module::void_type)
                continue;
            auto part = memory_type(m.type, space);
            // an array member strides as its place says, which a member's own padding may widen
            if (auto const& held = _m.at(m.type);
                _m.builtin_type_of(m.type) == nullptr && held.kind == check::type_kind::array && held.count > 0)
                part = array_type(memory_type(held.element, space), u32(held.count), placed.sizes[i] / held.count);
            auto const name_first = add_name(m.name);
            parts.push_back({.type = part,
                             .offset = placed.offsets[i],
                             .matrix_stride = column_stride(m.type, space),
                             .name_first = name_first,
                             .name_size = u32(m.name.size())});
        }
        auto const first = u32(_out.members.size());
        for (auto const& part : parts)
            _out.members.push_back(part);
        auto const name = _m.name_of(t);
        auto const name_first = add_name(name);
        _out.types.push_back({.kind = type_kind::structure,
                              .first_member = first,
                              .member_count = u32(parts.size()),
                              .name_first = name_first,
                              .name_size = u32(name.size())});
        result = type_ref(_out.types.size() - 1);
    }
    else
        unsupported(cc::format("{} in GPU memory", _m.name_of(t)));
    _memory_types.push_back({.type = t, .space = space, .ir = result});
    return result;
}

type_ref lowering::block_of_members(cc::string_view name,
                                    cc::span<emit::impl::planned_member const> planned,
                                    address_space space)
{
    auto parts = cc::vector<ir::member>();
    for (auto const& m : planned)
    {
        auto part = memory_type(m.type, space);
        if (auto const& held = _m.at(m.type);
            _m.builtin_type_of(m.type) == nullptr && held.kind == check::type_kind::array && held.count > 0)
            part = array_type(memory_type(held.element, space), u32(held.count), m.size / held.count);
        auto const name_first = add_name(m.name);
        parts.push_back({.type = part,
                         .offset = m.offset,
                         .matrix_stride = column_stride(m.type, space),
                         .name_first = name_first,
                         .name_size = u32(m.name.size())});
    }
    auto const first = u32(_out.members.size());
    for (auto const& part : parts)
        _out.members.push_back(part);
    auto const name_first = add_name(name);
    _out.types.push_back({.kind = type_kind::structure,
                          .is_block = true,
                          .first_member = first,
                          .member_count = u32(parts.size()),
                          .name_first = name_first,
                          .name_size = u32(name.size())});
    return type_ref(_out.types.size() - 1);
}

value_ref lowering::add_global(global g, cc::string_view name)
{
    g.name_first = add_name(name);
    g.name_size = u32(name.size());
    _out.globals.push_back(g);
    return global_at(u32(_out.globals.size() - 1));
}

value_ref lowering::add_local(type_ref value, cc::string_view name)
{
    auto const name_first = add_name(name);
    _out.locals.push_back(
        {.type = pointer_type(value, storage::function), .name_first = name_first, .name_size = u32(name.size())});
    return local_at(u32(_out.locals.size() - 1));
}

value_ref lowering::bound_block(emit::impl::planned_constants const& block)
{
    auto const is_inline = block.group < 0;
    auto index = isize(-1);
    for (auto i = isize(0); i < _plan.group_blocks.size(); ++i)
        if (&_plan.group_blocks[i] == &block)
            index = i;
    auto& known = is_inline ? _constants_global : _block_globals[index];
    if (ir::is_valid(known))
        return known;
    auto const is_packed
        = block.symbol != symbol_id::none
       && emit::impl::block_space(_m.bindings[_m.at(block.symbol).info]) == address_space::packed_constants;
    auto const space = is_packed ? address_space::packed_constants : address_space::constants;
    auto const pointee = block_of_members(block.block_name, block.members, space);
    known = add_global({.type = pointer_type(pointee, is_inline ? storage::push_constant : storage::uniform),
                        .kind = is_inline ? global_kind::inline_constants : global_kind::constants,
                        .set = block.group,
                        .binding = is_inline ? -1 : block.slot,
                        .is_read_only = true},
                       block.name);
    return known;
}

value_ref lowering::bound_resource(i32 index)
{
    auto& known = _resource_globals[index];
    if (ir::is_valid(known))
        return known;
    auto const& r = _plan.resources[index];
    auto const is_array = _m.at(r.type).kind == check::type_kind::array;
    auto const& info = _m.at(is_array ? _m.at(r.type).element : r.type);
    auto pointee = type_ref::none;
    auto space = storage::resource;
    switch (info.kind)
    {
    case check::type_kind::buffer:
    case check::type_kind::bytes:
    {
        auto const is_bytes = info.kind == check::type_kind::bytes;
        auto const element = is_bytes ? _uint : memory_type(r.element, r.space);
        auto const stride = is_bytes ? 4 : emit::impl::element_stride(_m, r.element, r.space);
        auto const name_first = add_name("data");
        _out.members.push_back({.type = runtime_array_type(element, stride),
                                .offset = 0,
                                .matrix_stride = is_bytes ? 0 : column_stride(r.element, r.space),
                                .name_first = name_first,
                                .name_size = 4});
        auto const block_name = add_name(r.name);
        _out.types.push_back({.kind = type_kind::structure,
                              .is_block = true,
                              .first_member = u32(_out.members.size() - 1),
                              .member_count = 1,
                              .name_first = block_name,
                              .name_size = u32(r.name.size())});
        pointee = type_ref(_out.types.size() - 1);
        space = storage::storage_buffer;
        break;
    }
    case check::type_kind::texture:
    case check::type_kind::image:
        pointee = image_type(info);
        break;
    case check::type_kind::sampler:
        pointee = intern({.kind = type_kind::sampler});
        break;
    case check::type_kind::acceleration_structure:
        pointee = intern({.kind = type_kind::acceleration_structure});
        break;
    default:
        unsupported(cc::format("the resource {}", r.host_name));
        return value_ref::none;
    }
    auto const is_image = info.kind == check::type_kind::image;
    auto const writes
        = (info.kind != check::type_kind::image && r.is_mut) || (is_image && info.access != access_mode::read);
    auto kind = global_kind::buffer;
    switch (info.kind)
    {
    case check::type_kind::bytes:
        kind = global_kind::bytes;
        break;
    case check::type_kind::texture:
        kind = global_kind::texture;
        break;
    case check::type_kind::image:
        kind = global_kind::image;
        break;
    case check::type_kind::sampler:
        kind = global_kind::sampler;
        break;
    case check::type_kind::acceleration_structure:
        kind = global_kind::acceleration_structure;
        break;
    default:
        break;
    }
    if (is_array || r.count != 1)
        pointee = r.count == 0 ? runtime_array_type(pointee, 0)
                               : intern({.kind = type_kind::array, .count = u32(r.count), .element = pointee});
    known = add_global({.type = pointer_type(pointee, space),
                        .kind = kind,
                        .set = r.group,
                        .binding = r.slot,
                        .is_read_only = !writes,
                        .is_write_only = is_image && info.access == access_mode::write,
                        .is_coherent = r.is_coherent,
                        .is_comparison = info.kind == check::type_kind::sampler && info.is_comparison},
                       r.name);
    return known;
}

value_ref lowering::bound_workgroup(i32 index)
{
    auto& known = _workgroup_globals[index];
    if (ir::is_valid(known))
        return known;
    auto const& w = _plan.workgroup[index];
    known = add_global({.type = pointer_type(value_type(w.type), storage::workgroup)}, w.name);
    return known;
}

value_ref lowering::bound_sampler(i32 index)
{
    auto& known = _sampler_globals[index];
    if (ir::is_valid(known))
        return known;
    auto const& s = _plan.samplers[index];
    // the reserved set, whose binding 0 no static sampler takes (EMIT-133)
    known = add_global({.type = pointer_type(intern({.kind = type_kind::sampler}), storage::resource),
                        .kind = global_kind::file_sampler,
                        .set = 3,
                        .binding = s.index + 1,
                        .is_read_only = true,
                        .is_comparison = _m.at(s.type).is_comparison},
                       s.name);
    return known;
}

cc::optional<place> lowering::chain_of(flat_expr_id id, cc::vector<u32>& chain)
{
    auto const& x = _e.at(id);
    if (auto const* const l = x.node.try_as<flat_local_ref>())
    {
        if (!_is_variable[index_of(l->local)])
            return {};
        return place{.pointer = _locals[index_of(l->local)], .type = x.type};
    }
    if (auto const* const b = x.node.try_as<flat_binding_member>())
    {
        if (auto const found = emit::impl::workgroup_of(_plan, b->binding, b->member); found >= 0)
            return place{.pointer = bound_workgroup(found), .type = x.type};
        if (emit::impl::resource_of(_plan, b->binding, b->member) >= 0)
            return {};
        auto const* const block = emit::impl::block_of(_plan, b->binding);
        if (block == nullptr)
            return {};
        chain.push_back(u32(uint_constant(u32(block->block_member_of[b->member]))));
        return place{.pointer = bound_block(*block), .type = x.type};
    }
    if (auto const* const w = x.node.try_as<flat_written_word>())
    {
        chain.push_back(u32(expr(w->word)));
        return place{.pointer = written_shadow(), .type = x.type};
    }
    if (auto const* const e = x.node.try_as<flat_buffer_element>())
    {
        auto const* const b = _e.at(e->buffer).node.try_as<flat_binding_member>();
        if (b == nullptr)
        {
            auto const element = bound_element(e->buffer);
            if (!ir::is_valid(element))
            {
                unsupported("a buffer of a binding array");
                return {};
            }
            chain.push_back(u32(uint_constant(0)));
            chain.push_back(u32(expr(e->index)));
            return place{.pointer = element, .type = x.type};
        }
        auto const found = emit::impl::resource_of(_plan, b->binding, b->member);
        chain.push_back(u32(uint_constant(0)));
        chain.push_back(u32(expr(e->index)));
        return place{.pointer = bound_resource(found), .type = x.type};
    }
    if (auto const* const m = x.node.try_as<flat_member>(); m != nullptr && !m->is_swizzle())
    {
        auto object = chain_of(m->object, chain);
        if (!object.has_value())
            return {};
        chain.push_back(u32(uint_constant(u32(position_in(_e.at(m->object).type, m->member)))));
        return place{.pointer = object.value().pointer, .type = x.type};
    }
    if (auto const* const a = x.node.try_as<flat_element>())
    {
        auto object = chain_of(a->object, chain);
        if (!object.has_value())
            return {};
        chain.push_back(u32(expr(a->index)));
        return place{.pointer = object.value().pointer, .type = x.type};
    }
    return {};
}

cc::optional<place> lowering::texel_of(flat_expr_id image, value_ref coordinate)
{
    auto const* const bound = _e.at(image).node.try_as<flat_binding_member>();
    auto const resource = bound != nullptr
                            ? bound_resource(emit::impl::resource_of(_plan, bound->binding, bound->member))
                            : bound_element(image);
    if (!ir::is_valid(resource))
    {
        unsupported("a texel of a binding array");
        return {};
    }
    auto const held = _out.at(_out.at(_out.type_of(resource)).element).element;
    auto const pointer = add(op::texel_pointer, pointer_type(held, storage::image),
                             {resource, coordinate, uint_constant(0)}, 0, nonuniform_of(resource));
    return place{.pointer = pointer, .type = _m.at(_e.at(image).type).element};
}

cc::optional<place> lowering::place_of(flat_expr_id id)
{
    auto chain = cc::vector<u32>();
    auto base = chain_of(id, chain);
    if (!base.has_value())
        return {};
    if (chain.empty())
        return base;

    // A member of memory has the type its layout gives it, which the pointer has to name.
    auto at = _out.at(_out.type_of(base.value().pointer)).element;
    auto const space = _out.at(_out.type_of(base.value().pointer)).space;
    for (auto const raw : chain)
    {
        auto const t = _out.at(at);
        switch (t.kind)
        {
        case type_kind::structure:
            at = _out.members[t.first_member + _out.constants[index_of(value_ref(raw))].low].type;
            break;
        case type_kind::array:
        case type_kind::runtime_array:
        case type_kind::vector:
        case type_kind::matrix:
            at = t.element;
            break;
        default:
            unsupported("an index into a scalar");
            return {};
        }
    }
    auto operands = cc::vector<u32>::create_with_capacity(chain.size() + 1);
    operands.push_back(u32(base.value().pointer));
    operands.push_back_range(chain);
    return place{
        .pointer = add(op::access_chain, pointer_type(at, space), operands, 0, nonuniform_of(base.value().pointer)),
        .type = base.value().type};
}

bool lowering::is_nonuniform(flat_expr_id id) const
{
    auto const* const c = _e.at(id).node.try_as<flat_call>();
    auto const* const record = c != nullptr ? _m.builtin_function(c->intrinsic) : nullptr;
    return record != nullptr && record->mark == builtins::index_mark::nonuniform;
}

value_ref lowering::bound_element(flat_expr_id id)
{
    auto const* const element = _e.at(id).node.try_as<flat_element>();
    auto const* const bound = element != nullptr ? _e.at(element->object).node.try_as<flat_binding_member>() : nullptr;
    if (bound == nullptr)
        return value_ref::none;
    auto const found = emit::impl::resource_of(_plan, bound->binding, bound->member);
    if (found < 0)
        return value_ref::none;
    auto const array = bound_resource(found);
    if (!ir::is_valid(array))
        return value_ref::none;
    auto const pointer = _out.at(_out.type_of(array));
    auto const held = _out.at(pointer.element).element;
    auto const space = pointer.space;
    u32 const operands[] = {u32(array), u32(expr(element->index))};
    return add(op::access_chain, pointer_type(held, space), operands, 0,
               is_nonuniform(element->index) ? k_ir_nonuniform : u8(0));
}

value_ref lowering::uniform_element_length(flat_expr_id id)
{
    // TEMPORARY(debt-sgl-vulkan-element-length-asked-at-uniform-indices): AMD's vulkan driver answers
    // OpArrayLength of a non-uniform element with the first lane's element, so each length is asked at a uniform index.
    if (_plan.which != emit::target::hlsl_vulkan)
        return value_ref::none;
    auto const* const element = _e.at(id).node.try_as<flat_element>();
    if (element == nullptr || !is_nonuniform(element->index))
        return value_ref::none;
    auto const* const bound = _e.at(element->object).node.try_as<flat_binding_member>();
    if (bound == nullptr)
        return value_ref::none;
    auto const found = emit::impl::resource_of(_plan, bound->binding, bound->member);
    if (found < 0)
        return value_ref::none;
    auto const array = bound_resource(found);
    if (!ir::is_valid(array))
        return value_ref::none;
    auto const pointer = _out.at(_out.type_of(array));
    auto const elements = _out.at(pointer.element);
    auto const held = elements.element;
    auto const space = pointer.space;
    auto const is_fixed = elements.kind == type_kind::array && elements.count <= 16;
    if (!is_fixed && _e.entry_stage != stage::compute && _e.entry_stage != stage::pixel)
        return value_ref::none;

    auto const index = expr(element->index);
    auto const index_type = _out.type_of(index);
    auto const length_at = [&](value_ref at)
    {
        u32 const chain[] = {u32(array), u32(at)};
        auto const p = add(op::access_chain, pointer_type(held, space), chain);
        u32 const operands[] = {u32(p), 0u};
        return add(op::array_length, _uint, operands);
    };
    if (is_fixed)
    {
        auto const constant = [&](u32 k) { return index_type == _uint ? uint_constant(k) : int_constant(i32(k)); };
        auto length = length_at(constant(0));
        for (auto k = u32(1); k < elements.count; ++k)
            length = add(op::select, _uint, {add(op::ieq, _bool, {index, constant(k)}), length_at(constant(k)), length});
        return length;
    }

    // a waterfall: each turn answers the lanes whose index the first lane still asking holds
    auto const variable = add_local(_uint, "element_length");
    auto const header = new_block();
    auto const body = new_block();
    auto const next = new_block();
    auto const merge = new_block();
    branch(header);
    open_block(header);
    set_merge(merge_kind::loop, merge, next);
    branch(body);
    open_block(body);
    auto const first = add(op::subgroup, index_type, {index}, u8(subgroup::broadcast_first));
    add(op::store, type_ref::none, {variable, length_at(first)});
    branch_if(add(op::ieq, _bool, {index, first}), merge, next);
    open_block(next);
    branch(header);
    open_block(merge);
    return add(op::load, _uint, {variable});
}

u8 lowering::nonuniform_of(value_ref pointer) const
{
    if (!ir::is_valid(pointer) || is_constant(pointer) || is_global(pointer) || is_local(pointer))
        return 0;
    auto const& i = _out.instructions[index_of(pointer)];
    return i.code == op::access_chain ? i.aux : u8(0);
}

value_ref lowering::load(place const& p)
{
    auto const held = _out.at(_out.type_of(p.pointer)).element;
    auto const wanted = value_type(p.type);
    auto const loaded = add(op::load, held, {p.pointer});
    return held == wanted ? loaded : add(op::copy_logical, wanted, {loaded});
}

void lowering::store(place const& p, value_ref value)
{
    auto const held = _out.at(_out.type_of(p.pointer)).element;
    auto const stored = held == value_type(p.type) ? value : add(op::copy_logical, held, {value});
    add(op::store, type_ref::none, {p.pointer, stored});
}

value_ref lowering::spilled(value_ref value, type_id type)
{
    auto const variable = add_local(value_type(type), "spilled");
    add(op::store, type_ref::none, {variable, value});
    return variable;
}

bool lowering::holds_array(type_ref t) const
{
    auto const info = _out.at(t);
    if (info.kind == type_kind::array)
        return true;
    if (info.kind != type_kind::structure)
        return false;
    for (auto i = u32(0); i < info.member_count; ++i)
        if (holds_array(_out.members[info.first_member + i].type))
            return true;
    return false;
}
