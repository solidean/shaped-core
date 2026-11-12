#include <clean-core/math/bit.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// Comparing types, and what an instance decides of a `const if` before its branches are checked (CHK-429, CHK-430).

namespace
{
constexpr auto error_type = checked_module::error_type;
} // namespace

type_id checker::type_parameter_stands_for(cc::string_view name) const
{
    for (auto i = type_parameter_names.size(); i > 0; --i)
        if (type_parameter_names[i - 1].first == name)
            return type_parameter_names[i - 1].second;
    return type_id::none;
}

bool checker::is_type_operand(function_scope const& scope, ast::expr_id expr) const
{
    auto const file = scope.file;
    auto const& node = ast_of(file).at(expr).node;
    // `scalar_of[T]` and a generic struct's instance, `ring[float, 4]`, are named by their head
    auto head = expr;
    if (auto const* const index = node.try_as<ast::index>())
        head = index->object;
    auto const* const n = ast::is_valid(head) ? ast_of(file).at(head).node.try_as<ast::name>() : nullptr;
    if (n != nullptr)
    {
        auto const text = text_of(file, n->where);
        if (scope.find_local(text) != nullptr)
            return false;
        if (head == expr && is_type_parameter_name(text))
        {
            // a value parameter is a constant in an instance, and a parameter with a value type in the open body
            auto const& t = out.at(type_parameter_stands_for(text));
            return t.kind != type_kind::constant && !(t.kind == type_kind::type_parameter && is_valid(t.element));
        }
        if (head != expr && text == "scalar_of")
            return true;
    }
    auto const* const found = symbols_named(file, head, &scope);
    if (found == nullptr || found->empty())
        return false;
    auto const kind = out.at(found->front()).kind;
    return kind == symbol_kind::structure || kind == symbol_kind::enumeration || kind == symbol_kind::alias;
}

cc::optional<type_id> checker::check_type_comparison(function_scope& scope, ast::expr_id id, ast::call const& call)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const arguments = ast.at(call.arguments);
    if (arguments.size() != 2)
        return cc::nullopt;
    for (auto const& a : arguments)
        if (!a.name.empty() || a.is_splat || !ast::is_valid(a.value))
            return cc::nullopt;
    auto const left = arguments[0].value;
    auto const right = arguments[1].value;
    if (!is_type_operand(scope, left) && !is_type_operand(scope, right))
        return cc::nullopt;

    // CHK-429: one operand naming a type makes both type positions
    auto const bool_type = type_of_builtin(builtins::k_bool, file, span_of(file, id));
    type_id types[2];
    for (auto k = 0; k < 2; ++k)
    {
        auto const operand = k == 0 ? left : right;
        if (!is_type_operand(scope, operand))
        {
            report(diagnostic_kind::type_mismatch, file, span_of(file, operand),
                   "a type compares with a type: `T == float`, never with a value");
            return error_type;
        }
        types[k] = resolve_type(file, operand, &scope);
        if (types[k] == error_type)
            return error_type;
    }
    // the open body judges names alone, and an instance knows both types
    if (!is_open(types[0]) && !is_open(types[1]) && bool_type != error_type)
    {
        auto const is_equal = types[0] == types[1];
        auto const is_negated = text_of(file, file_of(file).at(call.op).where) == "!=";
        set_target(file, id,
                   {.kind = target_kind::generic_value,
                    .index = i32(index_of(constant_type(bool_type, is_equal != is_negated ? 1u : 0u)))});
    }
    return bool_type;
}

cc::optional<bool> checker::known_condition(i32 file, ast::expr_id expr) const
{
    if (!ast::is_valid(expr))
        return cc::nullopt;
    auto const& ast = ast_of(file);
    auto const& node = ast.at(expr).node;
    auto const& where = tables_of(file).target_at(expr);

    // a generic's own bool, and a type comparison an instance decided
    if (where.kind == target_kind::generic_value)
    {
        auto const& constant = out.types[where.index];
        if (out.name_of(constant.element) == "bool")
            return constant.count != 0;
        return cc::nullopt;
    }
    // `true`, `false`, and a const of either that no option reaches
    if (where.kind == target_kind::symbol && is_valid(where.symbol) && out.at(where.symbol).kind == symbol_kind::constant
        && out.at(where.symbol).state == symbol_state::checked && !is_target_constant(where.symbol))
    {
        auto const& c = out.constants[out.at(where.symbol).info];
        if (c.kind != constant_kind::enum_case || is_valid(c.option) || !c.computed_options.empty()
            || out.name_of(c.type) != "bool")
            return cc::nullopt;
        // bool's cases are `false`, then `true`
        return c.case_index == 1;
    }
    auto const* const call = node.try_as<ast::call>();
    if (call == nullptr || !sgl::is_valid(call->op))
        return cc::nullopt;
    auto const op = text_of(file, file_of(file).at(call->op).where);
    auto const arguments = ast.at(call->arguments);
    if (op == "not" && arguments.size() == 1)
    {
        auto const inner = known_condition(file, arguments[0].value);
        return inner.has_value() ? cc::optional<bool>(!inner.value()) : cc::nullopt;
    }
    if (arguments.size() != 2)
        return cc::nullopt;
    // three-valued: a known false decides an `and`, and a known true an `or`, whatever the other side is
    if (op == "and" || op == "or")
    {
        auto const is_and = op == "and";
        auto const a = known_condition(file, arguments[0].value);
        auto const b = known_condition(file, arguments[1].value);
        if ((a.has_value() && a.value() != is_and) || (b.has_value() && b.value() != is_and))
            return !is_and;
        if (a.has_value() && b.has_value())
            return is_and;
        return cc::nullopt;
    }
    // a comparison of numbers an instance knows: a value parameter, a literal, a plain const, an enum's case
    if (!call->is_short_circuit && call->spelling == ast::call_spelling::infix)
    {
        auto const x = known_number_of(file, arguments[0].value);
        auto const y = known_number_of(file, arguments[1].value);
        if (!x.has_value() || !y.has_value())
            return cc::nullopt;
        auto const a = x.value();
        auto const b = y.value();
        if (op == "==")
            return a == b;
        if (op == "!=")
            return a != b;
        if (op == "<")
            return a < b;
        if (op == "<=")
            return a <= b;
        if (op == ">")
            return a > b;
        if (op == ">=")
            return a >= b;
    }
    return cc::nullopt;
}

cc::optional<f64> checker::known_number_of(i32 file, ast::expr_id expr) const
{
    if (!ast::is_valid(expr))
        return cc::nullopt;
    if (auto const n = number_of(file, expr); n.is_number)
        return n.is_integer ? f64(n.integer) : n.real;
    auto const& where = tables_of(file).target_at(expr);
    if (where.kind == target_kind::enum_case)
        return f64(where.index);
    if (where.kind == target_kind::generic_value)
    {
        auto const& constant = out.types[where.index];
        auto const element = out.name_of(constant.element);
        auto const bits = u32(constant.count);
        if (out.at(constant.element).kind == type_kind::enumeration || element == "int")
            return f64(i32(bits));
        if (element == "uint")
            return f64(bits);
        if (element == "float")
            return f64(cc::bit_cast<f32>(bits));
        return cc::nullopt;
    }
    if (where.kind == target_kind::symbol && is_valid(where.symbol) && out.at(where.symbol).kind == symbol_kind::constant
        && out.at(where.symbol).state == symbol_state::checked && !is_target_constant(where.symbol))
    {
        auto const& c = out.constants[out.at(where.symbol).info];
        if (is_valid(c.option) || !c.computed_options.empty())
            return cc::nullopt;
        switch (c.kind)
        {
        case constant_kind::integer:
            return c.is_unsigned ? f64(u32(c.integer)) : f64(c.integer);
        case constant_kind::enum_case:
            return f64(c.case_index);
        default:
            return cc::nullopt;
        }
    }
    return cc::nullopt;
}

// ---- text types (CHK-432) ----------------------------------------------------------------------------------------

namespace
{
/// The position of `name` in `names`, or -1 where it is none of them.
template <isize N>
i32 position_in(cc::string_view const (&names)[N], cc::string_view name)
{
    for (auto i = isize(0); i < N; ++i)
        if (names[i] == name)
            return i32(i);
    return -1;
}
} // namespace

bool checker::target_condition_of(i32 file, ast::expr_id expr, cc::vector<target_condition>& into)
{
    auto const& ast = ast_of(file);
    auto const refuse = [&]
    {
        unsupported(file, span_of(file, expr),
                    "a text type's condition other than target_api, target_language and has(.f), joined by and, or "
                    "and not");
        return false;
    };
    if (!ast::is_valid(expr))
        return false;
    auto const& node = ast.at(expr).node;
    auto const add = [&](target_condition c)
    {
        into.push_back(c);
        return true;
    };
    auto const named = [&](ast::expr_id e) -> symbol_id
    {
        auto const* const n = ast::is_valid(e) ? ast.at(e).node.try_as<ast::name>() : nullptr;
        auto const* const found = n != nullptr ? seen_from(file, text_of(file, n->where)) : nullptr;
        return found != nullptr && !found->empty() ? found->front() : symbol_id::none;
    };
    auto const dotted = [&](ast::expr_id e) -> cc::string_view
    {
        auto const* const d = ast::is_valid(e) ? ast.at(e).node.try_as<ast::leading_dot>() : nullptr;
        return d != nullptr ? text_of(file, d->name) : cc::string_view();
    };

    if (auto const s = named(expr); is_valid(s) && is_prelude_file(out.at(s).file)
                                    && out.at(s).kind == symbol_kind::constant
                                    && (out.at(s).name == "true" || out.at(s).name == "false"))
    {
        add({.kind = target_condition::kind_t::always});
        if (out.at(s).name == "false")
            add({.kind = target_condition::kind_t::negated, .lhs = i32(into.size()) - 1});
        return true;
    }
    auto const* const call = node.try_as<ast::call>();
    if (call == nullptr)
        return refuse();
    auto const arguments = ast.at(call->arguments);
    for (auto const& a : arguments)
        if (!a.name.empty() || a.is_splat)
            return refuse();

    // `has(.f)`, the prelude's
    if (!sgl::is_valid(call->op))
    {
        auto const callee = named(call->callee);
        if (!is_valid(callee) || !is_prelude_file(out.at(callee).file) || out.at(callee).name != "has"
            || arguments.size() != 1)
            return refuse();
        auto const name = dotted(arguments[0].value);
        auto const f = find_feature(name);
        if (f < 0)
        {
            report(diagnostic_kind::unknown_feature, file, span_of(file, arguments[0].value), name);
            return false;
        }
        return add({.kind = target_condition::kind_t::has, .value = f});
    }
    auto const op = text_of(file, file_of(file).at(call->op).where);
    if (op == "not" && arguments.size() == 1)
    {
        if (!target_condition_of(file, arguments[0].value, into))
            return false;
        return add({.kind = target_condition::kind_t::negated, .lhs = i32(into.size()) - 1});
    }
    if (arguments.size() != 2)
        return refuse();
    if (op == "and" || op == "or")
    {
        if (!target_condition_of(file, arguments[0].value, into))
            return false;
        auto const lhs = i32(into.size()) - 1;
        if (!target_condition_of(file, arguments[1].value, into))
            return false;
        return add({.kind = op == "and" ? target_condition::kind_t::both : target_condition::kind_t::either,
                    .lhs = lhs,
                    .rhs = i32(into.size()) - 1});
    }
    if (op != "==" && op != "!=")
        return refuse();
    // `target_api == .vulkan`, either way round
    for (auto k = 0; k < 2; ++k)
    {
        auto const s = named(arguments[k].value);
        auto const name = dotted(arguments[1 - k].value);
        if (!is_valid(s) || !is_target_constant(s) || name.empty())
            continue;
        auto const is_api = out.at(s).name == "target_api";
        auto const value = is_api ? position_in(k_graphics_api_names, name) : position_in(k_shading_language_names, name);
        if (value < 0)
        {
            report(diagnostic_kind::unknown_member, file, span_of(file, arguments[1 - k].value),
                   cc::format("{} has no case {}", is_api ? "graphics_api" : "shading_language", name));
            return false;
        }
        add({.kind = is_api ? target_condition::kind_t::api_is : target_condition::kind_t::language_is, .value = value});
        if (op == "!=")
            add({.kind = target_condition::kind_t::negated, .lhs = i32(into.size()) - 1});
        return true;
    }
    return refuse();
}

bool checker::compile_type_branches(symbol_id id, i32 file, ast::struct_decl const& s)
{
    auto const& ast = ast_of(file);
    auto const name = cc::string(out.at(id).name);
    auto has_fields = false;
    for (auto const& branch : ast.at(s.branches))
    {
        auto record = target_type_branch{.type_symbol = id, .is_fields = ast.at(branch.texts).empty()};
        if (ast::is_valid(branch.condition))
            (void)target_condition_of(file, branch.condition, record.condition);
        if (record.is_fields)
        {
            // one SGL branch, so the fields a check judges are the same for every compile
            if (has_fields)
                report(diagnostic_kind::invalid_target_text, file, file_of(file).at(branch.form).where,
                       cc::format("{} has a second SGL branch, and a type's fields are one list for every target", name));
            has_fields = true;
        }
        for (auto const text : ast.at(branch.texts))
        {
            auto spelled = plain_target_text(file, text, "a type's spelling");
            if (!spelled.has_value())
                continue;
            auto const language = spelled.value().language;
            if (record.languages.values().contains(language))
            {
                report(diagnostic_kind::invalid_target_text, file, span_of(file, text),
                       cc::format("{} has a second spelling in {}, where one per language is all a target takes", name,
                                  k_shading_language_names[isize(language)]));
                continue;
            }
            // a spelling is a type's name, so its lines are one
            auto line = cc::string();
            for (auto const c : spelled.value().text.as_span())
            {
                auto const is_space = c == ' ' || c == '\n' || c == '\r' || c == '\t';
                if (!is_space)
                    line += c;
                else if (!line.empty() && line.back() != ' ')
                    line += ' ';
            }
            while (!line.empty() && line.back() == ' ')
                line.resize_down_to(line.size() - 1);
            if (line.empty())
            {
                report(diagnostic_kind::invalid_target_text, file, span_of(file, text),
                       cc::format("{} spells no type in {}", name, k_shading_language_names[isize(language)]));
                continue;
            }
            record.languages.push_back(language);
            record.spellings.push_back(cc::move(line));
        }
        out.target_type_branches.push_back(cc::move(record));
    }
    return has_fields;
}

// ---- target layouts (CHK-522) ------------------------------------------------------------------------------------

namespace
{
/// The modules SGL ships, which a program reaches by `use` without a module directory of its own.
constexpr cc::string_view k_shipped_modules[] = {"accel"};
} // namespace

bool checker::is_shipped_module_file(i32 file) const
{
    if (file < 0 || file >= file_module.size() || file_module[file] <= 0)
        return false;
    for (auto const name : k_shipped_modules)
        if (modules[file_module[file]] == name)
            return true;
    return false;
}

bool checker::has_target_layout(type_id type) const
{
    return is_valid(type) && type != checked_module::error_type && out.at(type).kind == type_kind::structure
        && out.at(type).has_target_layout;
}

bool checker::refuse_held_layout(i32 file, source_span where, type_id type, cc::string_view holder)
{
    auto inner = type;
    while (is_valid(inner) && inner != checked_module::error_type && out.at(inner).kind == type_kind::array)
        inner = out.at(inner).element;
    if (!has_target_layout(inner))
        return false;
    unsupported(file, where,
                cc::format("{} holding a {}, which stands in each target's own order, so only a buffer's element and a "
                           "value of a body hold one",
                           holder, out.name_of(inner)));
    return true;
}

void checker::compile_layout_branches(symbol_id id,
                                      type_id type,
                                      i32 file,
                                      ast::struct_decl const& s,
                                      ast::range_of<member_info> fields)
{
    auto const& ast = ast_of(file);
    auto const name = cc::string(out.at(id).name);
    auto is_first = true;
    for (auto const& branch : ast.at(s.branches))
    {
        auto record = target_type_branch{.type_symbol = id, .is_fields = true};
        if (ast::is_valid(branch.condition))
            (void)target_condition_of(file, branch.condition, record.condition);
        if (!ast.at(branch.texts).empty())
        {
            report(diagnostic_kind::invalid_target_text, file, file_of(file).at(branch.form).where,
                   cc::format("{} lays its fields out per target, so each branch is its fields, never a target's text",
                              name));
            continue;
        }
        // the first branch is the fields the check judges, so its members are those already compiled
        auto const members = is_first ? fields : compile_members(file, branch.members, true);
        is_first = false;
        auto const theirs = out.at(fields);
        auto const mine = out.at(members);
        auto why = cc::string();
        for (auto const& m : mine)
        {
            auto is_known = false;
            for (auto const& f : theirs)
                is_known = is_known || (f.name == m.name && f.type == m.type);
            if (!is_known && why.empty())
                why = cc::format("{} {}: {}, which its first branch does not", name, m.name, out.name_of(m.type));
        }
        if (why.empty() && mine.size() != theirs.size())
            why = cc::format("{} has {} fields in one branch and {} in another", name, theirs.size(), mine.size());
        if (!why.empty())
        {
            report(diagnostic_kind::invalid_target_text, file, file_of(file).at(branch.form).where,
                   cc::format("every branch of a @target_layout struct declares the same fields, and this one declares "
                              "{}",
                              why));
            continue;
        }
        // CHK-526: the record as the target lays it out: a run of @bits fields shares one word, and an @absent one is
        // none; each field the check judged is then placed by name
        auto placed = cc::vector<member_info>();
        auto by_name = cc::vector<layout_place>();
        auto word_bits = 32;
        for (auto const& m : out.at(members))
        {
            if (m.is_absent)
            {
                by_name.push_back({.member = -1});
                continue;
            }
            if (m.layout_bits == 0)
            {
                placed.push_back(m);
                word_bits = 32;
                by_name.push_back({.member = i32(placed.size()) - 1});
                continue;
            }
            if (word_bits + m.layout_bits > 32)
            {
                placed.push_back({.name = cc::format("sgl_bits{}", placed.size()), .type = m.type});
                word_bits = 0;
            }
            by_name.push_back({.member = i32(placed.size()) - 1, .offset = word_bits, .bits = m.layout_bits});
            word_bits += m.layout_bits;
        }
        for (auto const& f : theirs)
            for (auto k = isize(0); k < mine.size(); ++k)
                if (mine[k].name == f.name)
                    record.places.push_back(by_name[k]);
        auto const first = u32(out.members.size());
        for (auto& m : placed)
            out.members.push_back(cc::move(m));
        record.layout = type_id(out.types.size());
        out.types.push_back({
            .kind = type_kind::structure,
            .symbol = id,
            .members = {.first = first, .count = u32(placed.size())},
            .layout_of = type,
            .arguments = {.first = u32(out.type_lists.size()), .count = 0},
        });
        out.target_type_branches.push_back(cc::move(record));
    }
}

void checker::prepare_target_layouts()
{
    target_layouts.clear();
    unplaced_layouts.clear();
    for (auto i = isize(0); i < out.types.size(); ++i)
    {
        auto const type = type_id(i);
        if (!has_target_layout(type))
            continue;
        auto const chosen = chosen_type_branch(out, out.at(type).symbol, compiled_target);
        if (chosen < 0 || !is_valid(out.target_type_branches[chosen].layout))
        {
            // a description and a test write for no target, and judge the type by its first branch alone
            if (compiled_target.api != graphics_api::none)
                unplaced_layouts.push_back(type);
            continue;
        }
        target_layouts.push_back({type, out.target_type_branches[chosen].layout});
    }
    if (target_layouts.empty())
        return;
    // a buffer is the one type that holds one (CHK-522), and it is interned, so its twin is made here
    auto const count = out.types.size();
    auto const structs = target_layouts.size();
    for (auto i = isize(0); i < count; ++i)
    {
        // by value: interning below moves `types`
        auto const element = out.at(type_id(i)).element;
        auto const is_mut = out.at(type_id(i)).is_mut;
        if (out.at(type_id(i)).kind != type_kind::buffer)
            continue;
        for (auto k = isize(0); k < structs; ++k)
            if (element == target_layouts[k].first)
            {
                auto const to = buffer_type(target_layouts[k].second, is_mut);
                target_layouts.push_back({type_id(i), to});
                break;
            }
    }
}

void checker::place_layout_bindings(isize first)
{
    if (target_layouts.empty())
        return;
    for (auto i = first; i < out.symbols.size(); ++i)
    {
        if (out.symbols[i].kind != symbol_kind::binding || out.symbols[i].state != symbol_state::checked)
            continue;
        auto const members = out.bindings[out.symbols[i].info].members;
        for (auto k = u32(0); k < members.count; ++k)
            for (auto const& [from, to] : target_layouts)
                if (out.members[members.first + k].type == from)
                {
                    out.members.own(members.first + k).type = to;
                    break;
                }
    }
}

namespace
{
bool holds(cc::span<target_condition const> nodes, i32 at, compile_target const& target)
{
    auto const& n = nodes[at];
    switch (n.kind)
    {
    case target_condition::kind_t::api_is:
        return i32(target.api) == n.value;
    case target_condition::kind_t::language_is:
        return i32(target.language) == n.value;
    case target_condition::kind_t::has:
        return target.device_features.has(feature(n.value));
    case target_condition::kind_t::always:
        return true;
    case target_condition::kind_t::negated:
        return !holds(nodes, n.lhs, target);
    case target_condition::kind_t::both:
        return holds(nodes, n.lhs, target) && holds(nodes, n.rhs, target);
    case target_condition::kind_t::either:
        return holds(nodes, n.lhs, target) || holds(nodes, n.rhs, target);
    }
    return false;
}
} // namespace

i32 sgl::check::chosen_type_branch(checked_module const& m, symbol_id type_symbol, compile_target const& target)
{
    for (auto i = isize(0); i < m.target_type_branches.size(); ++i)
    {
        auto const& b = m.target_type_branches[i];
        if (b.type_symbol != type_symbol)
            continue;
        if (b.condition.empty() || holds(b.condition, i32(b.condition.size()) - 1, target))
            return i32(i);
    }
    return -1;
}
