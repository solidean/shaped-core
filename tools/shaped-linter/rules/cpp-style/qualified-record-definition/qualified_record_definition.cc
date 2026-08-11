#include "qualified_record_definition.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/string.hh>

namespace scl
{
namespace
{
constexpr cc::string_view k_id = "qualified-record-definition";

constexpr cc::string_view k_rationale
    = "a header names its types once, in the library's fwd.hh, and defines them qualified — `struct "
      "cc::span { … };`. An open `namespace cc { … }` around the definition adds a scope a reader has to "
      "carry, and hides whether the type was ever forward-declared. `impl` and `custom` are the exceptions: "
      "both are namespaces you are meant to see opened.";

/// The nested namespaces a reader is meant to see opened, exempt at any depth.
/// `impl` holds what is public only because inlining needs it, `custom` the specialization seam — neither is a library's vocabulary.
constexpr cc::string_view k_open_namespaces[] = {"impl", "custom"};

/// The extensions that mark a translation unit rather than a header.
/// Everything else — `.hh`, and a path with no extension at all — counts as a header, since a definition in a TU is that file's own business.
constexpr cc::string_view k_implementation_extensions[] = {".cc", ".cpp", ".cxx", ".c"};

bool is_implementation_file(cc::string_view path)
{
    for (auto const ext : k_implementation_extensions)
        if (path.ends_with(ext))
            return true;
    return false;
}

cc::string_view trimmed(cc::string_view s)
{
    isize b = 0;
    isize e = s.size();
    while (b < e && cc::is_space(s[b]))
        ++b;
    while (e > b && cc::is_space(s[e - 1]))
        --e;
    return s.subview({.start = b, .end = e});
}

/// The `::`-separated components of a name as written — `cc::impl` becomes `cc`, `impl`.
cc::vector<cc::string_view> name_components(cc::string_view name)
{
    cc::vector<cc::string_view> out;
    isize start = 0;
    isize i = 0;
    while (i < name.size())
    {
        if (name[i] == ':' && i + 1 < name.size() && name[i + 1] == ':')
        {
            out.push_back(trimmed(name.subview({.start = start, .end = i})));
            i += 2;
            start = i;
            continue;
        }
        ++i;
    }
    out.push_back(trimmed(name.subview({.start = start, .end = name.size()})));
    return out;
}

/// The namespace `id` sits directly in, or -1 at file scope.
isize enclosing_namespace(syntax_tree const& tree, isize id)
{
    for (auto n = isize(0); n < tree.nodes.size(); ++n)
        if (tree.nodes[n].kind == node_kind::namespace_definition)
            for (auto const c : tree.nodes[n].children)
                if (c == id)
                    return n;
    return -1;
}

/// Is `ns`, or anything it nests in, one of the namespaces meant to be opened?
/// The walk goes outward because `namespace impl { namespace parsing { … } }` is as much inside `impl` as `cc::impl` is.
bool is_open_namespace(lint_context const& ctx, isize ns)
{
    for (auto id = ns; id >= 0; id = enclosing_namespace(ctx.tree, id))
        for (auto const component : name_components(ctx.source.span_text(ctx.tree[id].name)))
            for (auto const open : k_open_namespaces)
                if (component == open)
                    return true;
    return false;
}

/// Past the line terminator following `offset`, when only blanks — or, with `allow_comment`, a trailing `//` comment — lie between.
/// Returns `offset` unchanged when real code follows, so the one-line `namespace a { struct x { }; }` loses its own tokens and nothing else.
u32 line_tail(cc::string_view text, u32 offset, bool allow_comment)
{
    auto i = isize(offset);
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
        ++i;
    if (allow_comment && i + 1 < text.size() && text[i] == '/' && text[i + 1] == '/')
        while (i < text.size() && text[i] != '\n')
            ++i;

    if (i < text.size() && text[i] == '\r')
        ++i;
    if (i < text.size() && text[i] == '\n')
        return u32(i + 1);
    return i >= text.size() ? u32(text.size()) : offset;
}

/// The record definitions declared directly in `ns`.
/// A record nested in another record is that record's child, so it never appears here — only the outermost type is the one to qualify.
cc::vector<isize> direct_records(syntax_tree const& tree, isize ns)
{
    cc::vector<isize> out;
    for (auto const c : tree[ns].children)
        if (tree[c].kind == node_kind::record_definition)
            out.push_back(c);
    return out;
}

void check(lint_context& ctx)
{
    if (is_implementation_file(ctx.source.path()))
        return;

    auto const text = ctx.source.text();

    for (auto ns = isize(0); ns < ctx.tree.nodes.size(); ++ns)
    {
        auto const& n = ctx.tree.nodes[ns];
        if (n.kind != node_kind::namespace_definition)
            continue;

        auto const ns_name = ctx.source.span_text(n.name);
        if (ns_name.empty() || is_open_namespace(ctx, ns))
            continue;

        auto const records = direct_records(ctx.tree, ns);
        if (records.empty())
            continue;

        // An anonymous record has no name to put the qualifier in front of, and the fix is all-or-nothing: whatever stays behind would lose its namespace.
        auto all_named = true;
        for (auto const r : records)
            if (ctx.source.span_text(ctx.tree[r].name).empty())
                all_named = false;

        // The two edits that unwrap the namespace are shared by every finding in it, and `collect_fix_edits` merges the byte-identical copies.
        // Each finding therefore carries a fix that is complete on its own, which is the contract `--fix` rests on.
        auto const head = text_edit{.span = {.file_id = n.span.file_id,
                                             .byte_begin = n.span.byte_begin,
                                             .byte_end = line_tail(text, n.body.byte_begin + 1, false)}};
        auto const closer = text_edit{.span = {.file_id = n.span.file_id,
                                               .byte_begin = n.body.byte_end - 1,
                                               .byte_end = line_tail(text, n.body.byte_end, true)}};

        for (auto const r : records)
        {
            auto const& rec = ctx.tree[r];
            auto const rec_name = ctx.source.span_text(rec.name);

            auto suggested_fix = cc::optional<fix>();
            auto suggested_hint = cc::optional<hint>();
            if (n.body_holds_records_only && all_named)
            {
                auto const qualify = text_edit{
                    .span
                    = {.file_id = rec.name.file_id, .byte_begin = rec.name.byte_begin, .byte_end = rec.name.byte_begin},
                    .replacement = cc::string(ns_name) + "::"};
                suggested_fix = fix{.edits = {head, closer, qualify}};
            }
            else
                suggested_hint = hint{.message = cc::string("the namespace holds more than record definitions, so "
                                                            "only the record moves out: define it as `")
                                               + ns_name + "::" + rec_name
                                               + "` above or below the namespace, and leave the rest inside"};

            ctx.report({
                .rule_id = k_id,
                .span = rec.name,
                .message = cc::string("`") + rec_name + "` should be defined as `" + ns_name + "::" + rec_name
                         + "`, not inside an open `namespace " + ns_name + "`",
                .sev = severity::warning,
                .suggested_fix = cc::move(suggested_fix),
                .suggested_hint = cc::move(suggested_hint),
            });
        }
    }
}
} // namespace

rule const& qualified_record_definition_rule()
{
    static rule const r = {
        .id = k_id,
        .rationale = k_rationale,
        .layer = rule_layer::syntax_tree,
        .default_severity = severity::warning,
        .check = &check,
    };
    return r;
}
} // namespace scl
