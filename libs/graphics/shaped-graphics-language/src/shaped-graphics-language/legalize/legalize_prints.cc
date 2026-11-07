#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/reserved_bindings.hh>
#include <shaped-graphics-language/check/source_lines.hh>
#include <shaped-graphics-language/legalize/impl/legalizer.hh>

// LEGAL-70: a print as flat statements over `sg_debug`, so no target and no route knows what a print is.
//
// The window is a header of three words, then records (libs/graphics/shaped-graphics-language/docs/reserved-bindings.md):
//     let at = words[0].add(count)
//     if at + count <= words[1].load():
//         words[3 + at + i].store(word i)
//     else:
//         words[2].add(1)

using namespace sgl;
using namespace sgl::check;
using sgl::check::impl::stmt_list;

namespace
{
/// FNV-1a over `text`, continuing from `hash`.
[[nodiscard]] u32 hashed(u32 hash, cc::string_view text)
{
    for (auto const c : text)
        hash = (hash ^ u32(u8(c))) * 16777619u;
    return (hash ^ 0xFFu) * 16777619u;
}

/// The family and lanes of a printable type (CHK-424): `float3` is `float` in 3, and an enum one `int`.
[[nodiscard]] debug_value debug_value_of(checked_module const& m, type_id type)
{
    auto result = debug_value{.family = "int", .type = cc::string(m.name_of(type))};
    // the prelude declares `bool` an enum, which is no enum a message writes as its int
    if (m.at(type).kind == type_kind::enumeration && result.type != "bool")
        return result;
    auto name = cc::string_view(result.type);
    if (!name.empty() && name[name.size() - 1] >= '2' && name[name.size() - 1] <= '4')
    {
        result.lanes = name[name.size() - 1] - '0';
        name = name.subview({.offset = 0, .size = name.size() - 1});
    }
    result.family = cc::string(name);
    return result;
}

struct expander
{
    flat_builder& out;
    source_lines lines;
    symbol_id binding = symbol_id::none;

    [[nodiscard]] flat_expr_id word_literal(u32 value)
    {
        return out.add_expr(out.type_named(builtins::k_uint), flat_int_literal{.value = i32(value), .is_unsigned = true});
    }
    [[nodiscard]] flat_expr_id uint_call(cc::string_view name, flat_expr_id a, flat_expr_id b)
    {
        flat_expr_id const args[] = {a, b};
        return out.call(name, args);
    }
    [[nodiscard]] flat_expr_id word_at(flat_expr_id index)
    {
        return out.buffer_element(out.binding_member(binding, 0), index);
    }
    /// `words[index].name(args...)`, which only an atomic's call reaches.
    [[nodiscard]] flat_expr_id atomic(cc::string_view name, flat_expr_id index, cc::span<flat_expr_id const> args = {})
    {
        auto all = cc::vector<flat_expr_id>();
        all.push_back(word_at(index));
        all.push_back_range(args);
        return out.call_with_effect(name, all);
    }

    /// One lane as the word its record holds: its bits, a bool 0 or 1, and a `half` widened to a float first.
    [[nodiscard]] flat_expr_id word_of(flat_expr_id lane, cc::string_view family)
    {
        flat_expr_id const argument[] = {lane};
        if (family == "uint")
            return lane;
        if (family == "bool")
            return out.call("convert_bool_to_uint", argument);
        if (family == "int")
            return out.call("convert_int_to_uint", argument);
        if (family == "half")
        {
            flat_expr_id const wide[] = {out.call("convert_half_to_float", argument)};
            return out.call("reinterpret_as_uint", wide);
        }
        return out.call("reinterpret_as_uint", argument);
    }

    [[nodiscard]] debug_site site_of(flat_stmt const& s, flat_print const& n)
    {
        auto site = debug_site();
        if (n.message >= 0 && n.message < out.e.messages.size())
        {
            site.pieces = out.e.messages[n.message].pieces;
            site.is_assert = out.e.messages[n.message].is_assert;
        }
        if (!lines.files.empty())
            site.origin = lines.origin_of(out.e, s);
        for (auto const id : out.e.at(n.values))
            site.values.push_back(debug_value_of(out.m, out.e.at(id).type));

        auto id = hashed(2166136261u, out.e.name);
        id = hashed(id, cc::format("{}", n.message));
        for (auto const& piece : site.pieces)
            id = hashed(id, piece);
        if (site.origin.at.file >= 0)
            id = hashed(id, cc::format("{}:{}", lines.name_of(site.origin.at.file), site.origin.at.line));
        // one line inlined along two call chains is two sites, which another permutation of this entry point may hold
        for (auto const& from : site.origin.inlined_from)
            if (from.file >= 0)
                id = hashed(id, cc::format("<{}:{}", lines.name_of(from.file), from.line));
        site.id = id;
        return site;
    }

    void expand(flat_stmt const& s, flat_print const& n, stmt_list& into)
    {
        out.from = s.from;
        out.inlined_through = s.inlined_through;
        auto site = site_of(s, n);

        // each value bound once, so it is computed once whatever its lanes, and ahead of the reservation
        auto words = cc::vector<flat_expr_id>();
        auto const values = cc::vector<flat_expr_id>::create_copy_of(cc::span<flat_expr_id const>(out.e.at(n.values)));
        for (auto i = isize(0); i < values.size(); ++i)
        {
            auto const& value = site.values[i];
            // an enum is held as the int every target spells it, which is what its word converts from
            auto const is_enum
                = out.m.at(out.e.at(values[i]).type).kind == type_kind::enumeration && value.family == "int";
            auto const bound = out.add_local(local_kind::temporary, "printed",
                                             is_enum ? out.type_named(builtins::k_int) : out.e.at(values[i]).type);
            into.push_back(out.let(bound, values[i]));
            for (auto lane = 0; lane < value.lanes; ++lane)
            {
                constexpr cc::string_view k_lanes[] = {"x", "y", "z", "w"};
                auto const read = value.lanes == 1 ? out.local(bound) : out.member(out.local(bound), k_lanes[lane]);
                auto const word = out.add_local(local_kind::temporary, "word", out.type_named(builtins::k_uint));
                into.push_back(out.let(word, word_of(read, value.family)));
                words.push_back(out.local(word));
            }
        }

        auto const count = u32(words.size()) + 2;
        flat_expr_id const reserved[] = {word_literal(count)};
        auto const at = out.add_local(local_kind::temporary, "debug_at", out.type_named(builtins::k_uint));
        into.push_back(out.let(at, atomic("add", word_literal(k_debug_cursor_word), reserved)));
        auto const end = uint_call("add_uint", out.local(at), word_literal(count));
        auto const capacity = atomic("load", word_literal(k_debug_capacity_word));
        auto const fits = uint_call("less_equal_uint", end, capacity);

        auto const store = [&](stmt_list& body, u32 at_word, flat_expr_id word)
        {
            auto const first = uint_call("add_uint", out.local(at), word_literal(k_debug_header_words + at_word));
            flat_expr_id const argument[] = {word};
            body.push_back(out.eval(atomic("store", first, argument)));
        };
        auto written = stmt_list();
        store(written, 0, word_literal(site.id));
        store(written, 1, word_literal(count));
        for (auto i = isize(0); i < words.size(); ++i)
            store(written, u32(i) + 2, words[i]);
        flat_expr_id const one[] = {word_literal(1)};
        flat_stmt_id const dropped[] = {out.eval(atomic("add", word_literal(k_debug_dropped_word), one))};
        into.push_back(out.if_(fits, written, dropped));

        out.e.debug_sites.push_back(cc::move(site));
    }
};
} // namespace

symbol_id check::impl::reserved_symbol_of(checked_module const& m, cc::string_view name)
{
    for (auto i = isize(0); i < m.symbols.size(); ++i)
        if (m.symbols[i].kind == symbol_kind::binding && m.symbols[i].name == name && m.symbols[i].file < m.prelude_files)
            return symbol_id(i);
    return symbol_id::none;
}

void check::impl::expand_print(flat_builder& out,
                               flat_stmt const& s,
                               flat_print const& n,
                               legalize_options const& options,
                               stmt_list& into)
{
    auto const binding = reserved_symbol_of(out.m, "sg_debug");
    // a test's own prelude declares no window, and no record has anywhere to go
    if (!is_valid(binding))
        return;
    auto found = false;
    for (auto const id : out.e.reserved)
        found = found || id == binding;
    if (!found)
        out.e.reserved.push_back(binding);
    auto e
        = expander{.out = out, .lines = {.m = out.m, .files = options.files, .names = options.names}, .binding = binding};
    e.expand(s, n, into);
}
