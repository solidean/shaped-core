#include "references.hh"

#include <shaped-graphics-language/driver/impl/reference_reader.hh>

using namespace sgl;
using sgl::impl::reader;

namespace
{
/// How far back a call's opening paren is looked for, so a file of unbalanced parens costs a bounded walk.
constexpr auto max_tokens_walked = isize(4096);

[[nodiscard]] bool is_open(token_kind k)
{
    return k == token_kind::round_open || k == token_kind::square_open || k == token_kind::curly_open;
}

[[nodiscard]] bool is_close(token_kind k)
{
    return k == token_kind::round_close || k == token_kind::square_close || k == token_kind::curly_close;
}

/// The call around a position, as its tokens show it.
struct call_site
{
    /// The name called, the `f` of `f(` or of `x.f(`.
    source_span callee;
    bool is_method = false;
    /// The commas between the paren and the position, at the paren's depth.
    i32 argument = 0;
    /// The parameter the argument names, `flags` of `flags = …`; empty for a positional one.
    cc::string_view named;
};

/// The `(` of a call, whose callee is a name fused to it, possibly through a `[…]` of type arguments.
[[nodiscard]] cc::optional<call_site> call_of_paren(parsed_file const& file, isize paren)
{
    if (paren == 0 || !file.is_fused_left(token_id(i32(paren))))
        return {};
    auto i = paren - 1;
    if (file.tokens[i].kind == token_kind::square_close)
    {
        auto depth = 0;
        for (; i >= 0; --i)
        {
            auto const k = file.tokens[i].kind;
            depth += is_close(k) ? 1 : is_open(k) ? -1 : 0;
            if (depth == 0)
                break;
        }
        if (i <= 0 || !file.is_fused_left(token_id(i32(i))))
            return {};
        --i;
    }
    auto const& callee = file.tokens[i];
    if (callee.kind != token_kind::symbol || is_keyword(file.text_of(callee.where)))
        return {};
    return call_site{.callee = callee.where, .is_method = i > 0 && file.tokens[i - 1].kind == token_kind::dot};
}

/// The innermost call whose parens hold `offset`.
[[nodiscard]] cc::optional<call_site> call_around(parsed_file const& file, u32 offset)
{
    auto last = isize(-1);
    for (auto i = isize(0); i < file.tokens.size() && file.tokens[i].where.offset < offset; ++i)
        last = i;
    auto depth = 0;
    auto commas = 0;
    // the first token of the argument the position is in
    auto argument_start = last + 1;
    for (auto i = last; i >= 0 && last - i < max_tokens_walked; --i)
    {
        auto const k = file.tokens[i].kind;
        if (is_close(k))
            ++depth;
        else if (is_open(k) && depth > 0)
            --depth;
        else if (depth == 0 && k == token_kind::comma)
        {
            if (commas == 0)
                argument_start = i + 1;
            ++commas;
        }
        else if (depth == 0 && is_open(k))
        {
            if (commas == 0)
                argument_start = i + 1;
            if (k == token_kind::round_open)
                if (auto site = call_of_paren(file, i); site.has_value())
                {
                    site.value().argument = commas;
                    auto const& t = file.tokens;
                    auto const a = argument_start;
                    if (a + 1 <= last && t[a].kind == token_kind::symbol && t[a + 1].kind == token_kind::op
                        && file.text_of(t[a + 1].where) == "=")
                        site.value().named = file.text_of(t[a].where);
                    return site;
                }
            // a `[`, a `{` or a paren that is no call: the position is an argument of whatever holds it
            commas = 0;
            argument_start = i;
        }
    }
    return {};
}

/// The parameter's name as `label` writes it, `.flags: uint` being `flags`.
[[nodiscard]] cc::string_view parameter_name(cc::string_view label, parameter_label p)
{
    auto text = label.subview({.offset = isize(p.offset), .size = isize(p.length)});
    if (text.starts_with("."))
        text = text.subview(1);
    auto end = isize(0);
    while (end < text.size() && sgl::impl::is_word_char(text[end]))
        ++end;
    return text.subview({.start = 0, .end = end});
}
} // namespace

cc::optional<signature_help> sgl::signature_help_at(cc::span<check::module_file const> files,
                                                    check::checked_module const& m,
                                                    i32 file_index,
                                                    u32 offset)
{
    auto const r = reader{.files = files, .m = m};
    if (!r.has_file(file_index))
        return {};
    auto const& file = files[file_index].file;
    auto const& a = files[file_index].ast;
    auto const site = call_around(file, offset);
    if (!site.has_value())
        return {};
    auto const name = file.text_of(site.value().callee);

    // what the check resolved the callee to, where it got that far
    auto resolved = check::symbol_id::none;
    for (auto i = isize(0); i < a.exprs.size(); ++i)
    {
        auto const& e = a.exprs[i];
        auto const* n = e.node.try_as<ast::name>();
        auto const* mb = e.node.try_as<ast::member>();
        if ((n == nullptr || n->where != site.value().callee) && (mb == nullptr || mb->name != site.value().callee))
            continue;
        auto const t = r.target_at(file_index, ast::expr_id(i));
        if ((t.kind == check::target_kind::symbol || t.kind == check::target_kind::overload
             || t.kind == check::target_kind::constructor)
            && check::is_valid(t.symbol))
            resolved = t.symbol;
        break;
    }

    auto candidates = cc::vector<check::symbol_id>();
    if (check::is_valid(resolved) && m.at(resolved).kind == check::symbol_kind::function)
    {
        candidates.push_back(resolved);
        for (auto const o : r.overloads_of(resolved))
            candidates.push_back(o);
    }
    else
        // unresolved, or a struct whose constructors the call picks from: every function a call of this name may mean
        for (auto i = isize(0); i < m.symbols.size(); ++i)
        {
            auto const& s = m.symbols[i];
            if (s.kind != check::symbol_kind::function || s.name != name || !s.operator_spelling.empty()
                || !m.is_reached_file(s.file))
                continue;
            auto const is_constructor = s.role == check::function_role::constructor;
            if (check::is_valid(resolved) && !(is_constructor && s.owner == resolved))
                continue;
            if (site.value().is_method ? s.role != check::function_role::method
                                       : check::is_valid(s.owner) && !is_constructor)
                continue;
            candidates.push_back(check::symbol_id(i));
        }
    // the program's own functions of the name shadow the prelude's
    auto const is_prelude = [&](check::symbol_id id) { return m.at(id).file < m.prelude_file_count(); };
    auto has_own = false;
    for (auto const id : candidates)
        has_own = has_own || !is_prelude(id);
    if (has_own)
        candidates.remove_all_where(is_prelude);
    if (candidates.empty())
        return {};

    auto help = signature_help{.active_parameter = site.value().argument};
    for (auto const id : candidates)
    {
        auto const& s = m.at(id);
        auto sig = call_signature();
        sig.label = r.function_signature(s, true, &sig.parameters);
        sig.doc = r.doc_above(s.file, r.declared_name_of(s));
        help.signatures.push_back(cc::move(sig));
    }
    auto const& sigs = help.signatures;
    auto const named = site.value().named;
    auto const has_parameter = [&](isize i) -> bool
    {
        if (named.empty())
            return help.active_parameter < sigs[i].parameters.size();
        for (auto const p : sigs[i].parameters)
            if (parameter_name(sigs[i].label, p) == named)
                return true;
        return false;
    };
    // the resolved function is the first candidate, and stays active while it fits; a call still being written may have
    // resolved to an overload its next argument rules out
    if (!has_parameter(0))
        for (auto i = isize(1); i < sigs.size(); ++i)
            if (has_parameter(i))
            {
                help.active_signature = i32(i);
                break;
            }
    if (!named.empty())
    {
        auto const& active = sigs[help.active_signature];
        for (auto i = isize(0); i < active.parameters.size(); ++i)
            if (parameter_name(active.label, active.parameters[i]) == named)
                help.active_parameter = i32(i);
    }
    return help;
}
