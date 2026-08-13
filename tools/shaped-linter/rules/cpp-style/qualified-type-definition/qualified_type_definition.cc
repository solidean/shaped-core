#include "qualified_type_definition.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/string.hh>

namespace scl
{
namespace
{
constexpr cc::string_view k_id = "qualified-type-definition";

constexpr cc::string_view k_rationale
    = "a header names its types once, in the library's fwd.hh, and defines them qualified — `struct "
      "cc::span { … };`, `enum class cc::seek_dir : u8 { … };`. An open `namespace cc { … }` around the "
      "definition adds a scope a reader has to carry, and hides whether the type was ever forward-declared. "
      "`impl` and `custom` are the exceptions: both are namespaces you are meant to see opened.";

/// The nested namespaces a reader is meant to see opened, exempt at any depth.
/// `impl` holds what is public only because inlining needs it, `custom` the specialization seam — neither is a library's vocabulary.
constexpr cc::string_view k_open_namespaces[] = {"impl", "custom"};

/// The integral aliases of `cc::primitive_defines`, which every library's root namespace re-exports through a using-directive in its own `fwd.hh`.
/// An enum-base spelling one of them stops resolving the moment the definition leaves the namespace, and starts again once qualified with that root — `enum class sv::fit_mode : sv::u8`.
/// Qualified lookup follows the using-directive, which is what makes `sv::u8` name `cc::u8`.
///
/// Any OTHER unqualified enum-base is left alone entirely: the linter cannot tell which enclosing scope it came from, and guessing wrong is a rewrite that does not compile.
constexpr cc::string_view k_reexported_integer_aliases[]
    = {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "isize", "usize", "byte"};

bool is_reexported_alias(cc::string_view name)
{
    for (auto const alias : k_reexported_integer_aliases)
        if (name == alias)
            return true;
    return false;
}

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

/// Past any blank lines starting at `offset`.
/// Dropping a namespace head leaves whatever blank line opened its body behind, directly under the one that already separated the namespace from what came before.
u32 past_blank_lines(cc::string_view text, u32 offset)
{
    auto at = offset;
    while (at < text.size())
    {
        auto const next = line_tail(text, at, false);
        if (next == at) // real code on this line
            return at;
        at = next;
    }
    return at;
}

/// Back over any blank lines ending at `offset`, which is the start of a line.
/// The mirror of the above for a namespace closer: the blank line above it and the one below would otherwise end up adjacent.
u32 before_blank_lines(cc::string_view text, u32 offset)
{
    auto at = offset;
    while (at > 0)
    {
        auto line_begin = at - 1;
        while (line_begin > 0 && text[line_begin - 1] != '\n')
            --line_begin;
        if (line_tail(text, line_begin, false) != at)
            return at; // real code on that line
        at = line_begin;
    }
    return at;
}

/// Are the bytes in `[from, to)` nothing but whitespace?
bool only_blanks(cc::string_view text, u32 from, u32 to)
{
    for (auto i = isize(from); i < isize(to) && i < text.size(); ++i)
        if (!cc::is_space(text[i]))
            return false;
    return true;
}

/// The offset a record's statement starts at, its leading `//` comment block included.
/// The record node's span opens at `template` or at the keyword, so the doc comment above it would otherwise be left on the wrong side of a moved brace.
u32 statement_start(source_buffer const& src, u32 offset)
{
    auto line = src.line_col_at(offset).line;
    while (line > 1 && trimmed(src.line_text(line - 1)).starts_with("//"))
        --line;
    return src.line_span(line).byte_begin;
}

/// The offset just past a record definition's terminating `;`, and past the rest of that line when nothing but a comment follows.
u32 past_declaration(cc::string_view text, u32 record_end)
{
    auto i = isize(record_end);
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
        ++i;
    if (i < text.size() && text[i] == ';')
        ++i;
    return line_tail(text, u32(i), true);
}

/// Is this definition one the rule can ask to be qualified?
///
/// Four shapes are not, and each of them also ends the run it sits in, since a block that moves out cannot take them along:
///  - a type declared inside a function body, which the parser parents to the enclosing namespace rather than to the function;
///  - an anonymous one, which has no name to put a qualifier in front of;
///  - `struct S { } s;`, whose variable would land at file scope if the definition moved out;
///  - an unscoped enum with no enum-base, whose underlying type is only known once the enumerators are.
///    The grammar has no opaque declaration for it, and a qualified definition must refer back to one.
///
/// An enum-base that is an unqualified name is the fifth: it resolved through the namespace, so moving the definition out breaks it.
/// The re-exported integer aliases are the one such base the fix can carry along, by qualifying it too.
bool is_candidate(lint_context const& ctx, isize def)
{
    auto const& n = ctx.tree[def];
    if (n.scope != decl_scope::namespace_scope || n.has_declarator || ctx.source.span_text(n.name).empty())
        return false;
    if (n.kind != node_kind::enum_definition)
        return true;
    if (!n.enum_scoped && !n.enum_has_base)
        return false;

    auto const base = ctx.source.span_text(n.enum_base_name);
    return base.empty() || is_reexported_alias(base);
}

/// The record and enum definitions declared directly in `ns`, in source order.
/// A type nested in a record is that record's child, so it never appears here — only the outermost type is the one to qualify.
///
/// A definition that is not a candidate is still collected, because it is what ends the run around it.
/// Leaving it out would let a block move out over the top of it.
cc::vector<isize> direct_definitions(syntax_tree const& tree, isize ns)
{
    cc::vector<isize> out;
    for (auto const c : tree[ns].children)
        if (tree[c].kind == node_kind::record_definition || tree[c].kind == node_kind::enum_definition)
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

        auto const defs = direct_definitions(ctx.tree, ns);
        if (defs.empty())
            continue;

        auto const fid = n.span.file_id;
        auto const insertion = [fid](u32 at, cc::string replacement)
        {
            return text_edit{.span = {.file_id = fid, .byte_begin = at, .byte_end = at},
                             .replacement = cc::move(replacement)};
        };

        // Dropping the namespace head, and its closing brace, whole.
        // Only correct where nothing that must stay scoped is left behind — a run of records at the start of the body, or at its end.
        //
        // Each takes the blank lines on its inner side along.
        // They separated the definitions from the braces, and once the braces are gone they would double up with the blank lines that separated the namespace from its surroundings.
        auto const drop_head
            = text_edit{.span = {.file_id = fid,
                                 .byte_begin = n.span.byte_begin,
                                 .byte_end = past_blank_lines(text, line_tail(text, n.body.byte_begin + 1, false))}};
        auto const drop_closer = text_edit{.span = {.file_id = fid,
                                                    .byte_begin = before_blank_lines(text, n.body.byte_end - 1),
                                                    .byte_end = line_tail(text, n.body.byte_end, true)}};

        // A run is a maximal series of adjacent candidate definitions, which moves out of the namespace as one block.
        // Its edits are shared by every finding in it — including the qualifier for each of its types — so applying any one finding's fix rewrites the whole run consistently.
        // `collect_fix_edits` merges the byte-identical copies, so a whole-file run still lands exactly once.
        for (auto i = isize(0); i < defs.size(); ++i)
        {
            if (!is_candidate(ctx, defs[i]))
                continue;

            auto last = i;
            while (last + 1 < defs.size() && ctx.tree[defs[last + 1]].follows_definition
                   && is_candidate(ctx, defs[last + 1]))
                ++last;

            auto const open = statement_start(ctx.source, ctx.tree[defs[i]].span.byte_begin);
            auto const close = past_declaration(text, ctx.tree[defs[last]].span.byte_end);

            cc::vector<text_edit> edits;
            if (only_blanks(text, n.body.byte_begin + 1, open))
                edits.push_back(drop_head);
            else
                edits.push_back(insertion(open, cc::string("} // namespace ") + ns_name + "\n\n"));

            if (only_blanks(text, close, n.body.byte_end - 1))
                edits.push_back(drop_closer);
            else
                edits.push_back(insertion(close, cc::string("\nnamespace ") + ns_name + "\n{\n"));

            for (auto k = i; k <= last; ++k)
            {
                edits.push_back(insertion(ctx.tree[defs[k]].name.byte_begin, cc::string(ns_name) + "::"));

                // A re-exported alias is qualified with the namespace's ROOT, never with `ns_name` itself.
                // The using-directive that re-exports it sits in that root — `cc::console::u8` would not find what `cc::u8` does.
                auto const base = ctx.tree[defs[k]].enum_base_name;
                if (!ctx.source.span_text(base).empty())
                    edits.push_back(insertion(base.byte_begin, cc::string(name_components(ns_name)[0]) + "::"));
            }

            for (auto k = i; k <= last; ++k)
            {
                auto const& def = ctx.tree[defs[k]];
                auto const def_name = ctx.source.span_text(def.name);

                ctx.report({
                    .rule_id = k_id,
                    .span = def.name,
                    .message = cc::string("`") + def_name + "` should be defined as `" + ns_name + "::" + def_name
                             + "`, not inside an open `namespace " + ns_name + "`",
                    .sev = severity::warning,
                    .suggested_fix = fix{.edits = edits},
                });
            }

            i = last;
        }
    }
}
} // namespace

rule const& qualified_type_definition_rule()
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
