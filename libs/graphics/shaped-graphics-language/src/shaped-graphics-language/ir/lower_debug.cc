#include <clean-core/string/format.hh>
#include <shaped-graphics-language/ir/impl/lowering.hh>

// EMIT-183 below a text: the same record in the same buffer at the same address, so sg cannot tell which route wrote it.

using namespace sgl;
using namespace sgl::check;
using namespace sgl::ir;
using namespace sgl::ir::impl;

value_ref lowering::bound_debug()
{
    if (ir::is_valid(_debug_global))
        return _debug_global;
    auto const name_first = add_name("data");
    _out.members.push_back({.type = runtime_array_type(_uint, 4), .offset = 0, .name_first = name_first, .name_size = 4});
    auto const block_name = add_name("sg_debug");
    _out.types.push_back({.kind = type_kind::structure,
                          .is_block = true,
                          .first_member = u32(_out.members.size() - 1),
                          .member_count = 1,
                          .name_first = block_name,
                          .name_size = 8});
    auto const is_vulkan = _plan.which == emit::target::hlsl_vulkan;
    _debug_global = add_global({.type = pointer_type(type_ref(_out.types.size() - 1), storage::storage_buffer),
                                .kind = global_kind::bytes,
                                .set = is_vulkan ? emit::impl::k_debug_vulkan_set : emit::impl::k_debug_dx12_space,
                                .binding = is_vulkan ? emit::impl::k_debug_vulkan_binding : 0},
                               "sg_debug");
    return _debug_global;
}

value_ref lowering::debug_word(value_ref lane, cc::string_view family)
{
    if (family == "bool")
        return add(op::select, _uint, {lane, uint_constant(1), uint_constant(0)});
    auto const type = _out.type_of(lane);
    auto const& held = _out.at(type);
    if (held.width == 16 && held.kind == type_kind::float_)
        return add(op::bitcast, _uint, {add(op::f_resize, _float, {lane})});
    if (held.width != 32)
    {
        unsupported(cc::format("a print of {}", family));
        return value_ref::none;
    }
    return type == _uint ? lane : add(op::bitcast, _uint, {lane});
}

void lowering::print_(flat_stmt_id id, flat_print const& n)
{
    // a stage that cannot write the buffer drops its prints, values and all, as the text does
    auto const known = _plan.debug_site_of[isize(index_of(id))];
    if (known < 0)
        return;
    auto const& site = _plan.debug_sites[known];

    auto words = cc::vector<value_ref>();
    auto position = isize(0);
    for (auto const value_id : _e.at(n.values))
    {
        auto const& shape = site.values[position++];
        auto const value = expr(value_id);
        auto const scalar = scalar_of(_out.type_of(value));
        for (auto lane = 0; lane < shape.lanes; ++lane)
            words.push_back(debug_word(shape.lanes == 1 ? value : extracted(value, scalar, u32(lane)), shape.family));
    }
    if (!_failure.empty())
        return;

    auto const buffer = bound_debug();
    auto const pointer_to = pointer_type(_uint, storage::storage_buffer);
    auto const word_at
        = [&](value_ref index) { return add(op::access_chain, pointer_to, {buffer, uint_constant(0), index}); };
    auto const count = u32(words.size()) + 2;
    auto const at = add(op::atomic_iadd, _uint, {word_at(uint_constant(0)), uint_constant(count)});
    auto const end = add(op::iadd, _uint, {at, uint_constant(count)});
    auto const capacity = add(op::load, _uint, {word_at(uint_constant(1))});
    auto const fits = add(op::ule, _bool, {end, capacity});

    auto const then_block = new_block();
    auto const else_block = new_block();
    auto const merge = new_block();
    set_merge(merge_kind::selection, merge);
    branch_if(fits, then_block, else_block);

    open_block(then_block);
    // the header's three words stand ahead of the first record
    auto const first = add(op::iadd, _uint, {at, uint_constant(3)});
    auto const word_of_record
        = [&](u32 i) { return word_at(i == 0 ? first : add(op::iadd, _uint, {first, uint_constant(i)})); };
    add(op::store, type_ref::none, {word_of_record(0), uint_constant(site.id)});
    add(op::store, type_ref::none, {word_of_record(1), uint_constant(count)});
    for (auto i = isize(0); i < words.size(); ++i)
        add(op::store, type_ref::none, {word_of_record(u32(i) + 2), words[i]});
    branch(merge);

    open_block(else_block);
    add(op::atomic_iadd, _uint, {word_at(uint_constant(2)), uint_constant(1)});
    branch(merge);

    open_block(merge);
}
