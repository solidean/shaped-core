#include "wgsl_declarations.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/map.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>
#include <shaped-graphics/binding/shader_stage.hh>
#include <shaped-graphics/fwd.hh> // sg::reserved_binding_group

using namespace cc::primitive_defines;

namespace
{
enum class token_kind
{
    identifier,
    number,
    punct,
    end,
};

struct token
{
    token_kind kind = token_kind::end;
    cc::string_view text;
    int line = 1;
};

/// Splits WGSL into identifiers, numbers and single-character punctuation, dropping whitespace and both comment kinds.
/// `->` is the only two-character token the declaration grammar needs, so everything else stays one character.
cc::result<cc::vector<token>> tokenize(cc::string_view src)
{
    auto tokens = cc::vector<token>();
    auto line = 1;
    auto i = isize(0);
    auto const n = src.size();
    while (i < n)
    {
        auto const c = src[i];
        if (c == '\n')
        {
            ++line;
            ++i;
            continue;
        }
        if (cc::is_space(c))
        {
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '/')
        {
            while (i < n && src[i] != '\n')
                ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*')
        {
            // WGSL block comments nest.
            auto const start_line = line;
            auto depth = 0;
            do
            {
                if (src[i] == '\n')
                    ++line;
                if (src[i] == '/' && i + 1 < n && src[i + 1] == '*')
                {
                    ++depth;
                    i += 2;
                    continue;
                }
                if (src[i] == '*' && i + 1 < n && src[i + 1] == '/')
                {
                    --depth;
                    i += 2;
                    continue;
                }
                ++i;
            } while (depth > 0 && i < n);
            if (depth > 0)
                return cc::error(cc::format("line {}: unterminated block comment", start_line));
            continue;
        }
        if ((cc::is_upper(c) || cc::is_lower(c)) || c == '_')
        {
            auto const start = i;
            while (i < n && (cc::is_alphanumeric(src[i]) || src[i] == '_'))
                ++i;
            tokens.push_back(
                {.kind = token_kind::identifier, .text = src.subview({.offset = start, .size = i - start}), .line = line});
            continue;
        }
        if (cc::is_digit(c))
        {
            auto const start = i;
            while (i < n && (cc::is_alphanumeric(src[i]) || src[i] == '.'))
                ++i;
            tokens.push_back(
                {.kind = token_kind::number, .text = src.subview({.offset = start, .size = i - start}), .line = line});
            continue;
        }
        if (c == '-' && i + 1 < n && src[i + 1] == '>')
        {
            tokens.push_back({.kind = token_kind::punct, .text = src.subview({.offset = i, .size = 2}), .line = line});
            i += 2;
            continue;
        }
        tokens.push_back({.kind = token_kind::punct, .text = src.subview({.offset = i, .size = 1}), .line = line});
        ++i;
    }
    tokens.push_back({.kind = token_kind::end, .text = {}, .line = line});
    return tokens;
}

/// A host-shareable type's layout under WGSL's own rules; `size` is absent for a runtime-sized array.
struct type_layout
{
    isize align = 1;
    cc::optional<isize> size;
};

/// What a type expression names, once parsed: its layout where it has one, and the pieces a binding needs.
struct parsed_type
{
    cc::string name; // the outermost name, e.g. "texture_2d", "array", "vec4", a struct's name
    cc::vector<parsed_type> args;
    cc::vector<token> count; // an array's element count, left unevaluated until every const is known
    cc::string_view access;  // a storage texture's or pointer's access argument
};

class parser
{
public:
    explicit parser(cc::vector<token> tokens) : _tokens(cc::move(tokens)) {}

    cc::result<slib::wgsl_declarations> parse()
    {
        // Module-scope declarations may come in any order, so this loop only collects them.
        // Every integer argument — a count, an address, a workgroup size — is evaluated after it, once every const is known.
        auto entries = cc::vector<pending_entry>();
        auto bindings = cc::vector<pending_binding>();

        while (peek().kind != token_kind::end)
        {
            auto attrs = cc::vector<attribute>();
            CC_RETURN_IF_ERROR(read_attributes(attrs));

            auto const& t = peek();
            if (t.kind == token_kind::end)
                break;

            if (is(t, "enable") || is(t, "requires") || is(t, "diagnostic") || is(t, "const_assert"))
            {
                skip_statement();
                continue;
            }
            if (is(t, "alias"))
            {
                CC_RETURN_IF_ERROR(read_alias());
                continue;
            }
            if (is(t, "const"))
            {
                CC_RETURN_IF_ERROR(read_const());
                continue;
            }
            if (is(t, "override"))
            {
                // An override is only a problem where sg would need its value, which integer_of says.
                _overrides.push_back(cc::string(_tokens[_pos + 1].text));
                skip_statement();
                continue;
            }
            if (is(t, "struct"))
            {
                CC_RETURN_IF_ERROR(read_struct());
                continue;
            }
            if (is(t, "var"))
            {
                CC_RETURN_IF_ERROR(read_var(attrs, bindings));
                continue;
            }
            if (is(t, "fn"))
            {
                CC_RETURN_IF_ERROR(read_fn(attrs, entries));
                continue;
            }
            return cc::error(cc::format("line {}: unexpected '{}' at module scope", t.line, t.text));
        }

        if (entries.empty())
            return cc::error("the module declares no entry point; a WGSL shader for sg declares exactly one @vertex, "
                             "@fragment or @compute function");
        if (entries.size() > 1)
            return cc::error(cc::format("the module declares {} entry points ('{}' and '{}'); sg reads one per module, "
                                        "since a declaration parser cannot tell which entry point uses which binding",
                                        entries.size(), entries[0].declared.entry_point, entries[1].declared.entry_point));

        auto result = cc::move(entries[0].declared);
        if (result.stage == sg::shader_stage::compute)
        {
            auto const& args = entries[0].workgroup_size;
            auto dims = sg::compute_dimensions{};
            for (auto k = isize(0); k < args.size() && k < 3; ++k)
            {
                auto value = integer_of(args[k], entries[0].line);
                CC_RETURN_IF_ERROR(value);
                (k == 0 ? dims.x : k == 1 ? dims.y : dims.z) = int(value.value());
            }
            result.workgroup_size = dims;
        }
        for (auto& b : bindings)
        {
            auto binding = sg::binding{};
            CC_RETURN_IF_ERROR(to_binding(b, binding));
            result.bindings.push_back(cc::move(binding));
        }
        sg::apply_stage_visibility(result.bindings, result.stage);
        return result;
    }

private:
    struct attribute
    {
        cc::string_view name;
        cc::vector<cc::vector<token>> args;
        int line = 1;
    };

    struct pending_binding
    {
        cc::string name;
        cc::vector<token> group; // @group and @binding stay unevaluated until every const is known
        cc::vector<token> index;
        cc::string_view address_space; // "uniform", "storage", or empty for a handle type
        cc::string_view access;        // a storage buffer's access mode
        parsed_type type;
        int line = 1;
    };

    struct struct_member
    {
        parsed_type type;
        cc::vector<token> explicit_size; // empty when absent; unevaluated like every other integer argument
        cc::vector<token> explicit_align;
    };

    /// An entry point as declared, its `@workgroup_size` arguments not yet evaluated.
    struct pending_entry
    {
        slib::wgsl_declarations declared;
        cc::vector<cc::vector<token>> workgroup_size;
        int line = 1;
    };

    cc::vector<token> _tokens;
    isize _pos = 0;
    cc::map<cc::string, cc::vector<struct_member>> _structs;
    cc::map<cc::string, parsed_type> _aliases;
    cc::map<cc::string, i64> _constants;
    cc::vector<cc::string> _overrides;

    token const& peek(isize ahead = 0) const
    {
        auto const i = cc::min(_pos + ahead, _tokens.size() - 1);
        return _tokens[i];
    }

    token const& next()
    {
        auto const& t = peek();
        if (_pos < _tokens.size() - 1)
            ++_pos;
        return t;
    }

    static bool is(token const& t, cc::string_view text) { return t.text == text; }

    cc::result<token> expect(cc::string_view text)
    {
        auto const& t = peek();
        if (!is(t, text))
            return cc::error(cc::format("line {}: expected '{}', found '{}'", t.line, text, t.text));
        return next();
    }

    cc::result<token> expect_identifier()
    {
        auto const& t = peek();
        if (t.kind != token_kind::identifier)
            return cc::error(cc::format("line {}: expected a name, found '{}'", t.line, t.text));
        return next();
    }

    /// Skips to just past the next `;` at bracket depth zero, or past a balanced `{ }` block, whichever ends the statement.
    void skip_statement()
    {
        auto depth = 0;
        while (peek().kind != token_kind::end)
        {
            auto const& t = next();
            if (is(t, "(") || is(t, "[") || is(t, "{"))
                ++depth;
            else if (is(t, ")") || is(t, "]") || is(t, "}"))
            {
                --depth;
                if (depth == 0 && is(t, "}") && !is(peek(), ";"))
                    return;
            }
            else if (depth == 0 && is(t, ";"))
                return;
        }
    }

    /// Reads a run of `@name` / `@name(args)` attributes; arguments are kept as token runs split at top-level commas.
    cc::result<cc::unit> read_attributes(cc::vector<attribute>& out)
    {
        while (is(peek(), "@"))
        {
            next();
            auto name = expect_identifier();
            CC_RETURN_IF_ERROR(name);
            auto attr = attribute{.name = name.value().text, .args = {}, .line = name.value().line};
            if (is(peek(), "("))
            {
                next();
                auto current = cc::vector<token>();
                auto depth = 0;
                while (true)
                {
                    auto const& t = peek();
                    if (t.kind == token_kind::end)
                        return cc::error(cc::format("line {}: unterminated attribute '@{}'", attr.line, attr.name));
                    next();
                    if (depth == 0 && is(t, ")"))
                        break;
                    if (depth == 0 && is(t, ","))
                    {
                        attr.args.push_back(cc::move(current));
                        current = {};
                        continue;
                    }
                    // Only parentheses nest here: a '<' in an argument is as likely a comparison as a template.
                    if (is(t, "("))
                        ++depth;
                    if (is(t, ")"))
                        --depth;
                    current.push_back(t);
                }
                if (!current.empty())
                    attr.args.push_back(cc::move(current));
            }
            out.push_back(cc::move(attr));
        }
        return cc::unit{};
    }

    /// An integer argument: a literal with an optional `i` / `u` suffix, or a module-scope `const`.
    cc::result<i64> integer_of(cc::vector<token> const& expr, int line) const
    {
        if (expr.size() != 1)
            return cc::error(cc::format("line {}: expected an integer, found an expression sg does not evaluate", line));
        auto const& t = expr[0];
        if (t.kind == token_kind::identifier)
        {
            if (auto const* value = _constants.get_ptr(cc::string(t.text)))
                return *value;
            for (auto const& o : _overrides)
                if (o == t.text)
                    return cc::error(cc::format("line {}: '{}' is an override, whose value is chosen at pipeline "
                                                "creation; sg needs this one at reflection, so use a const",
                                                t.line, t.text));
            return cc::error(cc::format("line {}: '{}' is not a module-scope const", t.line, t.text));
        }
        auto text = t.text;
        if (!text.empty() && (text.back() == 'u' || text.back() == 'i'))
            text = text.subview({.offset = 0, .size = text.size() - 1});
        auto const not_literal
            = [&] { return cc::error(cc::format("line {}: '{}' is not an integer literal", t.line, t.text)); };
        if (text.size() <= 2 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X'))
        {
            auto const value = cc::from_string<i64>(text);
            if (!value.has_value())
                return not_literal();
            return value.value();
        }

        // cc has no hex from_string yet.
        auto value = i64(0);
        for (auto k = isize(2); k < text.size(); ++k)
        {
            auto const c = text[k];
            auto digit = 0;
            if (c >= '0' && c <= '9')
                digit = c - '0';
            else if (c >= 'a' && c <= 'f')
                digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                digit = c - 'A' + 10;
            else
                return not_literal();
            value = value * 16 + digit;
        }
        return value;
    }

    cc::result<parsed_type> read_type()
    {
        auto name = expect_identifier();
        CC_RETURN_IF_ERROR(name);
        auto type = parsed_type{.name = cc::string(name.value().text)};
        if (!is(peek(), "<"))
            return type;

        next();
        auto const is_array = type.name == "array" || type.name == "binding_array";
        for (auto arg_index = 0; !is(peek(), ">"); ++arg_index)
        {
            auto const& t = peek();
            if ((is_array && arg_index == 1) || t.kind == token_kind::number)
            {
                // The count may name a const declared further down, so it stays a token run until parse() has seen them all.
                auto depth = 0;
                while (peek().kind != token_kind::end && (depth > 0 || (!is(peek(), ",") && !is(peek(), ">"))))
                {
                    if (is(peek(), "("))
                        ++depth;
                    else if (is(peek(), ")"))
                        --depth;
                    type.count.push_back(next());
                }
            }
            else if (is(t, "read") || is(t, "write") || is(t, "read_write"))
            {
                type.access = t.text;
                next();
            }
            else if (t.kind == token_kind::identifier)
            {
                auto arg = read_type();
                CC_RETURN_IF_ERROR(arg);
                type.args.push_back(cc::move(arg.value()));
            }
            else
                return cc::error(cc::format("line {}: unexpected '{}' in a type", t.line, t.text));

            if (is(peek(), ","))
                next();
            else if (!is(peek(), ">"))
                return cc::error(
                    cc::format("line {}: expected ',' or '>' in a type, found '{}'", peek().line, peek().text));
        }
        next();
        return type;
    }

    cc::result<cc::unit> read_alias()
    {
        next(); // alias
        auto name = expect_identifier();
        CC_RETURN_IF_ERROR(name);
        CC_RETURN_IF_ERROR(expect("="));
        auto type = read_type();
        CC_RETURN_IF_ERROR(type);
        CC_RETURN_IF_ERROR(expect(";"));
        _aliases[cc::string(name.value().text)] = cc::move(type.value());
        return cc::unit{};
    }

    cc::result<cc::unit> read_const()
    {
        next(); // const
        auto name = expect_identifier();
        CC_RETURN_IF_ERROR(name);
        if (is(peek(), ":"))
        {
            next();
            auto type = read_type();
            CC_RETURN_IF_ERROR(type);
        }
        CC_RETURN_IF_ERROR(expect("="));

        // Only a lone integer literal is kept, since that is all a binding or a workgroup size can use.
        auto const& value = peek();
        if (value.kind == token_kind::number && is(peek(1), ";"))
            if (auto v = integer_of({value}, value.line); v.has_value())
                _constants[cc::string(name.value().text)] = v.value();
        skip_statement();
        return cc::unit{};
    }

    cc::result<cc::unit> read_struct()
    {
        next(); // struct
        auto name = expect_identifier();
        CC_RETURN_IF_ERROR(name);
        CC_RETURN_IF_ERROR(expect("{"));

        auto members = cc::vector<struct_member>();
        while (!is(peek(), "}"))
        {
            auto attrs = cc::vector<attribute>();
            CC_RETURN_IF_ERROR(read_attributes(attrs));
            auto member_name = expect_identifier();
            CC_RETURN_IF_ERROR(member_name);
            CC_RETURN_IF_ERROR(expect(":"));
            auto type = read_type();
            CC_RETURN_IF_ERROR(type);

            auto member = struct_member{.type = cc::move(type.value())};
            for (auto const& a : attrs)
                if ((a.name == "size" || a.name == "align") && a.args.size() == 1)
                    (a.name == "size" ? member.explicit_size : member.explicit_align) = a.args[0];
            members.push_back(cc::move(member));

            if (is(peek(), ","))
                next();
            else if (!is(peek(), "}"))
                return cc::error(cc::format("line {}: expected ',' or '}}' after a struct member, found '{}'",
                                            peek().line, peek().text));
        }
        next(); // }
        if (is(peek(), ";"))
            next();
        _structs[cc::string(name.value().text)] = cc::move(members);
        return cc::unit{};
    }

    cc::result<cc::unit> read_var(cc::vector<attribute> const& attrs, cc::vector<pending_binding>& bindings)
    {
        auto const line = next().line; // var
        auto binding = pending_binding{.line = line};

        if (is(peek(), "<"))
        {
            next();
            auto space = expect_identifier();
            CC_RETURN_IF_ERROR(space);
            binding.address_space = space.value().text;
            if (is(peek(), ","))
            {
                next();
                auto access = expect_identifier();
                CC_RETURN_IF_ERROR(access);
                binding.access = access.value().text;
            }
            CC_RETURN_IF_ERROR(expect(">"));
        }

        auto name = expect_identifier();
        CC_RETURN_IF_ERROR(name);
        binding.name = cc::string(name.value().text);

        if (is(peek(), ":"))
        {
            next();
            auto type = read_type();
            CC_RETURN_IF_ERROR(type);
            binding.type = cc::move(type.value());
        }
        skip_statement();

        for (auto const& a : attrs)
            if ((a.name == "group" || a.name == "binding") && a.args.size() == 1)
                (a.name == "group" ? binding.group : binding.index) = a.args[0];

        // A var with no resource address is module-private state, which is nothing sg binds.
        if (binding.group.empty() && binding.index.empty())
            return cc::unit{};
        if (binding.group.empty() || binding.index.empty())
            return cc::error(cc::format("line {}: '{}' needs both @group and @binding", line, binding.name));

        bindings.push_back(cc::move(binding));
        return cc::unit{};
    }

    cc::result<cc::unit> read_fn(cc::vector<attribute> const& attrs, cc::vector<pending_entry>& entries)
    {
        next(); // fn
        auto name = expect_identifier();
        CC_RETURN_IF_ERROR(name);

        auto stage = cc::optional<sg::shader_stage>();
        auto workgroup = cc::optional<attribute const*>();
        for (auto const& a : attrs)
        {
            if (a.name == "vertex")
                stage = sg::shader_stage::vertex;
            else if (a.name == "fragment")
                stage = sg::shader_stage::fragment;
            else if (a.name == "compute")
                stage = sg::shader_stage::compute;
            else if (a.name == "workgroup_size")
                workgroup = &a;
        }

        // The signature and body are skipped by bracket matching: nothing inside a function matters to a layout.
        while (!is(peek(), "{") && peek().kind != token_kind::end)
            next();
        auto depth = 0;
        do
        {
            auto const& t = next();
            if (is(t, "{"))
                ++depth;
            else if (is(t, "}"))
                --depth;
        } while (depth > 0 && peek().kind != token_kind::end);
        if (depth > 0)
            return cc::error(cc::format("line {}: the body of '{}' is not closed", name.value().line, name.value().text));

        if (!stage.has_value())
            return cc::unit{};

        auto entry = pending_entry{
            .declared = {.stage = stage.value(), .entry_point = cc::string(name.value().text)},
            .workgroup_size = {},
            .line = name.value().line,
        };
        if (stage.value() == sg::shader_stage::compute)
        {
            if (!workgroup.has_value())
                return cc::error(cc::format("line {}: compute entry point '{}' has no @workgroup_size",
                                            name.value().line, name.value().text));
            entry.workgroup_size = workgroup.value()->args;
            entry.line = workgroup.value()->line;
        }
        entries.push_back(cc::move(entry));
        return cc::unit{};
    }

    /// Resolves an alias to what it names, however many aliases deep.
    parsed_type const& resolved(parsed_type const& type) const
    {
        auto const* t = &type;
        while (auto const* alias = _aliases.get_ptr(t->name))
            t = alias;
        return *t;
    }

    /// The layout of a host-shareable type, under WGSL's alignment and size rules.
    cc::result<type_layout> layout_of(parsed_type const& type_in, int line) const
    {
        auto const& type = resolved(type_in);
        auto const& n = type.name;

        auto scalar = [&](cc::string_view s) -> cc::optional<isize>
        {
            if (s == "f32" || s == "i32" || s == "u32")
                return 4;
            if (s == "f16")
                return 2;
            return {};
        };

        if (auto const size = scalar(n); size.has_value())
            return type_layout{.align = size.value(), .size = size.value()};
        if (n == "atomic" && type.args.size() == 1)
            return layout_of(type.args[0], line);

        // vecN<T> and its shorthands (vec3f, vec2u, ...).
        auto vector = [&](isize components, isize scalar_size) -> type_layout
        {
            auto const align = (components == 3 ? 4 : components) * scalar_size;
            return type_layout{.align = align, .size = components * scalar_size};
        };
        auto shorthand_scalar = [&](char c) -> isize { return c == 'h' ? 2 : 4; };

        if (n.size() == 4 && n.starts_with("vec") && n[3] >= '2' && n[3] <= '4' && type.args.size() == 1)
        {
            auto const s = scalar(type.args[0].name);
            if (!s.has_value())
                return cc::error(cc::format("line {}: vector of '{}' is not host-shareable", line, type.args[0].name));
            return vector(n[3] - '0', s.value());
        }
        if (n.size() == 5 && n.starts_with("vec") && n[3] >= '2' && n[3] <= '4' && cc::string_view("fiuh").contains(n[4]))
            return vector(n[3] - '0', shorthand_scalar(n[4]));

        // matCxR<T> and its shorthands: C columns of vecR.
        if (n.size() >= 6 && n.starts_with("mat") && n[4] == 'x')
        {
            auto const columns = isize(n[3] - '0');
            auto const rows = isize(n[5] - '0');
            auto scalar_size = isize(4);
            if (n.size() == 7)
                scalar_size = shorthand_scalar(n[6]);
            else if (type.args.size() == 1)
                scalar_size = scalar(type.args[0].name).value_or(4);
            auto const column = vector(rows, scalar_size);
            return type_layout{.align = column.align,
                               .size = columns * cc::int_round_up_to_multiple(column.size.value(), column.align)};
        }

        if (n == "array")
        {
            if (type.args.empty())
                return cc::error(cc::format("line {}: an array needs an element type", line));
            auto element = layout_of(type.args[0], line);
            CC_RETURN_IF_ERROR(element);
            auto const& e = element.value();
            if (!e.size.has_value())
                return cc::error(cc::format("line {}: an array element must have a fixed size", line));
            auto const stride = cc::int_round_up_to_multiple(e.size.value(), e.align);
            if (type.count.empty())
                return type_layout{.align = e.align, .size = {}};
            auto count = integer_of(type.count, line);
            CC_RETURN_IF_ERROR(count);
            return type_layout{.align = e.align, .size = stride * isize(count.value())};
        }

        if (auto const* members = _structs.get_ptr(n))
        {
            auto offset = isize(0);
            auto align = isize(1);
            auto size = cc::optional<isize>(0);
            for (auto k = isize(0); k < members->size(); ++k)
            {
                auto const& m = (*members)[k];
                auto layout = layout_of(m.type, line);
                CC_RETURN_IF_ERROR(layout);
                auto member_align = layout.value().align;
                auto member_size = layout.value().size;
                if (!m.explicit_align.empty())
                {
                    auto value = integer_of(m.explicit_align, line);
                    CC_RETURN_IF_ERROR(value);
                    member_align = isize(value.value());
                }
                if (!m.explicit_size.empty())
                {
                    auto value = integer_of(m.explicit_size, line);
                    CC_RETURN_IF_ERROR(value);
                    member_size = isize(value.value());
                }
                align = cc::max(align, member_align);
                offset = cc::int_round_up_to_multiple(offset, member_align);
                if (!member_size.has_value())
                {
                    if (k + 1 != members->size())
                        return cc::error(
                            cc::format("line {}: only a struct's last member may be a runtime-sized array", line));
                    size = {};
                    break;
                }
                offset += member_size.value();
            }
            if (size.has_value())
                size = cc::int_round_up_to_multiple(offset, align);
            return type_layout{.align = align, .size = size};
        }

        return cc::error(cc::format("line {}: '{}' is not a host-shareable type sg can lay out", line, n));
    }

    static cc::optional<sg::texture_view_dimension> texture_dimension_of(cc::string_view n)
    {
        if (n.ends_with("_1d"))
            return sg::texture_view_dimension::tex_1d;
        if (n.ends_with("_2d_array"))
            return sg::texture_view_dimension::tex_2d_array;
        if (n == "texture_multisampled_2d" || n == "texture_depth_multisampled_2d")
            return sg::texture_view_dimension::tex_2d_ms;
        if (n.ends_with("_2d"))
            return sg::texture_view_dimension::tex_2d;
        if (n.ends_with("_3d"))
            return sg::texture_view_dimension::tex_3d;
        if (n.ends_with("_cube_array"))
            return sg::texture_view_dimension::cube_array;
        if (n.ends_with("_cube"))
            return sg::texture_view_dimension::cube;
        return {};
    }

    static cc::optional<sg::pixel_format> image_format_of(cc::string_view f)
    {
        struct entry
        {
            cc::string_view wgsl;
            sg::pixel_format format;
        };
        static constexpr entry table[] = {
            {"r8unorm", sg::pixel_format::r8_unorm},
            {"r8snorm", sg::pixel_format::r8_snorm},
            {"r8uint", sg::pixel_format::r8_uint},
            {"r8sint", sg::pixel_format::r8_sint},
            {"rg8unorm", sg::pixel_format::rg8_unorm},
            {"rg8snorm", sg::pixel_format::rg8_snorm},
            {"rg8uint", sg::pixel_format::rg8_uint},
            {"rg8sint", sg::pixel_format::rg8_sint},
            {"rgba8unorm", sg::pixel_format::rgba8_unorm},
            {"rgba8snorm", sg::pixel_format::rgba8_snorm},
            {"rgba8uint", sg::pixel_format::rgba8_uint},
            {"rgba8sint", sg::pixel_format::rgba8_sint},
            {"bgra8unorm", sg::pixel_format::bgra8_unorm},
            {"r16float", sg::pixel_format::r16_float},
            {"r16uint", sg::pixel_format::r16_uint},
            {"r16sint", sg::pixel_format::r16_sint},
            {"rg16float", sg::pixel_format::rg16_float},
            {"rg16uint", sg::pixel_format::rg16_uint},
            {"rg16sint", sg::pixel_format::rg16_sint},
            {"rgba16float", sg::pixel_format::rgba16_float},
            {"rgba16uint", sg::pixel_format::rgba16_uint},
            {"rgba16sint", sg::pixel_format::rgba16_sint},
            {"r32float", sg::pixel_format::r32_float},
            {"r32uint", sg::pixel_format::r32_uint},
            {"r32sint", sg::pixel_format::r32_sint},
            {"rg32float", sg::pixel_format::rg32_float},
            {"rg32uint", sg::pixel_format::rg32_uint},
            {"rg32sint", sg::pixel_format::rg32_sint},
            {"rgba32float", sg::pixel_format::rgba32_float},
            {"rgba32uint", sg::pixel_format::rgba32_uint},
            {"rgba32sint", sg::pixel_format::rgba32_sint},
            {"rgb10a2unorm", sg::pixel_format::rgb10a2_unorm},
            {"rg11b10ufloat", sg::pixel_format::rg11b10_float},
        };
        for (auto const& e : table)
            if (e.wgsl == f)
                return e.format;
        return {};
    }

    cc::result<cc::unit> to_binding(pending_binding const& p, sg::binding& b) const
    {
        auto const& type = resolved(p.type);
        auto const& n = type.name;
        auto group_value = integer_of(p.group, p.line);
        CC_RETURN_IF_ERROR(group_value);
        auto index_value = integer_of(p.index, p.line);
        CC_RETURN_IF_ERROR(index_value);
        auto const group = u32(group_value.value());
        auto const index = u32(index_value.value());
        b.name = p.name;
        b.group_index = group;
        b.index = index;
        b.count = 1;

        if (n == "binding_array")
            return cc::error(
                cc::format("line {}: '{}' is a binding array, which WebGPU core does not have", p.line, p.name));

        if (p.address_space == "uniform")
        {
            b.type = sg::binding_type::uniform_buffer;
            auto layout = layout_of(type, p.line);
            CC_RETURN_IF_ERROR(layout);
            if (!layout.value().size.has_value())
                return cc::error(cc::format("line {}: uniform '{}' has no fixed size", p.line, p.name));
            b.block_size = layout.value().size.value();
        }
        else if (p.address_space == "storage")
        {
            b.type = sg::binding_type::buffer;
            b.access = p.access == "read_write" ? sg::access_mode::read_write : sg::access_mode::read;
        }
        else if (!p.address_space.empty())
            return cc::error(cc::format("line {}: '{}' is in the {} address space, which is not a resource sg binds",
                                        p.line, p.name, p.address_space));
        else if (n == "sampler")
        {
            b.type = sg::binding_type::sampler;
            b.sampler_type = sg::sampler_binding_type::filtering;
        }
        else if (n == "sampler_comparison")
        {
            b.type = sg::binding_type::sampler;
            b.sampler_type = sg::sampler_binding_type::comparison;
        }
        else if (n == "texture_external")
            return cc::error(cc::format("line {}: '{}' is a texture_external, which sg has no view for", p.line, p.name));
        else if (n.starts_with("texture_storage_"))
        {
            b.type = sg::binding_type::image;
            b.texture_dimension = texture_dimension_of(n);
            if (type.args.empty())
                return cc::error(cc::format("line {}: storage texture '{}' declares no format", p.line, p.name));
            b.image_format = image_format_of(type.args[0].name);
            if (!b.image_format.has_value())
                return cc::error(cc::format("line {}: storage texture '{}' uses format '{}', which has no "
                                            "sg::pixel_format",
                                            p.line, p.name, type.args[0].name));
            // WGSL requires the access mode on a storage texture, so an absent one is a shader naga will refuse anyway.
            if (type.access == "read")
                b.access = sg::access_mode::read;
            else if (type.access == "write")
                b.access = sg::access_mode::write;
            else
                b.access = sg::access_mode::read_write;
        }
        else if (n.starts_with("texture_"))
        {
            b.type = sg::binding_type::texture;
            b.texture_dimension = texture_dimension_of(n);
            if (!b.texture_dimension.has_value())
                return cc::error(cc::format("line {}: '{}' is not a texture type sg knows", p.line, n));
            if (n.starts_with("texture_depth_"))
                b.sample_type = sg::texture_sample_type::depth;
            else if (!type.args.empty() && type.args[0].name == "i32")
                b.sample_type = sg::texture_sample_type::sint;
            else if (!type.args.empty() && type.args[0].name == "u32")
                b.sample_type = sg::texture_sample_type::uint;
            else if (n == "texture_multisampled_2d")
                b.sample_type = sg::texture_sample_type::unfilterable_float; // WebGPU never filters a multisampled texture
            else
                b.sample_type = sg::texture_sample_type::filterable_float;
        }
        else
            return cc::error(cc::format("line {}: '{}' of type '{}' is not a resource sg binds", p.line, p.name, n));

        // The reserved group reads differently: see wgsl_declarations::bindings.
        if (group == u32(sg::reserved_binding_group))
        {
            if (index == 0)
            {
                if (b.type != sg::binding_type::uniform_buffer)
                    return cc::error(cc::format("line {}: @group({}) @binding(0) is the inline-constants block, so "
                                                "'{}' must be a var<uniform>",
                                                p.line, group, p.name));
                b.group_index = {};
            }
            else if (b.type == sg::binding_type::sampler)
                b.index = index - 1;
            else
                return cc::error(cc::format("line {}: @group({}) is sg's reserved group, which holds only the "
                                            "inline-constants block at binding 0 and static samplers after it",
                                            p.line, group));
        }
        return cc::unit{};
    }
};
} // namespace

cc::result<slib::wgsl_declarations> slib::parse_wgsl_declarations(cc::string_view source)
{
    auto tokens = tokenize(source);
    CC_RETURN_IF_ERROR(tokens);
    return parser(cc::move(tokens.value())).parse();
}
