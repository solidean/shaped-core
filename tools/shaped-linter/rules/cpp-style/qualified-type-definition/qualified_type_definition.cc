#include "qualified_type_definition.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/string.hh>
#include <shaped-linter/lex/source_manager.hh>

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

/// The library's fwd.hh a moved type is declared in, and how a header includes it.
struct fwd_header
{
    source_buffer const* buffer = nullptr;
    cc::string include; // `<lib/fwd.hh>`'s inner text, the path below `src/`
};

/// Where a header's types are declared: the nearest `fwd.hh` in its folder or above, below its `src/` folder, as each library keeps one.
/// Absent without a run's source manager, without a `src/` folder above the header, without such a file, or when the header is that fwd.hh.
cc::optional<fwd_header> find_fwd_header(lint_context const& ctx)
{
    if (ctx.files == nullptr)
        return {};
    cc::vector<cc::string_view> parts;
    auto const path = ctx.source.path();
    isize start = 0;
    for (auto i = isize(0); i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/' || path[i] == '\\')
        {
            parts.push_back(path.subview({.start = start, .end = i}));
            start = i + 1;
        }
    auto src = isize(-1);
    for (auto i = isize(0); i + 1 < parts.size(); ++i)
        if (parts[i] == "src")
            src = i;
    if (src < 0)
        return {};
    auto const joined = [&](isize from, isize to)
    {
        auto s = cc::string();
        for (auto i = from; i < to; ++i)
        {
            if (i > from)
                s += '/';
            s += parts[i];
        }
        return s;
    };
    // the folder the header is in, then each folder above it down to just below src/
    if (parts.back() == "fwd.hh")
        return {}; // the header is its library's fwd.hh: nowhere to declare its types but here
    for (auto last = parts.size() - 1; last > src; --last)
    {
        auto const candidate = joined(0, last) + "/fwd.hh";
        auto const found = ctx.files->find_or_add_file(candidate);
        if (found.has_value())
            return fwd_header{.buffer = found.value(), .include = joined(src + 1, last) + "/fwd.hh"};
    }
    return {};
}

bool is_identifier_char(char c)
{
    return cc::is_alphanumeric(c) || c == '_';
}

/// Does `line` hold `word` as a whole identifier?
bool has_word(cc::string_view line, cc::string_view word)
{
    for (auto i = isize(0); i + word.size() <= line.size(); ++i)
        if (line.subview({.start = i, .end = i + word.size()}) == word && (i == 0 || !is_identifier_char(line[i - 1]))
            && (i + word.size() == line.size() || !is_identifier_char(line[i + word.size()])))
            return true;
    return false;
}

/// Does `text` already declare a type `name`: a line opening with its class-key, or a template head, naming it and ending in `;`?
/// A textual test: the namespace it sits in is not checked, which a fwd.hh holding one name twice would make wrong.
bool declares_type(cc::string_view text, cc::string_view name)
{
    isize begin = 0;
    for (auto i = isize(0); i <= text.size(); ++i)
    {
        if (i < text.size() && text[i] != '\n')
            continue;
        auto const line = trimmed(text.subview({.start = begin, .end = i}));
        begin = i + 1;
        auto const keyed = line.starts_with("struct ") || line.starts_with("class ") || line.starts_with("union ")
                        || line.starts_with("enum ") || line.starts_with("template");
        if (keyed && line.ends_with(";") && !line.contains('{') && has_word(line, name))
            return true;
    }
    return false;
}

/// Where a declaration goes in `text`: the start of the line closing its `namespace ns` block, or -1 when it has none.
isize namespace_insertion(cc::string_view text, cc::string_view ns)
{
    auto const head = cc::string("namespace ") + ns;
    for (auto at = isize(0); at + head.size() <= text.size(); ++at)
    {
        if (text.subview({.start = at, .end = at + head.size()}) != head || (at > 0 && text[at - 1] != '\n'))
            continue;
        auto i = at + head.size();
        if (i < text.size() && (is_identifier_char(text[i]) || text[i] == ':'))
            continue; // `namespace cc::impl` when looking for `namespace cc`
        while (i < text.size() && cc::is_space(text[i]))
            ++i;
        if (i >= text.size() || text[i] != '{')
            continue;
        auto depth = 0;
        for (; i < text.size(); ++i)
        {
            if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/')
                while (i < text.size() && text[i] != '\n')
                    ++i;
            if (i >= text.size())
                break;
            if (text[i] == '{')
                ++depth;
            else if (text[i] == '}' && --depth == 0)
            {
                auto line_begin = i;
                while (line_begin > 0 && text[line_begin - 1] != '\n')
                    --line_begin;
                return line_begin;
            }
        }
        return -1;
    }
    return -1;
}

/// Where an include goes in `text`: after its last `#include` line, else after `#pragma once`, else at the top.
u32 include_insertion(cc::string_view text)
{
    auto after = u32(0);
    isize begin = 0;
    for (auto i = isize(0); i < text.size(); ++i)
    {
        if (text[i] != '\n')
            continue;
        auto const line = trimmed(text.subview({.start = begin, .end = i}));
        if (line.starts_with("#include") || (after == 0 && line.starts_with("#pragma once")))
            after = u32(i + 1);
        begin = i + 1;
    }
    return after;
}

/// What fwd.hh declares `def` with — its head up to the name, the enum-base included — or empty when it needs no declaration there.
/// A specialization needs none: its primary template is what fwd.hh declares.
/// `blocked` is set where no declaration can be written: a template parameter's default argument belongs on the declaration alone,
/// so moving it there is a rewrite of the definition this fix does not make.
cc::string declaration_of(lint_context const& ctx, isize def, bool& blocked)
{
    auto const& n = ctx.tree[def];
    auto const text = ctx.source.text();
    auto const after_name = n.name.byte_end;
    auto i = isize(after_name);
    while (i < text.size() && cc::is_space(text[i]))
        ++i;
    if (i < text.size() && text[i] == '<')
        return {};
    // an enum's head runs to its brace, so a base of builtin words (`unsigned char`), which has no enum_base_name, comes along
    auto end = isize(after_name);
    if (n.kind == node_kind::enum_definition)
        while (end < text.size() && text[end] != '{')
            ++end;
    auto const head = trimmed(text.subview({.start = isize(n.span.byte_begin), .end = end}));
    if (head.starts_with("template") && head.contains('='))
    {
        blocked = true;
        return {};
    }
    return cc::string(head) + ";";
}

void check(lint_context& ctx)
{
    if (is_implementation_file(ctx.source.path()))
        return;

    auto const text = ctx.source.text();
    auto fwd = cc::optional<fwd_header>();
    auto fwd_looked_up = false;

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
                //
                // Emptiness is decided on the SPAN, never by asking the buffer for its text.
                // Only an enum carries an enum-base, so on a record this span is absent and still default-constructed,
                // and its file_id of 0 belongs to whichever file happened to be parsed first.
                auto const base = ctx.tree[defs[k]].enum_base_name;
                if (!base.empty())
                    edits.push_back(insertion(base.byte_begin, cc::string(name_components(ns_name)[0]) + "::"));
            }

            // A qualified definition needs its type declared in the namespace first, which is the library's fwd.hh's job.
            // The fix declares there what it does not yet, and includes it where the header does not; without a fwd.hh there is no fix.
            if (!fwd_looked_up)
            {
                fwd = find_fwd_header(ctx);
                fwd_looked_up = true;
            }
            auto why_no_fix = cc::string();
            if (!fwd.has_value())
                why_no_fix = cc::string("no fix: no fwd.hh was found in this header's folder or above it, below its "
                                        "src/ folder. ")
                           + "Declare the type in its library's fwd.hh, inside `namespace " + ns_name
                           + "`, include that fwd.hh here, and then define it qualified";
            else
            {
                auto const fwd_text = fwd.value().buffer->text();
                auto const fwd_id = fwd.value().buffer->file_id();
                auto declarations = cc::string();
                for (auto k = i; k <= last; ++k)
                {
                    auto blocked = false;
                    auto const declaration = declaration_of(ctx, defs[k], blocked);
                    if (blocked)
                        why_no_fix = cc::string("no fix: a template parameter's default argument belongs on the "
                                                "declaration in ")
                                   + fwd.value().include
                                   + " alone; declare the template there with its defaults, drop them here, and define "
                                     "it qualified";
                    else if (!declaration.empty()
                             && !declares_type(fwd_text, ctx.source.span_text(ctx.tree[defs[k]].name)))
                        declarations += declaration + "\n";
                }
                if (!declarations.empty())
                {
                    auto const at = namespace_insertion(fwd_text, ns_name);
                    auto const edit_at = [&](u32 offset, cc::string replacement)
                    {
                        return text_edit{.span = {.file_id = fwd_id, .byte_begin = offset, .byte_end = offset},
                                         .replacement = cc::move(replacement)};
                    };
                    if (at >= 0)
                        edits.push_back(edit_at(u32(at), declarations));
                    else
                        edits.push_back(edit_at(u32(fwd_text.size()),
                                                cc::string(fwd_text.ends_with("\n") ? "" : "\n") + "\nnamespace " + ns_name
                                                    + "\n{\n" + declarations + "} // namespace " + ns_name + "\n"));
                }
                auto const included
                    = text.contains(cc::string("<") + fwd.value().include + ">") || text.contains("\"fwd.hh\"");
                if (!included)
                    edits.push_back(insertion(include_insertion(text), "#include <" + fwd.value().include + ">\n"));
            }

            for (auto k = i; k <= last; ++k)
            {
                auto const& def = ctx.tree[defs[k]];
                auto const def_name = ctx.source.span_text(def.name);

                auto f = finding{
                    .rule_id = k_id,
                    .span = def.name,
                    .message = cc::string("`") + def_name + "` should be defined as `" + ns_name + "::" + def_name
                             + "`, not inside an open `namespace " + ns_name + "`",
                    .sev = severity::warning,
                };
                if (why_no_fix.empty())
                    f.suggested_fix = fix{.edits = edits};
                else
                    f.message += "; " + why_no_fix;
                ctx.report(cc::move(f));
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
