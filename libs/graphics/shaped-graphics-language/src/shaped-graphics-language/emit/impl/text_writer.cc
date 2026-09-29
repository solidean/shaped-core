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
        indent(out, depth);
        out += text;
        out += "\n";
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

    /// True when writing `id` puts lines in front of the statement that holds it: a struct built member by member.
    /// Such an expression cannot stand where it is evaluated more than once, or behind an `else`.
    bool writes_lines(flat_expr_id id) const
    {
        auto const& x = p.e.at(id);
        if (needs_member_assignment(x))
            return true;
        if (is_hlsl_atomic(id))
            return true;
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
        return record != nullptr && record->is_atomic;
    }

    bool needs_member_assignment(flat_expr const& x) const
    {
        return x.node.is<flat_construct>() && !is_builtin_type(p.m, x.type) && !d.has_struct_constructor();
    }

    /// `nonuniform i`, whose own text is `i`.
    [[nodiscard]] bool is_nonuniform_mark(flat_expr_id id) const
    {
        auto const* const c = p.e.at(id).node.try_as<flat_call>();
        auto const* const record = c != nullptr ? p.m.builtin_function(c->intrinsic) : nullptr;
        return record != nullptr && record->is_nonuniform_mark;
    }

    [[nodiscard]] bool is_array(check::type_id type) const
    {
        return is_valid(type) && p.m.at(type).kind == check::type_kind::array;
    }

    /// Declares `name` and assigns its members, or its elements, one by one, for a target without a struct constructor.
    void build_struct(flat_expr const& x, cc::string_view name)
    {
        auto declaration = cc::string();
        auto const dimensions = array_dimensions(p, x.type);
        d.write_local(declaration,
                      {.name = name, .type = type_text(p, d, x.type), .dimensions = dimensions, .is_mut = true});
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
            if (planned.member_of[i] >= 0)
                line(cc::format("{}.{} = {};", name, planned.members[planned.member_of[i]].name, expr(arguments[i]).text));
    }

    /// `is_broken` gives every argument a line of its own, which is how a struct built as a statement's value reads.
    rendered construct(flat_expr const& x, bool is_broken)
    {
        if (needs_member_assignment(x))
        {
            // A construction in the middle of an expression: every expression is pure, so building it first changes nothing.
            auto name = p.names.mint(is_array(x.type) ? cc::string("array_value")
                                                      : cc::format("{}_value", type_text(p, d, x.type)));
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
        auto text = cc::string(type_text(p, d, x.type));
        text += "(";
        for (auto i = isize(0); i < arguments.size(); ++i)
        {
            if (is_split)
            {
                text += i == 0 ? "\n" : ",\n";
                indent(text, depth + 1);
            }
            else if (i != 0)
                text += ", ";
            text += expr(arguments[i]).text;
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
        return cc::format("{}.{}", r.root, r.form->fields[leaf.fields[i]].name);
    }

    /// The value `r` addresses, rebuilt from the fields that hold it.
    cc::string read_memory(memory_ref const& r)
    {
        auto const type = cc::string(type_text(p, d, r.type));
        if (is_builtin_type(p.m, r.type))
        {
            auto const& leaf = leaf_at(r);
            if (leaf.is_packed)
                return cc::format("{}({})", type, field_text(r, leaf, 0));
            if (!leaf.is_split)
                return field_text(r, leaf, 0);
            auto const& record = *p.m.builtin_type_of(r.type);
            auto pieces = cc::vector<cc::string>();
            if (record.leaf_count > 4 && !d.has_struct_constructor())
            {
                // MSL builds a matrix from its columns, not from its scalars.
                auto const column = cc::string(builtin_spelling(p, "float4"));
                for (auto c = 0; c < 4; ++c)
                    pieces.push_back(cc::format("{}({}, {}, {}, {})", column, field_text(r, leaf, c * 4),
                                                field_text(r, leaf, c * 4 + 1), field_text(r, leaf, c * 4 + 2),
                                                field_text(r, leaf, c * 4 + 3)));
            }
            else
                for (auto i = isize(0); i < leaf.fields.size(); ++i)
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
            for (auto i = 0; i < record.leaf_count; ++i)
            {
                auto const piece = record.leaf_count > 4
                                     ? cc::format("{}[{}][{}]", name, i / 4, i % 4)
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

    /// `place = value;`, through a memory form where the place is in one.
    void assign(flat_assign const& a)
    {
        auto const value = expr(a.value, true).text;
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

    /// Every call is written from its registry record; nothing here knows one builtin from another.
    rendered call(flat_call const& c)
    {
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
            auto result = how.custom(
                {.target = d.language(), .arguments = arguments, .builtins = *p.m.builtins, .data = how.data, .mint = mint});
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
        auto const& x = p.e.at(id);
        if (auto const memory = memory_of(id); memory.has_value())
            return {.text = read_memory(memory.value())};
        // A component of a vector in a memory form is read from the field that holds it, not from the rebuilt vector.
        if (auto const* const member = x.node.try_as<flat_member>())
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
        x.node.visit(
            [&](flat_invalid const&) {}, [&](flat_literal const& l)
            { result = {.text = literal_text(l.value), .binds = l.value < 0 ? level::unary : level::primary}; },
            [&](flat_int_literal const& l)
            {
                auto const is_wrapped = l.value == -2147483647 - 1;
                auto text = l.is_unsigned ? cc::to_string(u32(l.value)) + "u" : int_literal_text(l.value);
                result = {.text = cc::move(text), .binds = l.value < 0 && !is_wrapped ? level::unary : level::primary};
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
                // A builtin's fields are spelled alike everywhere: x, y, z, w.
                if (is_builtin_type(p.m, object_type))
                {
                    result
                        = {.text = cc::format("{}.{}", object, p.m.at(p.m.at(object_type).members)[member.member].name)};
                    return;
                }
                auto const& planned = p.structs[p.struct_of_type[index_of(object_type)]];
                result = {.text = cc::format("{}.{}", object, planned.members[planned.member_of[member.member]].name)};
            },
            [&](flat_construct const&) { result = construct(x, is_broken); },
            [&](flat_call const& c) { result = call(c); }, [&](flat_not const& n)
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

    void loop_while(flat_while const& s)
    {
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
        d.write_local(declaration, {.name = name,
                                    .type = type_text(p, d, local.type),
                                    .value = text,
                                    .dimensions = dimensions,
                                    .is_mut = is_mut || declares_only});
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
        s.node.visit(
            [&](flat_let const& let) { declare(let.local, let.value, p.e.at(let.local).is_mut); },
            [&](flat_var const& var) { declare(var.local, var.value, true); }, [&](flat_assign const& a) { assign(a); },
            // `validate` refuses a tree that holds one
            [&](flat_print const&) {}, //
            [&](flat_eval const& v)
            {
                // an atomic HLSL writes as statements alone has nothing left to evaluate
                if (is_hlsl_atomic(v.value))
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
            },
            [&](flat_if const& i) { branch(i, false); },
            // none of the three is in a core tree
            [&](flat_block const&) {}, //
            [&](flat_leave const&) {}, //
            [&](flat_case const&) {},
            [&](flat_loop const& l)
            {
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
    return p.structs[p.struct_of_type[index_of(type)]].name;
}


void sgl::emit::impl::write_helpers(cc::string& out, plan const& p, dialect const& d)
{
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

cc::string sgl::emit::impl::write_text(plan& p, dialect const& d)
{
    auto w = writer{.p = p, .d = d};
    w.out.appendf("// SGL {} entry point '{}', written as {}.\n", check::stage_name(p.e.entry_stage), p.entry_name,
                  d.description());
    w.out += "// Generated: the SGL source is what to edit.\n\n";
    d.write_declarations(w.out, p);
    write_helpers(w.out, p, d);
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
    return cc::move(w.out);
}
