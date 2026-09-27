#include "memory_form.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/emit/impl/plan.hh>
#include <shaped-graphics-language/emit/reserved_words.hh>

namespace
{
using namespace sgl;
using namespace sgl::check;
using namespace sgl::emit;
using namespace sgl::emit::impl;

i32 round_up(i32 value, i32 alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

i32 max_of(i32 a, i32 b)
{
    return a > b ? a : b;
}

bool is_matrix(builtins::type_record const& record)
{
    return record.leaf_count > 4;
}

/// A builtin's size and alignment under `t`'s own rule.
builtins::block_layout native_layout(builtins::type_record const& record, emit::target t)
{
    return t == emit::target::msl ? record.msl_layout : record.wgsl_layout;
}

/// Where `t`'s own rule places a value, and how large and how aligned the value is there.
struct native_placement
{
    i32 size = 0;
    i32 alignment = 4;
};

/// Places `members` by `t`'s own rule from `base`, appending the offset of every builtin value in memory order.
/// WGSL's uniform rule aligns a nested struct at 16 and keeps what follows it a row-rounded size away; MSL has neither.
native_placement place_natively(checked_module const& m,
                                cc::span<member_info const> members,
                                address_space space,
                                emit::target t,
                                i32 base,
                                cc::vector<i32>& offsets)
{
    auto at = 0;
    auto alignment = 4;
    for (auto const& member : members)
    {
        if (member.type == checked_module::void_type)
            continue;
        if (auto const* const record = m.builtin_type_of(member.type))
        {
            auto const l = native_layout(*record, t);
            at = round_up(at, l.alignment);
            offsets.push_back(base + at);
            at += l.size;
            alignment = max_of(alignment, l.alignment);
            continue;
        }
        // The struct's own alignment first, which only a dry run over its members gives.
        auto dry = cc::vector<i32>();
        auto const inner_members = m.at(m.at(member.type).members);
        auto inner = place_natively(m, inner_members, space, t, 0, dry);
        auto const is_uniform = t == emit::target::wgsl && space == address_space::constants;
        auto const inner_alignment = is_uniform ? round_up(inner.alignment, 16) : inner.alignment;
        at = round_up(at, inner_alignment);
        place_natively(m, inner_members, space, t, base + at, offsets);
        at += is_uniform ? round_up(inner.size, 16) : inner.size;
        alignment = max_of(alignment, inner_alignment);
    }
    return {.size = round_up(at, alignment), .alignment = alignment};
}

cc::string_view component_name(i32 i)
{
    constexpr cc::string_view names[] = {"x", "y", "z", "w"};
    return names[i];
}

/// The scalar of a builtin as `t` spells it.
cc::string_view scalar_spelling(builtins::type_record const& record, emit::target t)
{
    auto const is_wgsl = t == emit::target::wgsl;
    switch (record.leaf_kind)
    {
    case value_kind::scalar_int:
        return is_wgsl ? "i32" : "int";
    case value_kind::scalar_uint:
        return is_wgsl ? "u32" : "uint";
    default:
        return is_wgsl ? "f32" : "float";
    }
}

struct form_builder
{
    checked_module const& m;
    emit::target t;
    memory_form form;
    i32 at = 0;
    i32 padding = 0;
    /// Every vector and matrix is split, which is what lowers the form's alignment to 4 for a buffer's stride.
    bool is_forced = false;

    cc::string unique(cc::string name)
    {
        if (is_reserved(t, name))
            name += "_";
        auto is_taken = true;
        while (is_taken)
        {
            is_taken = false;
            for (auto const& f : form.fields)
                is_taken = is_taken || f.name == name;
            if (is_taken)
                name += "_";
        }
        return name;
    }

    i32 add_field(cc::string name, cc::string type, i32 size)
    {
        form.fields.push_back({.name = unique(cc::move(name)), .type = cc::move(type), .offset = at});
        at += size;
        return i32(form.fields.size() - 1);
    }

    void pad_to(i32 offset)
    {
        while (at < offset)
            add_field(cc::format("_pad{}", padding++), cc::string(t == emit::target::wgsl ? "u32" : "uint"), 4);
    }

    /// The source names of the members along `path`, joined by `_`: `light_dir`.
    cc::string stem_of(cc::span<member_info const> members, cc::span<i32 const> path, cc::string_view fallback)
    {
        auto result = cc::string();
        auto level = members;
        for (auto const step : path)
        {
            auto const& member = level[step];
            if (!result.empty())
                result += "_";
            result += member.name;
            if (m.builtin_type_of(member.type) == nullptr)
                level = m.at(m.at(member.type).members);
        }
        return result.empty() ? cc::string(fallback) : result;
    }

    void add_leaf(placed_leaf const& leaf, cc::string const& stem)
    {
        auto const& record = *m.builtin_type_of(leaf.type);
        pad_to(leaf.offset);
        auto result = memory_leaf{.path = leaf.path, .type = leaf.type};
        auto const l = native_layout(record, t);
        auto const scalar = scalar_spelling(record, t);

        if (record.leaf_count == 1)
            result.fields.push_back(add_field(stem, cc::string(record.spelled_in(language_of(t))), 4));
        else if (!is_forced && leaf.offset % l.alignment == 0 && !(t == emit::target::msl && record.leaf_count == 3))
            result.fields.push_back(add_field(stem, cc::string(record.spelled_in(language_of(t))), l.size));
        else if (t == emit::target::msl && !is_matrix(record))
        {
            // MSL's packed vector sits at any 4-byte offset and takes exactly its scalars' bytes.
            result.is_packed = true;
            result.fields.push_back(
                add_field(stem, cc::format("packed_{}", record.spelled_in(language_of(t))), record.leaf_count * 4));
        }
        else
        {
            result.is_split = true;
            for (auto i = 0; i < record.leaf_count; ++i)
            {
                auto const name
                    = is_matrix(record) ? cc::format("{}_{}", stem, i) : cc::format("{}_{}", stem, component_name(i));
                result.fields.push_back(add_field(name, cc::string(scalar), 4));
            }
        }
        form.leaves.push_back(cc::move(result));
    }
};

/// The largest alignment a field of `form` has under `t`'s rule, which is what an array of it strides by.
i32 alignment_of(checked_module const& m, memory_form const& form, emit::target t)
{
    auto result = 4;
    for (auto const& leaf : form.leaves)
        if (!leaf.is_split && !leaf.is_packed)
            result = max_of(result, native_layout(*m.builtin_type_of(leaf.type), t).alignment);
    return result;
}

memory_form build(checked_module const& m,
                  cc::span<member_info const> members,
                  cc::span<placed_leaf const> leaves,
                  i32 size,
                  i32 stride,
                  cc::string_view element_name,
                  emit::target t,
                  bool is_forced)
{
    auto b = form_builder{.m = m, .t = t, .form = {}, .is_forced = is_forced};
    for (auto const& leaf : leaves)
        b.add_leaf(leaf, b.stem_of(members, leaf.path, element_name));
    b.pad_to(stride > 0 ? stride : size);
    return cc::move(b.form);
}

bool is_natively_placed(checked_module const& m,
                        cc::span<member_info const> members,
                        cc::span<placed_leaf const> leaves,
                        address_space space,
                        i32 stride,
                        emit::target t)
{
    auto offsets = cc::vector<i32>();
    auto const native = place_natively(m, members, space, t, 0, offsets);
    if (offsets.size() != leaves.size())
        return false;
    for (auto i = isize(0); i < leaves.size(); ++i)
        if (offsets[i] != leaves[i].offset)
            return false;
    // MSL's float3 takes 16 bytes, so the one value after it has to start past them as well.
    if (t == emit::target::msl)
        for (auto i = isize(0); i + 1 < leaves.size(); ++i)
            if (m.builtin_type_of(leaves[i].type)->leaf_count == 3 && leaves[i + 1].offset < leaves[i].offset + 16)
                return false;
    return stride == 0 || native.size == stride;
}

cc::optional<memory_form> form_for(checked_module const& m,
                                   cc::span<member_info const> members,
                                   cc::span<placed_leaf const> leaves,
                                   address_space space,
                                   i32 size,
                                   i32 stride,
                                   cc::string_view element_name,
                                   emit::target t)
{
    if (t != emit::target::wgsl && t != emit::target::msl)
        return {};
    if (is_natively_placed(m, members, leaves, space, stride, t))
        return {};
    auto form = build(m, members, leaves, size, stride, element_name, t, false);
    // An array's stride is its element's size rounded to the element's alignment, so a vector field whose alignment
    // does not divide the stride has to become scalars too.
    if (stride > 0 && round_up(stride, alignment_of(m, form, t)) != stride)
        form = build(m, members, leaves, size, stride, element_name, t, true);
    return form;
}
} // namespace

cc::optional<sgl::emit::impl::memory_form> sgl::emit::impl::memory_form_of(check::checked_module const& m,
                                                                           cc::span<check::member_info const> members,
                                                                           address_space space,
                                                                           i32 stride,
                                                                           emit::target t)
{
    auto const placed = place(m, members, space);
    return form_for(m, members, placed.leaves, space, placed.size, stride, "value", t);
}

cc::optional<sgl::emit::impl::memory_form> sgl::emit::impl::element_form_of(check::checked_module const& m,
                                                                            check::type_id element,
                                                                            emit::target t)
{
    auto const stride = element_stride(m, element);
    if (m.builtin_type_of(element) != nullptr)
    {
        // A builtin element is one value at offset 0, which a one-member root describes as well as any.
        auto const members = cc::vector<member_info>{member_info{.name = "value", .type = element}};
        auto const placed = place(m, members, address_space::storage);
        auto leaves = cc::vector<placed_leaf>();
        for (auto const& leaf : placed.leaves)
            leaves.push_back({.path = {}, .type = leaf.type, .offset = leaf.offset, .size = leaf.size});
        return form_for(m, members, leaves, address_space::storage, stride, stride, "value", t);
    }
    auto const members = m.at(m.at(element).members);
    auto const placed = place(m, members, address_space::storage);
    return form_for(m, members, placed.leaves, address_space::storage, placed.size, stride, "value", t);
}
