#include "dialect.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/to_string.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

namespace
{
using namespace sgl;
using namespace sgl::check;
using namespace sgl::emit;
using namespace sgl::emit::impl;

using level = builtins::precedence;
using rendered = builtins::written;

/// Shortest text that reads back as `value`, always with a decimal point: `1.0`, `0.45`, `1.0e+20`.
/// `value` must be finite.
cc::string literal_text(f64 value)
{
    auto text = cc::to_string(value);
    if (text.contains('.'))
        return text;
    auto const exponent = text.find('e');
    if (exponent < 0)
        text += ".0";
    else
        text.insert_range_at(exponent, ".0");
    return text;
}

/// `-2147483648` is no literal anywhere: it is a minus in front of a number that does not fit.
cc::string int_literal_text(i32 value)
{
    if (value == -2147483647 - 1)
        return "(-2147483647 - 1)";
    return cc::to_string(value);
}

struct writer
{
    plan& p;
    dialect const& d;
    cc::string out;
    /// How many levels the next line is indented by; the body of the function is level 1.
    int depth = 1;
    /// The entry point holds a `@coherent` member (EMIT-150).
    bool is_device_coherent = false;
    /// The images the entry point loads a texel of, where the target fences a store before a load (EMIT-151).
    cc::vector<flat_binding_member> fenced_images;
    /// EMIT-174: a member held as pieces that a write reaches into, and the local it is gathered in meanwhile.
    flat_expr_id gathered_member = flat_expr_id::none;
    cc::string gathered_name;

    /// EMIT-181: the files the line table reads, and the statement whose source line the next line writes.
    options const& sources;
    flat_stmt const* current = nullptr;
    cc::vector<line_origin> lines;
    /// Per file, the offset each of its lines starts at; built on the first line asked of the file.
    cc::vector<cc::vector<u32>> line_starts;
    /// The text's lines up to `counted`, which the next emitted line number counts on from.
    isize counted = 0;
    i32 newlines = 0;
    /// EMIT-182: the source line the target's compiler takes the next line for, and the calls the last comment named.
    source_line assumed;
    cc::vector<source_line> commented;
    /// The statement just begun, until its first line is written.
    flat_stmt const* unwritten = nullptr;
    /// EMIT-183: every print written into sg's debug buffer.
    cc::vector<debug_site> debug_sites;

    /// FNV-1a over `text`, continuing from `hash`.
    [[nodiscard]] static u32 hashed(u32 hash, cc::string_view text)
    {
        for (auto const c : text)
            hash = (hash ^ u32(u8(c))) * 16777619u;
        return (hash ^ 0xFFu) * 16777619u;
    }

    /// The family and lanes of a printable type (CHK-424): `float3` is `float` in 3, and an enum one `int`.
    [[nodiscard]] debug_value debug_value_of(type_id type) const
    {
        auto result = debug_value{.family = "int", .type = cc::string(p.m.name_of(type))};
        // the prelude declares `bool` an enum, which is no enum a message writes as its int
        if (p.m.at(type).kind == check::type_kind::enumeration && result.type != "bool")
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

    /// EMIT-183: a print as one record in sg's debug buffer, written whole or counted as dropped.
    void print_record(flat_stmt const& s, flat_print const& n)
    {
        if (!p.writes_debug)
            return;
        auto site = debug_site();
        if (n.message >= 0 && n.message < p.e.messages.size())
        {
            site.pieces = p.e.messages[n.message].pieces;
            site.is_assert = p.e.messages[n.message].is_assert;
        }
        if (!sources.files.empty())
            site.origin = origin_of(s);

        auto words = cc::vector<cc::string>();
        for (auto const id : p.e.at(n.values))
        {
            auto const type = p.e.at(id).type;
            auto value = debug_value_of(type);
            // bound once, so a value is computed once whatever its lanes
            auto const name = p.names.mint("printed");
            auto declaration = cc::string();
            auto const value_text = expr(id).text;
            d.write_local(declaration, {.name = name, .type = type_text(p, d, type), .value = value_text});
            line(declaration);
            for (auto lane = 0; lane < value.lanes; ++lane)
            {
                auto const lane_text = value.lanes == 1 ? cc::string(name) : cc::format("{}.{}", name, "xyzw"[lane]);
                words.push_back(d.debug_word(lane_text, value.family));
            }
            site.values.push_back(cc::move(value));
        }

        auto id = hashed(2166136261u, p.entry_name);
        id = hashed(id, cc::format("{}", n.message));
        for (auto const& piece : site.pieces)
            id = hashed(id, piece);
        if (site.origin.at.file >= 0)
            id = hashed(id, cc::format("{}:{}", name_of(site.origin.at.file), site.origin.at.line));
        // one line inlined along two call chains is two sites, which another permutation of this entry point may hold
        for (auto const& from : site.origin.inlined_from)
            if (from.file >= 0)
                id = hashed(id, cc::format("<{}:{}", name_of(from.file), from.line));
        site.id = id;

        auto const count = isize(words.size()) + 2;
        auto const at = p.names.mint("debug_at");
        if (auto const declaration = d.debug_declare(at); !declaration.empty())
            line(declaration);
        line(d.debug_reserve(at, count));
        open(condition_text("if", d.debug_fits(at, count)));
        line(d.debug_store(at, 0, cc::format("{}u", id)));
        line(d.debug_store(at, 1, cc::format("{}u", count)));
        for (auto i = isize(0); i < words.size(); ++i)
            line(d.debug_store(at, i + 2, words[i]));
        reopen("else");
        line(d.debug_drop());
        close();
        debug_sites.push_back(cc::move(site));
    }

    /// The name a target writes member `name` of struct `type` with, which the plan may have renamed.
    [[nodiscard]] cc::string_view member_of(type_id type, cc::string_view name) const
    {
        auto const k = p.struct_of_type[index_of(type)];
        if (k >= 0)
            for (auto const& m : p.structs[k].members)
                if (m.source_name == name)
                    return m.name;
        return name;
    }

    static void indent(cc::string& text, int levels)
    {
        for (auto i = 0; i < levels; ++i)
            text += k_indent;
    }

    void line(cc::string_view text)
    {
        if (current != nullptr && !sources.files.empty())
            map_line(origin_of(*current));
        indent(out, depth);
        out += text;
        out += "\n";
        if (assumed.file >= 0)
        {
            ++assumed.line;
            for (auto const c : text)
                assumed.line += c == '\n' ? 1 : 0;
        }
    }

    /// The line of `file` that byte `offset` is on; a line ends at `\n`, `\r\n` or a bare `\r`.
    [[nodiscard]] i32 line_at(i32 file, u32 offset)
    {
        if (line_starts.size() < sources.files.size())
            line_starts.resize_to_defaulted(sources.files.size());
        auto& starts = line_starts[file];
        if (starts.empty())
        {
            auto const text = cc::string_view(sources.files[file].file.source);
            starts.push_back(0);
            for (auto i = isize(0); i < text.size(); ++i)
                if (text[i] == '\n' || (text[i] == '\r' && (i + 1 >= text.size() || text[i + 1] != '\n')))
                    starts.push_back(u32(i + 1));
        }
        auto line = isize(1);
        while (line < starts.size() && starts[line] <= offset)
            ++line;
        return i32(line);
    }

    /// The source line of an expression or a statement of `file`; none where the file is not one of `sources`.
    [[nodiscard]] source_line line_of(i32 file, ast::expr_id expr, ast::stmt_id stmt)
    {
        if (file < 0 || file >= sources.files.size())
            return {};
        auto const& f = sources.files[file];
        auto const form = ast::is_valid(expr) ? f.ast.at(expr).form
                        : ast::is_valid(stmt) ? f.ast.at(stmt).form
                                              : form_id::none;
        if (!is_valid(form))
            return {};
        return {.file = file, .line = line_at(file, f.file.at(form).where.offset)};
    }

    /// Where statement `s` came from: its own line, and the calls it was inlined through.
    /// The prelude's frames are left out wherever another frame remains.
    [[nodiscard]] line_origin origin_of(flat_stmt const& s)
    {
        // innermost first: the statement, then its calls from the inside out
        auto frames = cc::vector<source_line>();
        frames.push_back(line_of(s.from.file, s.from.expr, s.from.stmt));
        auto const calls = p.e.at(s.inlined_through);
        for (auto i = calls.size(); i > 0; --i)
            frames.push_back(line_of(calls[i - 1].file, calls[i - 1].call, ast::stmt_id::none));

        auto const is_prelude = [&](source_line const& l) { return l.file >= 0 && l.file < p.m.prelude_files; };
        auto any_outside = false;
        for (auto const& f : frames)
            any_outside = any_outside || (f.file >= 0 && !is_prelude(f));
        auto result = line_origin();
        for (auto const& f : frames)
        {
            if (f.file < 0 || (any_outside && is_prelude(f)))
                continue;
            if (result.at.file < 0)
                result.at = f;
            else
                result.inlined_from.push_back(f);
        }
        return result;
    }

    /// The line the text's next line is, counted from 1.
    [[nodiscard]] i32 next_line()
    {
        for (; counted < out.size(); ++counted)
            newlines += out[counted] == '\n' ? 1 : 0;
        return newlines + 1;
    }

    [[nodiscard]] cc::string_view name_of(i32 file) const
    {
        return file >= 0 && file < sources.names.size() ? sources.names[file] : cc::string_view("<sgl>");
    }

    /// The name of `file` without its directories, which a comment is short enough with.
    [[nodiscard]] cc::string_view base_name_of(i32 file) const
    {
        auto const name = name_of(file);
        auto start = name.size();
        while (start > 0 && name[start - 1] != '/' && name[start - 1] != '\\')
            --start;
        return name.subview({.offset = start, .size = name.size() - start});
    }

    /// Records where the next line came from, behind the comment and the `#line` that say so where the target has them.
    void map_line(line_origin o)
    {
        if (o.at.file < 0)
            return;
        // only a statement's first line says where it is: a brace or an `else` after it is no line of its own
        auto const is_first = unwritten == current;
        unwritten = nullptr;
        if (is_first && sources.write_directives && d.has_line_directives())
        {
            if (!ast::impl::is_equal(o.inlined_from, commented))
            {
                commented = o.inlined_from;
                if (!o.inlined_from.empty())
                {
                    indent(out, depth);
                    out += "// inlined from";
                    for (auto i = isize(0); i < o.inlined_from.size(); ++i)
                        out.appendf("{} {}:{}", i == 0 ? "" : " >", base_name_of(o.inlined_from[i].file),
                                    o.inlined_from[i].line);
                    out += "\n";
                    if (assumed.file >= 0)
                        ++assumed.line;
                }
            }
            if (assumed != o.at)
            {
                out.appendf("#line {} \"", o.at.line);
                // a `#line` name is a string literal, so a Windows path's backslashes are escaped
                for (auto const c : name_of(o.at.file))
                {
                    if (c == '\\' || c == '"')
                        out += '\\';
                    out += c;
                }
                out += "\"\n";
                assumed = o.at;
            }
        }
        o.emitted = next_line();
        lines.push_back(cc::move(o));
    }

    /// `head` and the brace that opens its body, where the target puts it.
    void open(cc::string_view head)
    {
        if (d.is_c_like())
        {
            line(head);
            line("{");
        }
        else
            line(cc::format("{} {{", head));
        ++depth;
    }

    /// Closes one body and opens the next under `head`: an `else`.
    void reopen(cc::string_view head)
    {
        --depth;
        if (d.is_c_like())
        {
            line("}");
            line(head);
            line("{");
        }
        else
            line(cc::format("}} {} {{", head));
        ++depth;
    }

    void close(cc::string_view tail = "")
    {
        --depth;
        line(cc::format("}}{}", tail));
    }

    cc::string condition_text(cc::string_view keyword, cc::string_view condition) const
    {
        return d.is_c_like() ? cc::format("{} ({})", keyword, condition) : cc::format("{} {}", keyword, condition);
    }

    static cc::string wrapped(rendered r, level needed) { return builtins::wrapped(cc::move(r), needed); }

    /// `&&` and `||` never stand bare inside each other: WGSL refuses the mix, and no reader should need the ladder.
    static rendered logical(cc::string_view op, level own, rendered lhs, rendered rhs)
    {
        auto const needed_left = lhs.binds == own ? own : level::comparison;
        return {.text = cc::format("{} {} {}", wrapped(cc::move(lhs), needed_left), op,
                                   wrapped(cc::move(rhs), level::comparison)),
                .binds = own};
    }

    /// True when writing `id` puts lines in front of the statement that holds it: a struct built member by member, or
    /// a builtin whose writer says it may in this language.
    /// Such an expression cannot stand where it is evaluated more than once, or behind an `else`.
    bool writes_lines(flat_expr_id id) const
    {
        auto const& x = p.e.at(id);
        if (needs_member_assignment(x))
            return true;
        if (auto const* const c = x.node.try_as<flat_call>())
        {
            // CHK-427: a body in a target's own text is lines alone
            if (c->text >= 0)
                return true;
            if (auto const* const record = p.m.builtin_function(c->intrinsic);
                record != nullptr && record->write.writes_lines != nullptr && record->write.writes_lines(d.language()))
                return true;
        }
        auto result = false;
        check::impl::for_each_operand(p.e, x, [&](flat_expr_id operand) { result = result || writes_lines(operand); });
        return result;
    }

    /// An atomic call in HLSL, which hands its value before through an out parameter a statement of its own declares.
    [[nodiscard]] bool is_hlsl_atomic(flat_expr_id id) const
    {
        if (d.language() != builtins::language::hlsl)
            return false;
        auto const* const c = p.e.at(id).node.try_as<flat_call>();
        auto const* const record = c != nullptr ? p.m.builtin_function(c->intrinsic) : nullptr;
        if (record == nullptr || !record->is_atomic)
            return false;
        // the device's own float atomic is one call whose value is the whole of it, as vulkan writes it
        if (p.native_float_atomics && !p.e.at(c->arguments).empty())
            if (auto const& t = p.m.at(p.e.at(p.e.at(c->arguments)[0]).type);
                t.kind == check::type_kind::atomic && p.m.name_of(t.element) == builtins::k_float)
                return false;
        return true;
    }

    /// The image member whose texel `x` loads or, with `is_store`, stores; null for any other expression.
    /// An element of a binding array of images is its array's member.
    [[nodiscard]] flat_binding_member const* image_accessed_by(flat_expr const& x, bool is_store) const
    {
        auto const* const c = x.node.try_as<flat_call>();
        auto const* const record = c != nullptr ? p.m.builtin_function(c->intrinsic) : nullptr;
        if (record == nullptr || p.e.at(c->arguments).empty())
            return nullptr;
        if (is_store ? !record->is_image_store : record->is_image_store || record->name != "load")
            return nullptr;
        auto const image = p.e.at(c->arguments)[0];
        if (p.m.at(p.e.at(image).type).kind != check::type_kind::image)
            return nullptr;
        auto member = image;
        if (auto const* const element = p.e.at(member).node.try_as<flat_element>())
            member = element->object;
        return p.e.at(member).node.try_as<flat_binding_member>();
    }
    [[nodiscard]] flat_binding_member const* image_loaded_by(flat_expr const& x) const
    {
        return image_accessed_by(x, false);
    }

    /// `img.fence();` behind a store to an image the entry point also loads, so this thread's later load sees it.
    void fence_after(flat_expr_id id)
    {
        auto const* const b = image_accessed_by(p.e.at(id), true);
        if (b == nullptr)
            return;
        for (auto const& loaded : fenced_images)
            if (loaded.binding == b->binding && loaded.member == b->member)
            {
                auto const* const c = p.e.at(id).node.try_as<flat_call>();
                line(cc::format("{}.fence();", wrapped(expr(p.e.at(c->arguments)[0]), level::primary)));
                return;
            }
    }

    bool needs_member_assignment(flat_expr const& x) const
    {
        return x.node.is<flat_construct>() && !is_builtin_type(p.m, x.type)
            && (!d.has_struct_constructor() || has_pieces(x.type));
    }

    /// EMIT-174: whether `type` is an edge struct holding an array or a matrix as one member per piece.
    [[nodiscard]] bool has_pieces(type_id type) const
    {
        if (!is_valid(type) || is_builtin_type(p.m, type) || p.m.at(type).kind != type_kind::structure
            || p.struct_of_type[index_of(type)] < 0)
            return false;
        for (auto const& m : p.structs[p.struct_of_type[index_of(type)]].members)
            if (m.pieces > 0)
                return true;
        return false;
    }

    /// EMIT-174: `raw`, an edge struct's member held as one member per piece, rebuilt into the value of `type`.
    /// `is_local`: always into a mutable local, which a matrix otherwise is not.
    cc::string rebuilt_from_pieces(type_id type, cc::string_view raw, bool is_local = false)
    {
        if (is_array(type))
        {
            auto const name = p.names.mint("gathered");
            auto declaration = cc::string();
            d.write_local(
                declaration,
                {.name = name, .type = type_text(p, d, type), .dimensions = array_dimensions(p, type), .is_mut = true});
            line(declaration);
            for (auto i = isize(0); i < isize(p.m.at(type).count); ++i)
                line(cc::format("{}[{}] = {}_{};", name, i, raw, i));
            return name;
        }
        // a matrix from its columns, which HLSL's constructor reads as rows
        auto const n = p.m.name_of(type) == "mat2" ? 2 : p.m.name_of(type) == "mat3" ? 3 : 4;
        auto text = cc::format("{}(", type_text(p, d, type));
        for (auto i = 0; i < n; ++i)
            text.appendf("{}{}_{}", i == 0 ? "" : ", ", raw, i);
        text += ")";
        if (d.language() == builtins::language::hlsl)
            text = cc::format("transpose({})", text);
        if (!is_local)
            return text;
        auto const name = p.names.mint("gathered");
        auto declaration = cc::string();
        d.write_local(declaration, {.name = name, .type = type_text(p, d, type), .value = text, .is_mut = true});
        line(declaration);
        return name;
    }

    /// `text` without an outermost `transpose(...)` around all of it; empty where there is none.
    [[nodiscard]] static cc::string_view transposed_inner(cc::string_view text)
    {
        constexpr auto head = cc::string_view("transpose(");
        if (!text.starts_with(head) || !text.ends_with(")"))
            return {};
        auto depth = 0;
        for (auto i = head.size() - 1; i < text.size(); ++i)
        {
            depth += text[i] == '(' ? 1 : text[i] == ')' ? -1 : 0;
            if (depth == 0)
                return i == text.size() - 1 ? text.subview({.start = head.size(), .end = text.size() - 1})
                                            : cc::string_view();
        }
        return {};
    }

    [[nodiscard]] static bool is_name(cc::string_view text)
    {
        if (text.empty())
            return false;
        for (auto const c : text)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
                return false;
        return true;
    }

    /// EMIT-174: the member held as pieces that a write to `place` reaches into, other than through one named piece.
    [[nodiscard]] flat_expr_id gathered_by(flat_expr_id place) const
    {
        for (auto id = place; is_valid(id);)
        {
            auto const& x = p.e.at(id);
            if (id != place && p.accesses[index_of(id)].held == held_as::pieces)
                return id;
            if (auto const* const element = x.node.try_as<flat_element>())
            {
                if (is_piece(p, *element))
                    return flat_expr_id::none;
                id = element->object;
            }
            else if (auto const* const member = x.node.try_as<flat_member>())
                id = member->object;
            else
                break;
        }
        return flat_expr_id::none;
    }

    /// EMIT-174: stores `value`, of `type`, into `raw`'s pieces, its elements or its columns, from one evaluation.
    void store_pieces(cc::string_view raw, type_id type, cc::string_view value)
    {
        auto const is_hlsl_matrix = !is_array(type) && d.language() == builtins::language::hlsl;
        auto name = cc::string(value);
        if (is_hlsl_matrix || !is_name(value))
        {
            // HLSL indexes a matrix by its rows, so its columns are the rows of its transpose
            auto const inner = is_hlsl_matrix ? transposed_inner(value) : cc::string_view();
            name = p.names.mint("scattered");
            auto declaration = cc::string();
            d.write_local(declaration, {.name = name,
                                        .type = type_text(p, d, type),
                                        .value = !is_hlsl_matrix ? cc::string(value)
                                               : !inner.empty()  ? cc::string(inner)
                                                                 : cc::format("transpose({})", value),
                                        .dimensions = array_dimensions(p, type)});
            line(declaration);
        }
        auto const n = is_array(type)              ? i32(p.m.at(type).count)
                     : p.m.name_of(type) == "mat2" ? 2
                     : p.m.name_of(type) == "mat3" ? 3
                                                   : 4;
        for (auto i = 0; i < n; ++i)
            line(cc::format("{}_{} = {}[{}];", raw, i, name, i));
    }

    /// `nonuniform i`, whose own text is `i`; `uniform i` is no such mark, since it is written as the plain index.
    [[nodiscard]] bool is_nonuniform_mark(flat_expr_id id) const
    {
        auto const* const c = p.e.at(id).node.try_as<flat_call>();
        auto const* const record = c != nullptr ? p.m.builtin_function(c->intrinsic) : nullptr;
        return record != nullptr && record->mark == builtins::index_mark::nonuniform;
    }

    [[nodiscard]] bool is_array(check::type_id type) const
    {
        return is_valid(type) && p.m.at(type).kind == check::type_kind::array;
    }

    /// The array `raw` holds as 4-vectors, rebuilt into a local of its own elements, each read through `lanes`.
    /// Every index path down `type`'s array dimensions to its innermost elements, `[0][1]`, in order.
    [[nodiscard]] cc::vector<cc::string> index_paths(type_id type) const
    {
        auto paths = cc::vector<cc::string>();
        paths.push_back({});
        while (is_array(type) && p.m.builtin_type_of(type) == nullptr)
        {
            auto deeper = cc::vector<cc::string>();
            for (auto const& path : paths)
                for (auto i = isize(0); i < isize(p.m.at(type).count); ++i)
                    deeper.push_back(cc::format("{}[{}]", path, i));
            paths = cc::move(deeper);
            type = p.m.at(type).element;
        }
        return paths;
    }

    cc::string narrowed_array(flat_expr const& x, cc::string const& raw, cc::string_view lanes)
    {
        auto const name = p.names.mint("narrowed");
        auto declaration = cc::string();
        d.write_local(
            declaration,
            {.name = name, .type = type_text(p, d, x.type), .dimensions = array_dimensions(p, x.type), .is_mut = true});
        line(declaration);
        // EMIT-168: an element of an array of mat3 is a float4x3, which narrows as one does
        auto const is_matrix = is_mat3(p.m, innermost_of(p.m, x.type));
        for (auto const& at : index_paths(x.type))
            line(is_matrix ? cc::format("{}{} = ((float3x3){}{});", name, at, raw, at)
                           : cc::format("{}{} = {}{}{};", name, at, raw, at, lanes));
        return name;
    }

    /// Stores `value`, an array of 3-vectors, into `raw`, which holds it as 4-vectors, element by element.
    void widen_array_into(cc::string_view raw, type_id type, cc::string_view value)
    {
        auto const name = p.names.mint("widened");
        auto declaration = cc::string();
        d.write_local(
            declaration,
            {.name = name, .type = type_text(p, d, type), .value = value, .dimensions = array_dimensions(p, type)});
        line(declaration);
        auto const is_matrix = is_mat3(p.m, innermost_of(p.m, type));
        for (auto const& at : index_paths(type))
            line(is_matrix ? cc::format("{}{} = sgl_widen_mat3({}{});", raw, at, name, at)
                           : cc::format("{}{}.xyz = {}{};", raw, at, name, at));
    }

    /// EMIT-173: `text`, the expression `id` as its target holds it, converted back to the value it reads as.
    /// A raw one stays as held: it is a place, or an array indexed in place.
    rendered read_as_value(flat_expr_id id, rendered text)
    {
        auto const& access = p.accesses[index_of(id)];
        if (access.is_raw)
            return text;
        switch (access.held)
        {
        case held_as::itself:
            return text;
        case held_as::widened_mat3:
            return {.text = cc::format("((float3x3){})", text.text)};
        case held_as::packed_tail:
            return {.text = cc::format("{}({})", type_text(p, d, p.e.at(id).type), text.text)};
        case held_as::widened_array:
            return {.text = narrowed_array(p.e.at(id), text.text, access.lanes)};
        case held_as::pieces:
            return {.text = rebuilt_from_pieces(p.e.at(id).type, text.text)};
        case held_as::vector_mat2:
            return {.text = cc::format("sgl_narrow_mat2({})", text.text)};
        }
        return text;
    }

    /// Whether a value of `type` is a struct written with padding fields, which a target writes as memory holds it.
    [[nodiscard]] bool is_padded(type_id type) const
    {
        if (is_builtin_type(p.m, type) || is_array(type) || !is_valid(type) || index_of(type) >= p.struct_of_type.size())
            return false;
        auto const at = p.struct_of_type[index_of(type)];
        return at >= 0 && p.structs[at].memory_size > 0;
    }

    void build_struct(flat_expr const& x, cc::string_view name)
    {
        auto declaration = cc::string();
        auto const dimensions = array_dimensions(p, x.type);
        d.write_local(declaration, {.name = name,
                                    .type = type_text(p, d, x.type),
                                    .dimensions = dimensions,
                                    .is_mut = true,
                                    .is_zeroed = is_padded(x.type)});
        line(declaration);

        auto const arguments = p.e.at(x.node.as<flat_construct>().arguments);
        if (is_array(x.type))
        {
            for (auto i = isize(0); i < arguments.size(); ++i)
                line(cc::format("{}[{}] = {};", name, i, expr(arguments[i]).text));
            return;
        }
        auto const& planned = p.structs[p.struct_of_type[index_of(x.type)]];
        for (auto i = isize(0); i < arguments.size() && i < planned.member_of.size(); ++i)
            if (planned.member_of[i] >= 0 && planned.members[planned.member_of[i]].pieces > 0)
                store_pieces(cc::format("{}.{}", name, planned.members[planned.member_of[i]].name),
                             p.e.at(arguments[i]).type, expr(arguments[i]).text);
            else if (planned.member_of[i] >= 0 && holds_widened_array(p, x.type, i))
                widen_array_into(cc::format("{}.{}", name, planned.members[planned.member_of[i]].name),
                                 p.e.at(arguments[i]).type, expr(arguments[i]).text);
            else if (planned.member_of[i] >= 0)
                line(cc::format("{}.{} = {};", name, planned.members[planned.member_of[i]].name,
                                holds_widened_mat3(p, x.type, i)
                                    ? cc::format("sgl_widen_mat3({})", expr(arguments[i]).text)
                                    : expr(arguments[i]).text));
    }

    /// The vector local whose every field, in order, the arguments from `at` on are; none where they are not.
    flat_expr_id spread_vector_at(cc::span<flat_expr_id const> arguments, isize at) const
    {
        auto const* const first = p.e.at(arguments[at]).node.try_as<flat_member>();
        if (first == nullptr || first->is_swizzle() || first->member != 0)
            return flat_expr_id::none;
        auto const* const local = p.e.at(first->object).node.try_as<flat_local_ref>();
        auto const& vector = p.m.at(p.e.at(first->object).type);
        if (local == nullptr || !is_builtin_type(p.m, p.e.at(first->object).type) || !vector.has_swizzles)
            return flat_expr_id::none;
        auto const width = isize(vector.members.count);
        if (width < 2 || at + width > arguments.size())
            return flat_expr_id::none;
        for (auto k = isize(1); k < width; ++k)
        {
            auto const* const m = p.e.at(arguments[at + k]).node.try_as<flat_member>();
            auto const* const ref = m != nullptr ? p.e.at(m->object).node.try_as<flat_local_ref>() : nullptr;
            if (ref == nullptr || m->is_swizzle() || m->member != k || ref->local != local->local)
                return flat_expr_id::none;
        }
        return first->object;
    }

    /// `is_broken` gives every argument a line of its own, which is how a struct built as a statement's value reads.
    rendered construct(flat_expr const& x, bool is_broken)
    {
        if (needs_member_assignment(x))
        {
            // A construction in the middle of an expression: every expression is pure, so building it first changes nothing.
            // a type a target renamed `ray_` takes no second underscore, which C++ reserves
            auto const spelled = cc::string(type_text(p, d, x.type));
            auto type = cc::string_view(spelled);
            while (type.ends_with("_"))
                type = type.subview({.start = 0, .end = type.size() - 1});
            auto name = p.names.mint(is_array(x.type) ? cc::string("array_value") : cc::format("{}_value", type));
            build_struct(x, name);
            return {.text = cc::move(name)};
        }

        // A void argument stands for a field no target declares, and its value is no value to write (EMIT-106).
        auto arguments = cc::vector<flat_expr_id>();
        for (auto const a : p.e.at(x.node.as<flat_construct>().arguments))
            if (p.e.at(a).type != checked_module::void_type)
                arguments.push_back(a);
        // a vector stays on its line: its arguments are short, and `vec3f(0.45, 0.8, -0.4)` is how a shader reads
        auto const is_split = is_broken && arguments.size() > 1 && !is_builtin_type(p.m, x.type);
        auto const is_vector = is_builtin_type(p.m, x.type) && p.m.at(x.type).has_swizzles;
        auto text = cc::string(type_text(p, d, x.type));
        text += "(";
        for (auto i = isize(0), written = isize(0); i < arguments.size(); ++written)
        {
            if (is_split)
            {
                text += written == 0 ? "\n" : ",\n";
                indent(text, depth + 1);
            }
            else if (written != 0)
                text += ", ";
            // `..v` spread every field of a vector local: every target's vector takes the vector whole
            if (auto const whole = is_vector ? spread_vector_at(arguments, i) : flat_expr_id::none; is_valid(whole))
            {
                text += expr(whole).text;
                i += p.m.at(p.m.at(p.e.at(whole).type).members).size();
                continue;
            }
            text += expr(arguments[i]).text;
            ++i;
        }
        if (is_split)
        {
            text += "\n";
            indent(text, depth);
        }
        text += ")";
        return {.text = cc::move(text)};
    }

    /// A value inside a root its target writes as a memory form: a constant block, or a buffer's element.
    struct memory_ref
    {
        memory_form const* form = nullptr;
        /// What reaches the root: the block's global, or `buffer[index]`.
        cc::string root;
        /// Member positions from the root down, as `memory_leaf::path` holds them.
        cc::vector<i32> path;
        type_id type = type_id::none;
        /// EMIT-181: the element of an array leaf this addresses, as text; empty for the leaf itself or any other.
        cc::string index;
    };

    /// Where `id` reads or writes inside a root written as its memory form; nothing for every other expression.
    /// A component of a builtin, `v.x`, is not one: it is read from the rebuilt value, and written by `assign`.
    cc::optional<memory_ref> memory_of(flat_expr_id id)
    {
        auto const& x = p.e.at(id);
        if (auto const* const b = x.node.try_as<flat_binding_member>())
        {
            if (resource_of(p, b->binding, b->member) >= 0 || workgroup_of(p, b->binding, b->member) >= 0)
                return {};
            auto const& block = *block_of(p, b->binding);
            if (!block.form.has_value())
                return {};
            return memory_ref{.form = &block.form.value(),
                              .root = block.name,
                              .path = {block.block_member_of[b->member]},
                              .type = x.type};
        }
        if (auto const* const element = x.node.try_as<flat_buffer_element>())
        {
            auto const* const buffer = p.e.at(element->buffer).node.try_as<flat_binding_member>();
            auto const found = buffer != nullptr ? resource_of(p, buffer->binding, buffer->member) : -1;
            if (found < 0 || !p.resources[found].element_form.has_value())
                return {};
            return memory_ref{.form = &p.resources[found].element_form.value(),
                              .root = cc::format("{}[{}]", p.resources[found].name, expr(element->index).text),
                              .path = {},
                              .type = x.type};
        }
        if (auto const* const member = x.node.try_as<flat_member>())
        {
            if (is_builtin_type(p.m, p.e.at(member->object).type))
                return {};
            auto inner = memory_of(member->object);
            if (!inner.has_value())
                return {};
            inner.value().path.push_back(member->member);
            inner.value().type = x.type;
            return inner;
        }
        // EMIT-181: an element of a packed array leaf is read and written through the leaf's own field
        if (auto const* const element = x.node.try_as<flat_element>(); element != nullptr && is_builtin_type(p.m, x.type))
        {
            auto inner = memory_of(element->object);
            if (!inner.has_value() || !inner.value().index.empty() || is_builtin_type(p.m, inner.value().type)
                || p.m.at(inner.value().type).kind != type_kind::array)
                return {};
            auto const& leaf = leaf_at(inner.value());
            if (leaf.count == 0)
                return {};
            auto index = expr(element->index).text;
            // a split element names its index once per scalar, so anything but a name or a literal is evaluated once
            if (leaf.is_split && !is_name(index))
            {
                auto const name = p.names.mint("at");
                auto declaration = cc::string();
                d.write_local(
                    declaration,
                    {.name = name, .type = type_text(p, d, p.e.at(element->index).type), .value = cc::move(index)});
                line(declaration);
                index = name;
            }
            inner.value().index = cc::move(index);
            inner.value().type = x.type;
            return inner;
        }
        return {};
    }

    static bool starts_with(cc::span<i32 const> path, cc::span<i32 const> prefix)
    {
        if (path.size() < prefix.size())
            return false;
        for (auto i = isize(0); i < prefix.size(); ++i)
            if (path[i] != prefix[i])
                return false;
        return true;
    }

    memory_leaf const& leaf_at(memory_ref const& r) const
    {
        for (auto const& leaf : r.form->leaves)
            if (leaf.path.size() == r.path.size() && starts_with(leaf.path, r.path))
                return leaf;
        CC_UNREACHABLE("every builtin value of a root is a leaf of its memory form");
    }

    cc::string field_text(memory_ref const& r, memory_leaf const& leaf, isize i) const
    {
        if (r.index.empty())
            return cc::format("{}.{}", r.root, r.form->fields[leaf.fields[i]].name);
        // EMIT-181: every element of a split array is its scalars back to back in one array
        auto const& field = r.form->fields[leaf.fields[0]];
        if (leaf.is_split)
            return cc::format("{}.{}[{} * {} + {}]", r.root, field.name, p.m.builtin_type_of(leaf.type)->leaf_count,
                              r.index, i);
        return cc::format("{}.{}[{}]", r.root, field.name, r.index);
    }

    /// How many fields hold the value `leaf` is, or each element of it: a split array's scalars live in one field.
    [[nodiscard]] isize pieces_of(memory_leaf const& leaf) const
    {
        return leaf.count > 0 && leaf.is_split ? isize(p.m.builtin_type_of(leaf.type)->leaf_count) : leaf.fields.size();
    }

    /// The text of a matrix of `record`'s size: its columns' count, which is also each column's.
    [[nodiscard]] static i32 matrix_size_of(builtins::type_record const& record)
    {
        return record.leaf_count == 16 ? 4 : record.leaf_count == 9 ? 3 : 2;
    }

    [[nodiscard]] static bool is_matrix_record(builtins::type_record const& record)
    {
        return record.leaf_count > 4 || record.hlsl == "float2x2";
    }

    /// EMIT-181: the whole array a packed array leaf `r` holds, rebuilt into a local element by element.
    cc::string read_packed_array(memory_ref const& r, memory_leaf const& leaf)
    {
        auto const name = p.names.mint("unpacked");
        auto declaration = cc::string();
        d.write_local(
            declaration,
            {.name = name, .type = type_text(p, d, r.type), .dimensions = array_dimensions(p, r.type), .is_mut = true});
        line(declaration);
        for (auto i = 0; i < leaf.count; ++i)
        {
            auto element = r;
            element.index = cc::to_string(i);
            element.type = leaf.type;
            line(cc::format("{}[{}] = {};", name, i, read_memory(element)));
        }
        return name;
    }

    /// The value `r` addresses, rebuilt from the fields that hold it.
    cc::string read_memory(memory_ref const& r)
    {
        auto const type = cc::string(type_text(p, d, r.type));
        // an array's whole-row elements are never split, so its one field is the array itself
        if (!is_builtin_type(p.m, r.type) && p.m.at(r.type).kind == type_kind::array)
        {
            auto const& leaf = leaf_at(r);
            if (leaf.count > 0 && (leaf.is_split || leaf.is_packed))
                return read_packed_array(r, leaf);
            return field_text(r, leaf, 0);
        }
        if (is_builtin_type(p.m, r.type))
        {
            auto const& leaf = leaf_at(r);
            if (leaf.is_widened)
                return cc::format("((float3x3){})", field_text(r, leaf, 0));
            if (leaf.is_packed)
                return cc::format("{}({})", type, field_text(r, leaf, 0));
            if (!leaf.is_split)
                return field_text(r, leaf, 0);
            auto const& record = *p.m.builtin_type_of(r.type);
            auto pieces = cc::vector<cc::string>();
            // a square matrix, 2, 3 or 4 columns of as many rows
            auto const n = matrix_size_of(record);
            if (is_matrix_record(record) && d.language() == builtins::language::hlsl)
            {
                // HLSL fills a matrix row by row from its scalars, and the fields hold it column by column.
                for (auto row = 0; row < n; ++row)
                    for (auto column = 0; column < n; ++column)
                        pieces.push_back(field_text(r, leaf, column * n + row));
            }
            else if (is_matrix_record(record) && !d.has_struct_constructor())
            {
                // MSL builds a matrix from its columns, not from its scalars.
                auto const column_type = cc::string(builtin_spelling(p, n == 2   ? "float2"
                                                                        : n == 3 ? "float3"
                                                                                 : "float4"));
                for (auto c = 0; c < n; ++c)
                {
                    auto column = cc::format("{}(", column_type);
                    for (auto row = 0; row < n; ++row)
                        column.appendf("{}{}", row == 0 ? "" : ", ", field_text(r, leaf, c * n + row));
                    pieces.push_back(column + ")");
                }
            }
            else
                for (auto i = isize(0); i < pieces_of(leaf); ++i)
                    pieces.push_back(field_text(r, leaf, i));
            auto text = cc::format("{}(", type);
            for (auto i = isize(0); i < pieces.size(); ++i)
                text.appendf("{}{}", i == 0 ? "" : ", ", pieces[i]);
            return text + ")";
        }

        // A struct is its members, each read the same way; a void member is no member of the text (EMIT-106).
        auto text = cc::format("{}{}", type, d.has_struct_constructor() ? "(" : "{");
        auto is_first = true;
        auto const members = p.m.at(p.m.at(r.type).members);
        for (auto i = isize(0); i < members.size(); ++i)
        {
            if (members[i].type == checked_module::void_type)
                continue;
            auto inner = r;
            inner.path.push_back(i32(i));
            inner.type = members[i].type;
            text.appendf("{}{}", is_first ? "" : ", ", read_memory(inner));
            is_first = false;
        }
        return text + (d.has_struct_constructor() ? ")" : "}");
    }

    /// The text of the value at `path` below `value`, a value of the root's type at `from`: its members by their names.
    cc::string member_path_text(cc::string_view value, type_id from, cc::span<i32 const> path) const
    {
        auto text = cc::string(value);
        auto type = from;
        for (auto const step : path)
        {
            auto const& planned = p.structs[p.struct_of_type[index_of(type)]];
            text.appendf(".{}", planned.members[planned.member_of[step]].name);
            type = p.m.at(p.m.at(type).members)[step].type;
        }
        return text;
    }

    /// Stores `value` at `r`, or only its `component` where one is given, piece by piece where the value is split.
    void write_memory(memory_ref const& r, i32 component, cc::string_view value, type_id value_type)
    {
        if (is_builtin_type(p.m, r.type))
        {
            auto const& leaf = leaf_at(r);
            auto const& record = *p.m.builtin_type_of(r.type);
            // EMIT-181: a whole packed array is stored element by element; it is only ever a member of a struct being
            // stored, so `value` already names one evaluation of it
            if (leaf.count > 0 && r.index.empty() && (leaf.is_split || leaf.is_packed))
            {
                for (auto i = 0; i < leaf.count; ++i)
                {
                    auto element = r;
                    element.index = cc::to_string(i);
                    write_memory(element, -1, cc::format("{}[{}]", value, i), r.type);
                }
                return;
            }
            if (leaf.is_widened)
            {
                line(cc::format("{} = sgl_widen_mat3({});", field_text(r, leaf, 0), value));
                return;
            }
            if (!leaf.is_split)
            {
                auto const place = field_text(r, leaf, 0);
                line(component < 0
                         ? cc::format("{} = {};", place, value)
                         : cc::format("{}.{} = {};", place, p.m.at(p.m.at(r.type).members)[component].name, value));
                return;
            }
            if (component >= 0)
            {
                line(cc::format("{} = {};", field_text(r, leaf, component), value));
                return;
            }
            auto const name = p.names.mint("stored");
            auto declaration = cc::string();
            d.write_local(declaration, {.name = name, .type = type_text(p, d, value_type), .value = value});
            line(declaration);
            auto const n = matrix_size_of(record);
            for (auto i = 0; i < record.leaf_count; ++i)
            {
                // field i is row i % n of column i / n, and only HLSL indexes a matrix by its row first
                auto const is_hlsl = d.language() == builtins::language::hlsl;
                auto const piece = is_matrix_record(record)
                                     ? cc::format("{}[{}][{}]", name, is_hlsl ? i % n : i / n, is_hlsl ? i / n : i % n)
                                     : cc::format("{}.{}", name, p.m.at(p.m.at(r.type).members)[i].name);
                line(cc::format("{} = {};", field_text(r, leaf, i), piece));
            }
            return;
        }

        // A struct is stored leaf by leaf from one evaluation of the value.
        auto const name = p.names.mint("stored");
        auto declaration = cc::string();
        d.write_local(declaration, {.name = name, .type = type_text(p, d, value_type), .value = value});
        line(declaration);
        for (auto const& leaf : r.form->leaves)
        {
            if (!starts_with(leaf.path, r.path))
                continue;
            auto inner = r;
            inner.path = leaf.path;
            inner.type = leaf.type;
            auto const below = cc::span<i32 const>(leaf.path).subspan(
                {.offset = r.path.size(), .size = leaf.path.size() - r.path.size()});
            write_memory(inner, -1, member_path_text(name, r.type, below), leaf.type);
        }
    }

    /// The letters of a swizzle of a vector of `type`, `zyx`.
    cc::string letters_text(type_id type, swizzle const& letters) const
    {
        auto const fields = p.m.at(p.m.at(type).members);
        auto text = cc::string();
        for (auto i = 0; i < letters.count; ++i)
            text += fields[letters.fields[i]].name;
        return text;
    }

    /// `v.zy = value;` where the target assigns through a swizzle, and one component at a time from a local that holds
    /// the value where it does not, or where the vector is in a memory form (EMIT-143).
    void assign_swizzle(flat_member const& place, flat_expr_id stored)
    {
        auto const value = expr(stored, true).text;
        auto const object_type = p.e.at(place.object).type;
        auto const memory = memory_of(place.object);
        auto const object = wrapped(expr(place.object), level::primary);
        if (!memory.has_value() && d.assigns_through_swizzles())
            return line(cc::format("{}.{} = {};", object, letters_text(object_type, place.letters), value));

        auto const value_type = p.e.at(stored).type;
        auto const name = p.names.mint("swizzled");
        auto declaration = cc::string();
        d.write_local(declaration, {.name = name, .type = type_text(p, d, value_type), .value = value});
        line(declaration);
        auto const fields = p.m.at(p.m.at(object_type).members);
        auto const components = p.m.at(p.m.at(value_type).members);
        for (auto i = 0; i < place.letters.count; ++i)
        {
            auto const field = place.letters.fields[i];
            auto const component = cc::format("{}.{}", name, components[i].name);
            if (memory.has_value())
                write_memory(memory.value(), field, component, fields[field].type);
            else
                line(cc::format("{}.{} = {};", object, fields[field].name, component));
        }
    }

    /// `place = value;`, through a memory form where the place is in one.
    void assign(flat_assign const& a)
    {
        if (auto const* const member = p.e.at(a.place).node.try_as<flat_member>();
            member != nullptr && member->is_swizzle())
            return assign_swizzle(*member, a.value);
        // EMIT-174: a write into a member held as pieces goes through a local it is gathered in, and back
        if (auto const gathered = gathered_by(a.place); is_valid(gathered) && gathered != gathered_member)
        {
            auto const raw = expr(gathered).text;
            auto const type = p.e.at(gathered).type;
            gathered_name = rebuilt_from_pieces(type, raw, true);
            gathered_member = gathered;
            assign(a);
            gathered_member = flat_expr_id::none;
            return store_pieces(raw, type, gathered_name);
        }
        auto const held = p.accesses[index_of(a.place)].held;
        if (held == held_as::pieces)
        {
            auto const value = expr(a.value, true).text;
            return store_pieces(expr(a.place).text, p.e.at(a.place).type, value);
        }
        if (held == held_as::widened_array)
        {
            auto const value = expr(a.value, true).text;
            return widen_array_into(expr(a.place).text, p.e.at(a.place).type, value);
        }
        auto value = expr(a.value, true).text;
        if (held == held_as::widened_mat3)
            value = cc::format("sgl_widen_mat3({})", value);
        auto target = memory_of(a.place);
        auto component = -1;
        if (!target.has_value())
            if (auto const* const member = p.e.at(a.place).node.try_as<flat_member>();
                member != nullptr && is_builtin_type(p.m, p.e.at(member->object).type))
            {
                target = memory_of(member->object);
                component = member->member;
            }
        if (target.has_value())
            return write_memory(target.value(), component, value, p.e.at(a.value).type);
        line(cc::format("{} = {};", expr(a.place).text, value));
    }

    /// A body in a target's own text, written ahead of the statement that holds the call, each placeholder the argument
    /// it names, and `$result` a local the text assigns, which is the call's value (CHK-427).
    rendered target_text_call(flat_call const& c, check::type_id type)
    {
        auto const& t = p.m.target_texts[c.text];
        auto arguments = cc::vector<cc::string>();
        for (auto const id : p.e.at(c.arguments))
            arguments.push_back(wrapped(expr(id), level::primary));
        auto result = cc::string();
        if (type != check::checked_module::void_type)
        {
            result = p.names.mint(cc::format("{}_text", p.m.at(c.callee).name));
            auto const type_name = cc::string(type_text(p, d, type));
            auto declaration = cc::string();
            d.write_local(declaration, {.name = result, .type = type_name, .is_mut = true, .is_zeroed = is_padded(type)});
            line(declaration);
        }
        auto text = cc::string(t.pieces[0]);
        for (auto i = isize(0); i < t.slots.size(); ++i)
        {
            text += t.slots[i] < 0 ? cc::string_view(result) : cc::string_view(arguments[t.slots[i]]);
            text += t.pieces[i + 1];
        }
        // each line of the text a line of its own, indented where the call stands
        auto start = isize(0);
        for (auto i = isize(0); i <= text.size(); ++i)
            if (i == text.size() || text[i] == '\n')
            {
                if (i > start)
                    line(cc::string(cc::string_view(text).subview({.offset = start, .size = i - start})));
                start = i + 1;
            }
        return {.text = cc::move(result)};
    }

    /// EMIT-139: the per-instance hit-group offsets of the acceleration structure `id` names, which sg's metal backend
    /// mirrors slot for slot beside each group's argument buffer: `hit_offsets->groups[group][slot + element]`.
    /// Empty where `id` is no acceleration-structure member, or an element of one.
    cc::string hit_offsets_of(flat_expr_id id)
    {
        if (p.m.at(p.e.at(id).type).kind != type_kind::acceleration_structure)
            return {};
        auto named = id;
        auto element = cc::string();
        if (auto const* const e = p.e.at(id).node.try_as<flat_element>())
        {
            named = e->object;
            element = cc::format(" + {}", wrapped(expr(e->index), level::additive));
        }
        auto const* const member = p.e.at(named).node.try_as<flat_binding_member>();
        auto const found = member != nullptr ? resource_of(p, member->binding, member->member) : -1;
        if (found < 0)
            return {};
        auto const& r = p.resources[found];
        return cc::format("sgl_ctx.hit_offsets->groups[{}][{}{}].instances", r.group, r.slot, element);
    }

    /// Every call is written from its registry record; nothing here knows one builtin from another.
    rendered call(flat_call const& c, check::type_id type)
    {
        if (c.text >= 0)
            return target_text_call(c, type);
        auto const& record = *p.m.builtin_function(c.intrinsic);
        auto arguments = cc::vector<rendered>();
        for (auto const id : p.e.at(c.arguments))
            arguments.push_back(expr(id));

        auto const& how = record.write;
        switch (how.kind)
        {
        case builtins::spelling_kind::infix:
            return builtins::write_infix(how.text, how.binds, cc::move(arguments[0]), cc::move(arguments[1]));
        case builtins::spelling_kind::prefix:
            // parenthesized whenever it is no name: `--x` is a decrement in every C-like target
            return {.text = cc::format("{}{}", how.text, wrapped(cc::move(arguments[0]), level::primary)),
                    .binds = level::unary};
        case builtins::spelling_kind::custom:
        {
            auto mint = [&](cc::string_view desired) { return p.names.mint(desired); };
            auto const offsets = d.language() == builtins::language::msl && !arguments.empty()
                                   ? hit_offsets_of(p.e.at(c.arguments)[0])
                                   : cc::string();
            auto result = how.custom({.target = d.language(),
                                      .arguments = arguments,
                                      .builtins = *p.m.builtins,
                                      .data = how.data,
                                      .mint = mint,
                                      .result_type = p.m.builtin_type_of(type) != nullptr || record.gives_prelude_struct
                                                       ? type_text(p, d, type)
                                                       : cc::string_view(),
                                      .hit_offsets = offsets,
                                      .is_device_coherent = is_device_coherent,
                                      .is_pixel_stage = p.e.entry_stage == check::stage::pixel,
                                      .native_float_atomics = p.native_float_atomics});
            // the statements its value needs, ahead of the one that holds it
            for (auto const& l : result.lines)
                line(l);
            result.lines.clear();
            return result;
        }
        case builtins::spelling_kind::call:
            break;
        }

        auto text = cc::string(record.called_in(d.language()));
        text += "(";
        for (auto i = isize(0); i < arguments.size(); ++i)
        {
            if (i != 0)
                text += ", ";
            text += arguments[i].text;
        }
        text += ")";
        return {.text = cc::move(text)};
    }

    rendered expr(flat_expr_id id, bool is_broken = false)
    {
        if (id == gathered_member)
            return {.text = gathered_name};
        auto const& x = p.e.at(id);
        if (auto const memory = memory_of(id); memory.has_value())
            return {.text = read_memory(memory.value())};
        // A component of a vector in a memory form is read from the field that holds it, not from the rebuilt vector.
        if (auto const* const member = x.node.try_as<flat_member>(); member != nullptr && !member->is_swizzle())
            if (auto const* const record = p.m.builtin_type_of(p.e.at(member->object).type);
                record != nullptr && record->leaf_count <= 4)
                if (auto const object = memory_of(member->object); object.has_value())
                {
                    auto const& leaf = leaf_at(object.value());
                    auto const component = p.m.at(p.m.at(p.e.at(member->object).type).members)[member->member].name;
                    if (leaf.is_split)
                        return {.text = field_text(object.value(), leaf, member->member)};
                    return {.text = cc::format("{}.{}", field_text(object.value(), leaf, 0), component)};
                }
        auto result = rendered();
        // EMIT-140: a literal of a 16-bit type is a construction of its type, since a bare one is 32 bits in MSL
        auto const* const literal_type = p.m.builtin_type_of(x.type);
        auto const narrow = [&](rendered r)
        {
            if (literal_type == nullptr || !is_16_bit(literal_type->leaf_kind))
                return r;
            return rendered{.text = cc::format("{}({})", literal_type->spelled_in(d.language()), r.text)};
        };
        x.node.visit(
            [&](flat_invalid const&) {}, [&](flat_literal const& l)
            { result = narrow({.text = literal_text(l.value), .binds = l.value < 0 ? level::unary : level::primary}); },
            [&](flat_int_literal const& l)
            {
                auto const is_wrapped = l.value == -2147483647 - 1;
                auto text = l.is_hex && (l.is_unsigned || l.value >= 0)
                              ? cc::format("{:#x}{}", u32(l.value), l.is_unsigned ? "u" : "")
                          : l.is_unsigned ? cc::to_string(u32(l.value)) + "u"
                                          : int_literal_text(l.value);
                result = narrow(
                    {.text = cc::move(text), .binds = l.value < 0 && !is_wrapped ? level::unary : level::primary});
            },
            [&](flat_bool_literal const& l) { result = {.text = l.value ? "true" : "false"}; },
            [&](flat_enum_value const& v)
            { result = {.text = p.enums[p.enum_of_type[index_of(x.type)]].case_names[v.case_index]}; },
            [&](flat_local_ref const& l) { result = {.text = p.locals[index_of(l.local)]}; },
            [&](flat_binding_member const& b)
            {
                // A buffer is a global of its own; every other member is a field of a block, `@inline` or its group's.
                if (auto const found = resource_of(p, b.binding, b.member); found >= 0)
                {
                    result = {.text = p.resources[found].name};
                    return;
                }
                if (auto const found = workgroup_of(p, b.binding, b.member); found >= 0)
                {
                    result = {.text = p.workgroup[found].name};
                    return;
                }
                auto const& block = *block_of(p, b.binding);
                result = {.text = cc::format("{}.{}", block.name, block.members[block.block_member_of[b.member]].name)};
                result = read_as_value(id, result);
            },
            [&](flat_file_sampler const& smp) { result = {.text = p.samplers[sampler_of(p, smp.sampler)].name}; },
            [&](flat_buffer_element const& b)
            {
                auto const buffer = wrapped(expr(b.buffer), level::primary);
                result = {.text = cc::format("{}[{}]", buffer, expr(b.index).text), .binds = level::primary};
            },
            [&](flat_element const& a)
            {
                auto const object = wrapped(expr(a.object), level::primary);
                // EMIT-174: a literal index into an array held as pieces names its piece
                if (id != gathered_member && is_valid(a.object) && a.object != gathered_member && is_piece(p, a))
                {
                    result = {.text = cc::format("{}_{}", object, p.e.at(a.index).node.as<flat_int_literal>().value)};
                    return;
                }
                // EMIT-169 and EMIT-172: an element of an array held as 4-vectors is read and written through its lanes
                if (auto const& held = p.accesses[index_of(a.object)]; held.held == held_as::widened_array)
                {
                    // an inner array is still held widened, and only its own elements read through the lanes
                    auto const lanes = is_array(x.type) ? cc::string_view() : held.lanes;
                    result = read_as_value(id, {.text = cc::format("{}[{}]{}", object, expr(a.index).text, lanes),
                                                .binds = level::primary});
                    return;
                }
                auto index = expr(a.index).text;
                // EMIT-121: a marked index into a binding array tells HLSL, and SPIR-V through it, that it may differ
                if (d.language() == builtins::language::hlsl && is_nonuniform_mark(a.index)
                    && p.m.takes_slots(p.e.at(a.object).type))
                    index = cc::format("NonUniformResourceIndex({})", index);
                result = {.text = cc::format("{}[{}]", object, index), .binds = level::primary};
            },
            [&](flat_member const& member)
            {
                auto const object_type = p.e.at(member.object).type;
                auto object = wrapped(expr(member.object), level::primary);
                // EMIT-142: a prelude vector's swizzle is the target's own, spelled alike everywhere
                if (member.is_swizzle())
                {
                    result = {.text = cc::format("{}.{}", object, letters_text(object_type, member.letters))};
                    return;
                }
                // A builtin's fields are spelled alike everywhere: x, y, z, w.
                if (is_builtin_type(p.m, object_type))
                {
                    result
                        = {.text = cc::format("{}.{}", object, p.m.at(p.m.at(object_type).members)[member.member].name)};
                    return;
                }
                auto const& planned = p.structs[p.struct_of_type[index_of(object_type)]];
                result = read_as_value(
                    id, {.text = cc::format("{}.{}", object, planned.members[planned.member_of[member.member]].name)});
            },
            [&](flat_construct const&) { result = construct(x, is_broken); },
            [&](flat_call const& c) { result = call(c, x.type); }, [&](flat_not const& n)
            { result = {.text = cc::format("!{}", wrapped(expr(n.operand), level::primary)), .binds = level::unary}; },
            [&](flat_and const& a) { result = logical("&&", level::logical_and, expr(a.lhs), expr(a.rhs)); },
            [&](flat_or const& o) { result = logical("||", level::logical_or, expr(o.lhs), expr(o.rhs)); },
            // never in a core tree, which is all that reaches a writer
            [&](flat_block const&) {}, [&](flat_by_target const&) {});
        return result;
    }

    void body(ast::range_of<flat_stmt_id> range)
    {
        for (auto const id : p.e.at(range))
            statement(p.e.at(id));
    }

    /// `is_chained` writes `else if` onto the `if` before it.
    void branch(flat_if const& s, bool is_chained)
    {
        auto const head = condition_text(is_chained ? "else if" : "if", expr(s.condition).text);
        if (is_chained)
            reopen(head);
        else
            open(head);
        body(s.then_body);

        auto const else_body = p.e.at(s.else_body);
        if (else_body.empty())
            return close();
        // `else if` reads better than an `if` nested in an `else`, as long as the condition writes no line of its own
        auto const* const nested = else_body.size() == 1 ? p.e.at(else_body[0]).node.try_as<flat_if>() : nullptr;
        if (nested != nullptr && !writes_lines(nested->condition))
            return branch(*nested, true);
        reopen("else");
        body(s.else_body);
        close();
    }

    /// CHK-414: the target's hint ahead of a loop written `@unroll(false)`.
    void hint_rolled(check::label_id label)
    {
        if (check::is_valid(label) && p.e.labels[index_of(label)].is_rolled && !d.rolled_loop_hint().empty())
            line(d.rolled_loop_hint());
    }

    void loop_while(flat_while const& s)
    {
        hint_rolled(s.label);
        if (!writes_lines(s.condition))
        {
            open(condition_text("while", expr(s.condition).text));
            body(s.body);
            return close();
        }
        // The lines the condition writes must run before every test, so the test moves into the loop.
        open(d.is_c_like() ? "while (true)" : "loop");
        auto const condition = cc::format("!{}", wrapped(expr(s.condition), level::primary));
        open(condition_text("if", condition));
        line("break;");
        close();
        body(s.body);
        close();
    }

    void once(flat_once const& s)
    {
        if (d.is_c_like())
        {
            open("do");
            body(s.body);
            return close(" while (false);");
        }
        // WGSL has no `do`: a loop that ends in a `break` runs once, and the `break` is left out where no path reaches it
        open("loop");
        body(s.body);
        auto const statements = p.e.at(s.body);
        auto const ends_in_exit
            = !statements.empty()
           && (p.e.at(statements.back()).node.is<flat_break>() || p.e.at(statements.back()).node.is<flat_return>()
               || p.e.at(statements.back()).node.is<flat_discard>());
        if (!ends_in_exit)
            line("break;");
        close();
    }

    void declare(local_id id, flat_expr_id value, bool is_mut)
    {
        auto const& local = p.e.at(id);
        auto const name = cc::string_view(p.locals[index_of(id)]);
        if (is_valid(value) && needs_member_assignment(p.e.at(value)))
        {
            build_struct(p.e.at(value), name);
            return;
        }
        // a value only a local may hold, such as a ray query: declared without an initializer, and never `const`
        auto const* const call = is_valid(value) ? p.e.at(value).node.try_as<flat_call>() : nullptr;
        auto const* const record = call != nullptr ? p.m.builtin_function(call->intrinsic) : nullptr;
        auto const declares_only = record != nullptr && record->declares_only;
        auto const text = is_valid(value) && !declares_only ? expr(value, true).text : cc::string();
        auto declaration = cc::string();
        auto const dimensions = array_dimensions(p, local.type);
        auto const* const type_record = p.m.builtin_type_of(local.type);
        auto const& flags = p.local_flags[index_of(id)];
        // EMIT-184: forcing micromaps to 2 states is a name of Shader Model 6.9 alone, and the template only a hint
        auto const sees_micromaps
            = p.e.features.has(check::feature::opacity_micromap) && d.flags_query_for_micromaps(flags);
        auto type = declares_only && d.language() == builtins::language::hlsl && type_record != nullptr
                         && type_record->hlsl_flagged != nullptr && flags.has_value()
                      ? type_record->hlsl_flagged(sees_micromaps ? flags.value() : flags.value() & ~1024u)
                      : cc::string(type_text(p, d, local.type));
        // EMIT-184: a query of an entry point that states opacity micromaps sees them, which HLSL asks of each query
        if (declares_only && type_record != nullptr && type_record->hlsl_flagged != nullptr
            && d.language() == builtins::language::hlsl && sees_micromaps && type.ends_with(">"))
            type = cc::format("{}, RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS>",
                              type.subview({.offset = 0, .size = type.size() - 1}));
        d.write_local(declaration, {.name = name,
                                    .type = type,
                                    .value = text,
                                    .dimensions = dimensions,
                                    .is_mut = is_mut || declares_only,
                                    .is_zeroed = text.empty() && is_padded(local.type)});
        line(declaration);
    }

    /// True when the arm's last statement already leaves the switch, so the C-like targets need no `break` of their own.
    [[nodiscard]] bool ends_in_exit(ast::range_of<flat_stmt_id> range) const
    {
        auto const statements = p.e.at(range);
        if (statements.empty())
            return false;
        auto const& last = p.e.at(statements.back());
        return last.node.is<flat_break>() || last.node.is<flat_return>() || last.node.is<flat_continue>()
            || last.node.is<flat_discard>();
    }

    /// One arm: its labels, its body, and the `break` that stops the C-like targets falling into the next one.
    void arm(cc::span<flat_expr_id const> patterns, ast::range_of<flat_stmt_id> range)
    {
        if (d.is_c_like())
        {
            for (auto const pattern : patterns)
                line(cc::format("case {}:", expr(pattern).text));
            if (patterns.empty())
                line("default:");
            ++depth;
            body(range);
            if (!ends_in_exit(range))
                line("break;");
            --depth;
            return;
        }
        auto labels = cc::string();
        for (auto const pattern : patterns)
        {
            if (!labels.empty())
                labels += ", ";
            labels += expr(pattern).text;
        }
        open(patterns.empty() ? cc::string("default:") : cc::format("case {}:", labels));
        body(range);
        close();
    }

    void switch_(flat_switch const& s)
    {
        open(condition_text("switch", expr(s.scrutinee).text));
        for (auto const& a : p.e.at(s.arms))
            arm(p.e.at(a.patterns), a.body);
        arm({}, s.default_body);
        close();
    }

    void statement(flat_stmt const& s)
    {
        auto const* const outer = current;
        current = &s;
        unwritten = &s;
        statement_node(s);
        current = outer;
    }

    void statement_node(flat_stmt const& s)
    {
        s.node.visit(
            [&](flat_let const& let) { declare(let.local, let.value, p.e.at(let.local).is_mut); },
            [&](flat_var const& var) { declare(var.local, var.value, true); }, [&](flat_assign const& a) { assign(a); },
            // `validate` refuses a tree that holds one
            [&](flat_print const& n) { print_record(s, n); }, //
            [&](flat_eval const& v)
            {
                // an atomic HLSL writes as statements alone has nothing left to evaluate, nor has a target's own text
                auto const* const evaluated = p.e.at(v.value).node.try_as<flat_call>();
                if (is_hlsl_atomic(v.value) || (evaluated != nullptr && evaluated->text >= 0))
                {
                    (void)expr(v.value);
                    return;
                }
                // A call that gives nothing is a statement as it stands, in every target.
                auto text = cc::string();
                if (p.e.at(v.value).type == checked_module::void_type)
                    text = cc::format("{};", expr(v.value).text);
                else
                    d.write_eval(text, expr(v.value).text);
                line(text);
                fence_after(v.value);
            },
            [&](flat_if const& i) { branch(i, false); },
            // none of the three is in a core tree
            [&](flat_block const&) {}, //
            [&](flat_leave const&) {}, //
            [&](flat_case const&) {},
            [&](flat_loop const& l)
            {
                hint_rolled(l.label);
                open(d.is_c_like() ? "while (true)" : "loop");
                body(l.body);
                close();
            },
            [&](flat_while const& w) { loop_while(w); },
            [&](flat_for const& f)
            {
                auto const first = expr(f.first).text;
                auto const end = wrapped(expr(f.end), level::additive);
                auto head = cc::string();
                d.write_for_head(head, p.locals[index_of(f.index)], first, end);
                hint_rolled(f.label);
                open(head);
                body(f.body);
                close();
            },
            [&](flat_continue const&) { line("continue;"); },                                                //
            [&](flat_discard const&) { line(d.discard_statement()); }, [&](flat_once const& o) { once(o); }, //
            [&](flat_break const&) { line("break;"); },                                                      //
            [&](flat_switch const& sw) { switch_(sw); },
            // a core tree holds none, and the plan refuses one that is not core (EMIT-66)
            [&](flat_check const&) {},
            [&](flat_return const& r)
            {
                // A void result was erased by LEGAL-52, and a compute entry point has none either.
                if (!is_valid(r.value))
                {
                    line("return;");
                    return;
                }
                // EMIT-139: metal's any hit is a traversal function that answers whether it accepts, and writes the
                // payload back into the ray data it copied it from
                auto const is_msl = d.language() == builtins::language::msl;
                auto const write_back = [&]
                {
                    if (is_msl && check::is_valid(p.e.input))
                        line(cc::format("*reinterpret_cast<ray_data {}*>(sgl_data.payload) = {};",
                                        type_text(p, d, p.e.input), p.locals[0]));
                };
                if (is_msl && p.e.entry_stage == check::stage::any_hit)
                {
                    auto const decision = p.names.mint("decision");
                    line(cc::format("const int {} = {};", decision, expr(r.value, true).text));
                    write_back();
                    line(cc::format("return {{{0} != 1, {0} != 2}};", decision));
                    return;
                }
                if (is_msl && p.e.entry_stage == check::stage::intersection)
                {
                    auto const type = p.e.at(r.value).type;
                    auto const reported = p.names.mint("reported");
                    line(cc::format("const {} {} = {};", type_text(p, d, type), reported, expr(r.value, true).text));
                    write_back();
                    line(cc::format("if ({}.{})", reported, member_of(type, "is_hit")));
                    line("{");
                    line(cc::format("    *reinterpret_cast<ray_data {}*>(sgl_data.attributes) = {}.{};",
                                    type_text(p, d, p.m.at(type).element), reported, member_of(type, "attributes")));
                    auto const continues = check::is_valid(p.e.ends_search)
                                             ? cc::format("!{}", p.locals[index_of(p.e.ends_search)])
                                             : cc::string("true");
                    line(cc::format("    return {{true, {}.{}, {}}};", reported, member_of(type, "t"), continues));
                    line("}");
                    line("return {false, 0.0f, true};");
                    return;
                }
                // EMIT-136: an any hit's decision is the target's call that ends the stage, and a plain return
                // accepts the candidate
                if (p.e.entry_stage == check::stage::any_hit)
                {
                    auto const decision = p.names.mint("decision");
                    line(cc::format("const int {} = {};", decision, expr(r.value, true).text));
                    line(cc::format("if ({} == 1)", decision));
                    line("    IgnoreHit();");
                    line(cc::format("if ({} == 2)", decision));
                    line("    AcceptHitAndEndSearch();");
                    line("return;");
                    return;
                }
                // EMIT-137: an intersection's report is the target's call, made where it hits
                if (p.e.entry_stage == check::stage::intersection)
                {
                    auto const type = p.e.at(r.value).type;
                    auto const reported = p.names.mint("reported");
                    line(cc::format("const {} {} = {};", type_text(p, d, type), reported, expr(r.value, true).text));
                    line(cc::format("if ({}.{})", reported, member_of(type, "is_hit")));
                    line(cc::format("    ReportHit({0}.{1}, 0, {0}.{2});", reported, member_of(type, "t"),
                                    member_of(type, "attributes")));
                    line("return;");
                    return;
                }
                auto const& value = p.e.at(r.value);
                if (needs_member_assignment(value))
                {
                    auto const name = p.names.mint("result");
                    build_struct(value, name);
                    line(cc::format("return {};", name));
                    return;
                }
                line(cc::format("return {};", expr(r.value, true).text));
            });
    }
};
} // namespace

void sgl::emit::impl::write_enum_constants(cc::string& out, plan const& p, dialect const& d)
{
    for (auto const& e : p.enums)
    {
        auto const cases = p.m.at(p.m.at(e.type).cases);
        for (auto i = isize(0); i < cases.size(); ++i)
            d.write_enum_constant(out, e.case_names[i], cases[i].value);
        if (!cases.empty())
            out += "\n";
    }
}

void sgl::emit::impl::write_buffers(cc::string& out, plan const& p, dialect const& d)
{
    // `p.resources` is in group then slot order, so one binding's resources are one run.
    auto first = isize(0);
    for (auto const id : p.e.bindings)
    {
        auto last = first;
        while (last < p.resources.size() && p.resources[last].binding == id)
            ++last;
        auto const* const block = block_of(p, id);
        auto const is_group_block = block != nullptr && block->group >= 0;
        if (is_group_block || last > first)
            d.write_group(out, p, is_group_block ? block : nullptr,
                          cc::span<planned_resource const>(p.resources).subspan({.offset = first, .size = last - first}));
        first = last;
    }
    for (auto const& s : p.samplers)
        d.write_file_sampler(out, p, s);
    if (!p.samplers.empty())
        out += "\n";
}

cc::string_view sgl::emit::impl::type_text(plan const& p, dialect const& d, check::type_id type)
{
    // what a pixel stage that writes images alone returns, which HLSL and MSL spell as C does (CHK-418)
    if (type == check::checked_module::void_type)
        return "void";
    if (auto const* const record = p.m.builtin_type_of(type))
        return record->spelled_in(d.language());
    // An enum is its cases' `int` on every target (EVAL-64); the constants of EMIT-76 are what its values read as.
    if (is_valid(type) && p.m.at(type).kind == check::type_kind::enumeration && p.m.builtins != nullptr)
    {
        auto const id = p.m.builtins->find_type(builtins::k_int);
        if (p.m.builtins->is_known(id))
            return p.m.builtins->at(id).spelled_in(d.language());
    }
    if (is_valid(type) && p.m.at(type).kind == check::type_kind::array)
        return p.array_texts[p.array_of_type[index_of(type)]];
    if (is_valid(type) && p.m.at(type).kind == check::type_kind::atomic)
        return atomic_text(p, type);
    // CHK-432: a text type is spelled as its compile's branch says
    for (auto const& t : p.e.text_types)
        if (t.type == type)
            return t.spelling;
    return p.structs[p.struct_of_type[index_of(type)]].name;
}


void sgl::emit::impl::write_helpers(cc::string& out, plan const& p, dialect const& d)
{
    // CHK-428: the preambles of each file whose bodies in a target's text the entry point writes, once, in the order
    // first needed
    auto const language = d.language() == builtins::language::hlsl ? check::shading_language::hlsl
                        : d.language() == builtins::language::wgsl ? check::shading_language::wgsl
                                                                   : check::shading_language::msl;
    auto text_files = cc::vector<i32>();
    for (auto const& x : p.e.exprs)
        if (auto const* const call = x.node.try_as<check::flat_call>(); call != nullptr && call->text >= 0)
            if (auto const file = p.m.target_texts[call->text].file; !text_files.values().contains(file))
                text_files.push_back(file);
    for (auto const file : text_files)
        for (auto const& preamble : p.m.target_preambles)
            if (preamble.file == file && preamble.language == language)
                out.appendf("{}\n", preamble.text);

    // EMIT-102: each helper the entry point's builtin calls need, once, in the order first needed.
    auto written = cc::vector<cc::string>();
    for (auto const& x : p.e.exprs)
    {
        auto const* const call = x.node.try_as<check::flat_call>();
        auto const* const record = call != nullptr ? p.m.builtin_function(call->intrinsic) : nullptr;
        if (record == nullptr || record->write.helper == nullptr)
            continue;
        auto types = cc::vector<cc::string>();
        for (auto const argument : p.e.at(call->arguments))
        {
            auto const type = p.e.at(argument).type;
            types.push_back(check::is_resource(p.m.at(type).kind) ? d.resource_text(p, type)
                                                                  : cc::string(type_text(p, d, type)));
        }
        auto text = record->write.helper({.target = d.language(), .argument_types = types, .data = record->write.data});
        auto is_known = text.empty();
        for (auto const& w : written)
            is_known = is_known || w == text;
        if (!is_known)
            written.push_back(cc::move(text));
    }
    for (auto const& w : written)
        out.appendf("{}\n", w);
}

sgl::emit::impl::written_text sgl::emit::impl::write_text(plan& p, dialect const& d, options const& sources)
{
    auto w = writer{.p = p, .d = d, .sources = sources};
    // EMIT-183: a stage that cannot write the buffer drops its prints, and declares no buffer
    if (sources.diagnostics && d.writes_debug_in(p.e.entry_stage))
        for (auto const& s : p.e.stmts)
            p.writes_debug = p.writes_debug || s.node.is<flat_print>();
    for (auto const& r : p.resources)
        w.is_device_coherent = w.is_device_coherent || r.is_coherent;
    if (d.fences_image_stores())
        for (auto const& x : p.e.exprs)
            if (auto const* const b = w.image_loaded_by(x))
                w.fenced_images.push_back(*b);
    w.out.appendf("// SGL {} entry point '{}', written as {}.\n", check::stage_name(p.e.entry_stage), p.entry_name,
                  d.description());
    w.out += "// Generated: the SGL source is what to edit.\n\n";
    d.write_declarations(w.out, p);
    write_helpers(w.out, p, d);
    if (p.writes_debug)
        d.write_debug_buffer(w.out);
    d.write_function_head(w.out, p);
    if (d.declares_workgroup_in_function())
        for (auto const& memory : p.workgroup)
        {
            auto declaration = cc::string();
            d.write_workgroup(declaration, memory, p);
            w.line(declaration);
        }
    for (auto const id : p.e.at(p.e.body))
        w.statement(p.e.at(id));
    w.out += "}\n";
    d.write_function_tail(w.out, p);
    return {.text = cc::move(w.out), .lines = cc::move(w.lines), .debug_sites = cc::move(w.debug_sites)};
}
