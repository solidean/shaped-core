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
    return record.leaf_count > 4 || record.hlsl == "float2x2";
}

bool is_hlsl(emit::target t)
{
    return t == emit::target::hlsl_dx12 || t == emit::target::hlsl_vulkan;
}

/// A builtin's size and alignment under `t`'s own rule in `space`.
/// HLSL aligns every value to its scalar in a structured buffer, and in a constant buffer starts a matrix on a row.
builtins::block_layout native_layout(builtins::type_record const& record, emit::target t, address_space space)
{
    if (is_hlsl(t))
        return is_storage(space) ? builtins::block_layout{.size = record.leaf_count * scalar_size_of(record),
                                                          .alignment = scalar_size_of(record)}
                                 : record.hlsl_layout;
    return t == emit::target::msl ? record.msl_layout : record.wgsl_layout;
}

/// Whether a vector at `offset` reaches past the end of its 16-byte row.
bool crosses_row(builtins::type_record const& record, i32 offset)
{
    auto const size = record.leaf_count * scalar_size_of(record);
    return record.leaf_count > 1 && !is_matrix(record) && offset / 16 != (offset + size - 1) / 16;
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
    auto alignment = 1;
    for (auto const& member : members)
    {
        if (member.type == checked_module::void_type)
            continue;
        // an array strides by its element rounded up to the element's own alignment, and WGSL's uniform rule starts it
        // on a row
        if (m.builtin_type_of(member.type) == nullptr && m.at(member.type).kind == type_kind::array)
        {
            auto element = m.at(member.type).element;
            auto count = i32(m.at(member.type).count);
            while (m.builtin_type_of(element) == nullptr && m.at(element).kind == type_kind::array)
            {
                count *= i32(m.at(element).count);
                element = m.at(element).element;
            }
            auto stride = 0;
            auto element_alignment = 1;
            // EMIT-172: every target declares a `@padding(.every_element)` array as 4-vectors, a row apiece
            if (member.padding == check::padding_kind::every_element)
            {
                stride = 16;
                element_alignment = 16;
            }
            else if (auto const* const record = m.builtin_type_of(element))
            {
                auto const l = native_layout(*record, t, space);
                stride = round_up(l.size, l.alignment);
                element_alignment = l.alignment;
            }
            else if (t == emit::target::msl && !is_packed(space))
            {
                auto const natural = place_struct(m, element, space);
                element_alignment = natural.alignment;
                stride = round_up(natural.size, element_alignment);
            }
            else
            {
                auto dry = cc::vector<i32>();
                auto const inner = place_natively(m, m.at(m.at(element).members), space, t, 0, dry);
                element_alignment
                    = t == emit::target::wgsl && !is_storage(space) ? round_up(inner.alignment, 16) : inner.alignment;
                stride = round_up(inner.size, element_alignment);
            }
            auto const array_alignment
                = t == emit::target::wgsl && !is_storage(space) ? round_up(element_alignment, 16) : element_alignment;
            at = round_up(at, array_alignment);
            offsets.push_back(base + at);
            at += count * stride;
            alignment = max_of(alignment, array_alignment);
            continue;
        }
        if (auto const* const record = m.builtin_type_of(member.type))
        {
            auto const l = native_layout(*record, t, space);
            at = round_up(at, l.alignment);
            offsets.push_back(base + at);
            at += l.size;
            alignment = max_of(alignment, l.alignment);
            continue;
        }
        // MSL declares a struct in natural memory as SGL places it, a shared tail packed (EMIT-170)
        if (t == emit::target::msl && !is_packed(space))
        {
            auto const natural = place_struct(m, member.type, space);
            at = round_up(at, natural.alignment);
            for (auto const& leaf : natural.leaves)
                offsets.push_back(base + at + leaf.offset);
            at += natural.size;
            alignment = max_of(alignment, natural.alignment);
            continue;
        }
        // The struct's own alignment first, which only a dry run over its members gives.
        auto dry = cc::vector<i32>();
        auto const inner_members = m.at(m.at(member.type).members);
        auto inner = place_natively(m, inner_members, space, t, 0, dry);
        auto const is_uniform = t == emit::target::wgsl && !is_storage(space);
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
/// WGSL has no 16-bit integer, so a short never reaches a WGSL form (EMIT-109).
cc::string_view scalar_spelling(builtins::type_record const& record, emit::target t)
{
    auto const is_wgsl = t == emit::target::wgsl;
    switch (record.leaf_kind)
    {
    case value_kind::scalar_int:
        return is_wgsl ? "i32" : "int";
    case value_kind::scalar_uint:
        return is_wgsl ? "u32" : "uint";
    case value_kind::scalar_half:
        if (is_hlsl(t))
            return "float16_t";
        return is_wgsl ? "f16" : "half";
    case value_kind::scalar_short:
        return is_hlsl(t) ? "int16_t" : "short";
    case value_kind::scalar_ushort:
        return is_hlsl(t) ? "uint16_t" : "ushort";
    // WGSL has no 64-bit scalar, so a long never reaches a WGSL form either (EMIT-109)
    case value_kind::scalar_long:
        return is_hlsl(t) ? "int64_t" : "long";
    case value_kind::scalar_ulong:
        return is_hlsl(t) ? "uint64_t" : "ulong";
    case value_kind::scalar_double:
        return "double";
    default:
        return is_wgsl ? "f32" : "float";
    }
}

struct form_builder
{
    checked_module const& m;
    emit::target t;
    address_space space;
    memory_form form;
    i32 at = 0;
    i32 padding = 0;
    /// Every vector and matrix is split, which is what lowers the form's alignment to 4 for a buffer's stride.
    bool is_forced = false;
    /// A vector that crosses a 16-byte row is split as well, which only a `.cpp` block's layout lets happen (EMIT-154).
    bool splits_rows = false;

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

    i32 add_field(cc::string name, cc::string type, i32 size, i32 count = 0)
    {
        form.fields.push_back({.name = unique(cc::move(name)), .type = cc::move(type), .offset = at, .count = count});
        at += size;
        return i32(form.fields.size() - 1);
    }

    [[nodiscard]] cc::string_view short_padding() const
    {
        if (is_hlsl(t))
            return "uint16_t";
        return t == emit::target::wgsl ? "f16" : "ushort";
    }

    /// Pads in 4-byte words, and with one 2-byte scalar where a 16-bit value leaves room of 2: a WGSL text that has
    /// one holds a half, so `f16` is enabled there.
    void pad_to(i32 offset)
    {
        while (at + 4 <= offset)
            add_field(cc::format("_pad{}", padding++), cc::string(t == emit::target::wgsl ? "u32" : "uint"), 4);
        if (at < offset)
            add_field(cc::format("_pad{}", padding++), cc::string(short_padding()), 2);
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
            if (m.builtin_type_of(member.type) == nullptr && m.at(member.type).kind == type_kind::structure)
                level = m.at(m.at(member.type).members);
        }
        return result.empty() ? cc::string(fallback) : result;
    }

    void add_leaf(placed_leaf const& leaf, cc::string const& stem)
    {
        // an array of structs is one field read in place, which every target places as SGL does where the form is built
        if (leaf.count > 0 && m.builtin_type_of(leaf.type) == nullptr)
        {
            pad_to(leaf.offset);
            auto result = memory_leaf{.path = leaf.path, .type = leaf.type};
            result.fields.push_back(add_field(stem, cc::string(m.name_of(leaf.type)), leaf.size, leaf.count));
            form.fields.back().struct_type = leaf.type;
            form.leaves.push_back(cc::move(result));
            return;
        }
        auto const& record = *m.builtin_type_of(leaf.type);
        pad_to(leaf.offset);
        auto result = memory_leaf{.path = leaf.path, .type = leaf.type};
        auto const l = native_layout(record, t, space);
        auto const scalar = scalar_spelling(record, t);
        auto const scalar_size = scalar_size_of(record);

        auto const is_crossing = splits_rows && crosses_row(record, leaf.offset);
        // a packed mat3's columns stand 12 bytes apart, which no target's own matrix does
        auto const is_packed_mat3 = is_matrix(record) && record.leaf_count == 9 && is_packed(space);
        // an array's elements take whole rows, which every target places as SGL does, so it is never split
        if (is_hlsl(t) && is_matrix(record) && record.leaf_count == 9 && !is_packed(space) && leaf.count == 0)
        {
            result.is_widened = true;
            result.fields.push_back(add_field(stem, "float4x3", 48));
        }
        // EMIT-178: a packed buffer element's array strides by its element's scalars, which only some arrays of a
        // target's own do; one that does not is packed (MSL) or split into one array of every element's scalars
        else if (leaf.count > 0 && space == address_space::packed_storage && !is_hlsl(t))
        {
            result.count = leaf.count;
            auto const element_size = record.leaf_count * scalar_size;
            auto const is_native
                = record.leaf_count == 1
               || (!is_forced && !is_matrix(record) && round_up(l.size, l.alignment) == element_size
                   && leaf.offset % l.alignment == 0 && !(t == emit::target::msl && record.leaf_count == 3));
            if (is_native)
                result.fields.push_back(
                    add_field(stem, cc::string(record.spelled_in(language_of(t))), leaf.size, leaf.count));
            else if (t == emit::target::msl && !is_matrix(record))
            {
                result.is_packed = true;
                result.fields.push_back(
                    add_field(stem, cc::format("packed_{}", record.spelled_in(language_of(t))), leaf.size, leaf.count));
            }
            else
            {
                result.is_split = true;
                result.fields.push_back(add_field(stem, cc::string(scalar), leaf.size, leaf.count * record.leaf_count));
            }
        }
        else if (leaf.count > 0)
            result.fields.push_back(add_field(stem, cc::string(record.spelled_in(language_of(t))), leaf.size, leaf.count));
        else if (record.leaf_count == 1)
            result.fields.push_back(add_field(stem, cc::string(record.spelled_in(language_of(t))), scalar_size));
        else if (!is_forced && !is_crossing && !is_packed_mat3 && leaf.offset % l.alignment == 0
                 && !(t == emit::target::msl && record.leaf_count == 3))
            result.fields.push_back(add_field(stem, cc::string(record.spelled_in(language_of(t))), l.size));
        else if (t == emit::target::msl && !is_matrix(record) && !is_crossing)
        {
            // MSL's packed vector sits at any offset of its scalar's size and takes exactly its scalars' bytes.
            result.is_packed = true;
            result.fields.push_back(add_field(stem, cc::format("packed_{}", record.spelled_in(language_of(t))),
                                              record.leaf_count * scalar_size));
        }
        else
        {
            result.is_split = true;
            for (auto i = 0; i < record.leaf_count; ++i)
            {
                auto const name
                    = is_matrix(record) ? cc::format("{}_{}", stem, i) : cc::format("{}_{}", stem, component_name(i));
                result.fields.push_back(add_field(name, cc::string(scalar), scalar_size));
            }
        }
        form.leaves.push_back(cc::move(result));
    }
};

/// The largest alignment a field of `form` has under `t`'s rule, which is what an array of it strides by.
/// A split or packed value aligns to its scalar, and a padding field to its own size.
i32 alignment_of(checked_module const& m, memory_form const& form, emit::target t, address_space space)
{
    auto result = 1;
    for (auto const& leaf : form.leaves)
    {
        if (m.builtin_type_of(leaf.type) == nullptr)
            continue;
        auto const& record = *m.builtin_type_of(leaf.type);
        result = max_of(result, leaf.is_split || leaf.is_packed ? scalar_size_of(record)
                                                                : native_layout(record, t, space).alignment);
    }
    for (auto const& field : form.fields)
        if (field.name.starts_with("_pad"))
            result = max_of(result, field.type == "u32" || field.type == "uint" ? 4 : 2);
    return result;
}

memory_form build(checked_module const& m,
                  cc::span<member_info const> members,
                  cc::span<placed_leaf const> leaves,
                  address_space space,
                  i32 size,
                  i32 stride,
                  cc::string_view element_name,
                  emit::target t,
                  bool is_forced)
{
    auto b = form_builder{.m = m,
                          .t = t,
                          .space = space,
                          .form = {},
                          .is_forced = is_forced,
                          .splits_rows = space == address_space::packed_constants};
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
    // MSL's float3 takes 16 bytes and its half3 8, so the one value after it has to start past them as well.
    if (t == emit::target::msl)
        for (auto i = isize(0); i + 1 < leaves.size(); ++i)
            if (auto const* const record = m.builtin_type_of(leaves[i].type);
                record != nullptr && leaves[i].count == 0 && record->leaf_count == 3 && leaves[i].path.size() <= 1
                && leaves[i + 1].offset < leaves[i].offset + record->msl_layout.size)
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
    // EMIT-154: every target writes a packed block as its memory form
    if (space == address_space::packed_constants)
        return build(m, members, leaves, space, size, stride, element_name, t, false);
    // EMIT-112: HLSL writes a padding field into each gap instead, and packs a buffer's element as C++ does
    if (is_hlsl(t))
        return {};
    // EMIT-170: MSL packs a shared tail where the root declares it, so natural memory needs no form there either
    if (t == emit::target::msl && !is_packed(space))
        return {};
    if (is_natively_placed(m, members, leaves, space, stride, t))
        return {};
    auto form = build(m, members, leaves, space, size, stride, element_name, t, false);
    // An array's stride is its element's size rounded to the element's alignment, so a vector field whose alignment
    // does not divide the stride has to become scalars too.
    if (stride > 0 && round_up(stride, alignment_of(m, form, t, space)) != stride)
        form = build(m, members, leaves, space, size, stride, element_name, t, true);
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
    return form_for(m, members, placed.leaves, space, placed.end, stride, "value", t);
}

cc::optional<sgl::emit::impl::memory_form> sgl::emit::impl::element_form_of(check::checked_module const& m,
                                                                            check::type_id element,
                                                                            address_space space,
                                                                            emit::target t)
{
    // an atomic is a 4-byte integer at offset 0 in every target's own rule
    if (m.at(element).kind == type_kind::atomic)
        return {};
    auto const stride = element_stride(m, element, space);
    if (auto const* const record = m.builtin_type_of(element))
    {
        // A builtin element is one value at offset 0, which a one-member root describes as well as any.
        auto const members = cc::vector<member_info>{member_info{.name = "value", .type = element}};
        auto const placed = place(m, members, space);
        auto leaves = cc::vector<placed_leaf>();
        for (auto const& leaf : placed.leaves)
            leaves.push_back({.path = {}, .type = leaf.type, .offset = leaf.offset, .size = leaf.size});
        // EMIT-112: HLSL strides a buffer by its element's own size, so a 3-vector's padding has to be a field
        // CHK-518: and DXC's DXIL drops a `row_major` written in the buffer's template, which a field keeps
        if (is_hlsl(t) && (stride != record->leaf_count * scalar_size_of(*record) || record->name == "affine3"))
            return build(m, members, leaves, space, stride, stride, "value", t, false);
        return form_for(m, members, leaves, space, stride, stride, "value", t);
    }
    // EMIT-170: MSL declares a struct in natural memory as SGL places it, so its element needs no form
    if (t == emit::target::msl && !is_packed(space))
        return {};
    auto const members = m.at(m.at(element).members);
    auto const placed = place(m, members, space);
    return form_for(m, members, placed.leaves, space, placed.size, stride, "value", t);
}
