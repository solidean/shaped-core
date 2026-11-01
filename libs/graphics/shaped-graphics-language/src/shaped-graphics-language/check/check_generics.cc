#include <clean-core/sequence/sequence.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>
#include <shaped-graphics-language/interpret/scalar.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// Generic functions and the prelude's generic structs (the spec's checking file, CHK-338 to CHK-341).
//
// A type parameter is opaque where it is declared: a generic body is checked once, over it.
// A call deduces what each parameter stands for, and inlining writes that in its place, so no emitter meets one.
// Bindings are pairs laid out flat: a type parameter, then what it stands for.

type_id checker::new_type_parameter(cc::string_view name, symbol_id owner)
{
    auto const id = type_id(out.types.size());
    out.types.push_back({.kind = type_kind::type_parameter, .symbol = owner, .spelled = cc::string(name)});
    return id;
}

bool checker::is_open(type_id type) const
{
    if (!is_valid(type) || type == checked_module::error_type)
        return false;
    auto const& t = out.at(type);
    switch (t.kind)
    {
    case type_kind::type_parameter:
        return true;
    case type_kind::structure:
        return t.is_template || (is_valid(t.generic) && is_open(t.element));
    case type_kind::array:
        return is_open(t.element);
    case type_kind::function:
    {
        if (is_open(t.element))
            return true;
        for (auto const& m : out.at(t.members))
            if (is_open(m.type))
                return true;
        return false;
    }
    default:
        return false;
    }
}

void checker::collect_type_parameters(type_id type, cc::vector<type_id>& into) const
{
    if (!is_open(type))
        return;
    auto const& t = out.at(type);
    if (t.kind == type_kind::type_parameter)
    {
        if (!into.values().any([&](type_id p) { return p == type; }))
            into.push_back(type);
        return;
    }
    if (t.kind == type_kind::function)
        for (auto const& m : out.at(t.members))
            collect_type_parameters(m.type, into);
    collect_type_parameters(t.element, into);
}

namespace
{
/// What `parameter` stands for in `bindings`, or `none` where it is not bound.
type_id bound_to(cc::span<type_id const> bindings, type_id parameter)
{
    for (auto i = isize(0); i + 1 < bindings.size(); i += 2)
        if (bindings[i] == parameter)
            return bindings[i + 1];
    return type_id::none;
}
} // namespace

namespace
{
/// The key of the instance of `generic` for `argument` in `checker::instances`.
u64 instance_key(type_id generic, type_id argument)
{
    return (u64(u32(index_of(generic))) << 32) | u64(u32(index_of(argument)));
}
} // namespace

type_id checker::existing_instance(type_id generic, type_id argument) const
{
    if (argument == out.at(generic).element)
        return generic;
    auto const* const found = instances.get_ptr(instance_key(generic, argument));
    return found != nullptr ? *found : type_id::none;
}

type_id checker::instance_of(type_id generic, type_id argument)
{
    if (auto const found = existing_instance(generic, argument); is_valid(found))
        return found;
    // by value: substituting a member may intern another instance, and `types` and `members` then move
    auto const templated = out.at(generic);
    type_id const bindings[] = {templated.element, argument};
    auto members = cc::vector<member_info>::create_copy_of(out.at(templated.members));
    for (auto& m : members)
        m.type = substitute(m.type, bindings);
    // a member's substitution may have interned this very instance through a nested mention of it
    if (auto const found = existing_instance(generic, argument); is_valid(found))
        return found;
    auto const id = type_id(out.types.size());
    out.types.push_back({
        .kind = type_kind::structure,
        .symbol = templated.symbol,
        .members = {.first = u32(out.members.size()), .count = u32(members.size())},
        .element = argument,
        .spelled = cc::format("{}[{}]", out.at(templated.symbol).name, out.name_of(argument)),
        .generic = generic,
    });
    instances[instance_key(generic, argument)] = id;
    out.members.push_back_range(members);
    return id;
}

type_id checker::substitute(type_id type, cc::span<type_id const> bindings)
{
    if (bindings.empty() || !is_open(type))
        return type;
    // by value: interning below moves `types`
    auto const t = out.at(type);
    switch (t.kind)
    {
    case type_kind::type_parameter:
    {
        auto const to = bound_to(bindings, type);
        return is_valid(to) ? to : type;
    }
    case type_kind::structure:
    {
        auto const generic = t.is_template ? type : t.generic;
        return instance_of(generic, substitute(t.element, bindings));
    }
    case type_kind::array:
        return array_type(substitute(t.element, bindings), t.count);
    case type_kind::function:
    {
        auto parameters = cc::vector<type_id>();
        for (auto const& m : out.at(t.members))
            parameters.push_back(m.type);
        for (auto& p : parameters)
            p = substitute(p, bindings);
        return function_type(parameters, substitute(t.element, bindings));
    }
    default:
        return type;
    }
}

type_id checker::substitute_existing(type_id type, cc::span<type_id const> bindings) const
{
    if (bindings.empty() || !is_open(type))
        return type;
    auto const& t = out.at(type);
    switch (t.kind)
    {
    case type_kind::type_parameter:
    {
        auto const to = bound_to(bindings, type);
        return is_valid(to) ? to : type;
    }
    case type_kind::structure:
    {
        auto const argument = substitute_existing(t.element, bindings);
        if (!is_valid(argument))
            return type_id::none;
        return existing_instance(t.is_template ? type : t.generic, argument);
    }
    case type_kind::array:
    {
        auto const element = substitute_existing(t.element, bindings);
        for (auto i = isize(0); is_valid(element) && i < out.types.size(); ++i)
            if (out.types[i].kind == type_kind::array && out.types[i].element == element && out.types[i].count == t.count)
                return type_id(i);
        return type_id::none;
    }
    default:
        // a function type stands for a closure, which is inlined and never a value of the tree
        return type;
    }
}

bool checker::unify(type_id pattern, type_id actual, cc::vector<type_id>& bindings) const
{
    if (pattern == actual)
        return true;
    if (!is_open(pattern) || !is_valid(actual) || actual == checked_module::error_type)
        return false;
    auto const& p = out.at(pattern);
    auto const& a = out.at(actual);
    switch (p.kind)
    {
    case type_kind::type_parameter:
    {
        if (auto const to = bound_to(bindings, pattern); is_valid(to))
            return to == actual;
        bindings.push_back(pattern);
        bindings.push_back(actual);
        return true;
    }
    case type_kind::structure:
    {
        if (a.kind != type_kind::structure)
            return false;
        auto const pattern_generic = p.is_template ? pattern : p.generic;
        auto const actual_generic = a.is_template ? actual : a.generic;
        return pattern_generic == actual_generic && unify(p.element, a.element, bindings);
    }
    case type_kind::array:
        return a.kind == type_kind::array && a.count == p.count && unify(p.element, a.element, bindings);
    case type_kind::function:
    {
        if (a.kind != type_kind::function || a.members.count != p.members.count)
            return false;
        auto const ps = out.at(p.members);
        auto const as = out.at(a.members);
        for (auto i = isize(0); i < ps.size(); ++i)
            if (!unify(ps[i].type, as[i].type, bindings))
                return false;
        return unify(p.element, a.element, bindings);
    }
    default:
        return false;
    }
}

void checker::instantiate_generics()
{
    // CHK-340: every instance an inlined body will name exists before flattening, which only looks them up.
    // Starting from each call whose arguments are known, a generic callee's calls are followed with the arguments
    // theirs stand for there; recursion is refused, so this ends.
    auto pending = cc::vector<cc::vector<type_id>>();
    auto done = cc::vector<cc::vector<type_id>>();
    auto const is_closed = [&](cc::span<type_id const> bindings)
    {
        for (auto i = isize(1); i < bindings.size(); i += 2)
            if (is_open(bindings[i]))
                return false;
        return true;
    };
    for (auto const& r : out.call_records)
        if (auto const b = out.at(r.type_arguments); !b.empty() && is_closed(b))
            pending.push_back(cc::vector<type_id>::create_copy_of(b));

    while (!pending.empty())
    {
        auto bindings = cc::move(pending.back());
        pending.remove_back();
        if (done.values().any([&](cc::vector<type_id> const& d) { return ast::impl::is_equal(d, bindings); }))
            continue;
        // every open type of the module, as these arguments make it
        auto const count = out.types.size();
        for (auto i = isize(0); i < count; ++i)
            if (is_open(type_id(i)))
                (void)substitute(type_id(i), bindings);
        for (auto const& r : out.call_records)
        {
            auto const inner = cc::vector<type_id>::create_copy_of(out.at(r.type_arguments));
            if (inner.empty() || is_closed(inner))
                continue;
            auto composed = inner;
            for (auto i = isize(1); i < composed.size(); i += 2)
                composed[i] = substitute(composed[i], bindings);
            if (is_closed(composed))
                pending.push_back(cc::move(composed));
        }
        done.push_back(cc::move(bindings));
    }
}

type_id checker::generic_named(i32 file, ast::expr_id expr)
{
    auto const* const n = ast::is_valid(expr) ? ast_of(file).at(expr).node.try_as<ast::name>() : nullptr;
    if (n == nullptr)
        return type_id::none;
    auto const* const found = seen_from(file, text_of(file, n->where));
    if (found == nullptr || found->empty() || out.at(found->front()).kind != symbol_kind::structure)
        return type_id::none;
    auto const* const d
        = ast_of(out.at(found->front()).file).at(out.at(found->front()).declaration).node.try_as<ast::struct_decl>();
    if (d == nullptr || d->type_parameters.empty())
        return type_id::none;
    if (demand(found->front(), file, n->where) != symbol_state::checked)
        return type_id::none;
    set_target(file, expr, {.kind = target_kind::symbol, .symbol = found->front()});
    auto const type = out.at(found->front()).type;
    return out.at(type).is_template ? type : type_id::none;
}

i32 checker::instantiate(symbol_id function, cc::span<type_id const> bindings, i32 file, source_span where)
{
    auto const info = out.at(function).info;
    if (generic_passes > 0 || info < 0 || out.at(function).state != symbol_state::checked)
        return -1;
    auto const parameters = cc::vector<type_id>::create_copy_of(out.at(out.functions[info].type_parameters));
    if (parameters.empty())
        return -1;
    // in the order the function declares its parameters, so two calls that bound them in another order meet
    auto key = cc::vector<type_id>();
    for (auto const p : parameters)
    {
        auto const to = bound_to(bindings, p);
        if (!is_valid(to) || to == checked_module::error_type || is_open(to))
            return -1;
        key.push_back(p);
        key.push_back(to);
    }
    if (auto const found = existing_function_instance(function, key); found >= 0)
        return found;
    // recursion was or will be reported where the calls loop, and an instance of it would never end
    for (auto const& at : instantiating)
        if (out.function_instances[at.instance].function == function)
            return -1;

    auto const id = i32(out.function_instances.size());
    out.function_instances.push_back(
        {.function = function, .bindings = {.first = u32(out.type_lists.size()), .count = u32(key.size())}});
    out.type_lists.push_back_range(key);

    // the body names each type parameter, which now stands for its argument; the names are copied, since the type
    // table they are spelled in grows while the body is checked
    auto names = cc::vector<cc::string>();
    for (auto const p : parameters)
        names.push_back(out.at(p).spelled);
    auto const outer_names = cc::move(type_parameter_names);
    type_parameter_names = {};
    for (auto k = isize(0); k < parameters.size(); ++k)
        type_parameter_names.push_back({names[k], key[2 * k + 1]});
    auto const outer_instance = current_instance;
    auto const outer_expected = expected_result;
    current_instance = id;
    expected_result = type_id::none;
    instantiating.push_back({.instance = id, .file = file, .where = where});
    check_body_of(function, id);
    instantiating.remove_back();
    expected_result = outer_expected;
    current_instance = outer_instance;
    type_parameter_names = cc::move(outer_names);
    return id;
}

void checker::warn_uninstantiated_generics(isize first)
{
    for (auto i = first; i < out.symbols.size(); ++i)
    {
        auto const& s = out.symbols[i];
        if (s.kind != symbol_kind::function || s.state != symbol_state::checked || s.info < 0
            || s.file != program_file() || out.functions[s.info].type_parameters.empty())
            continue;
        auto is_instantiated = false;
        for (auto const& instance : out.function_instances)
            is_instantiated = is_instantiated || instance.function == symbol_id(i);
        auto const* const f = ast_of(s.file).at(s.declaration).node.try_as<ast::fun_decl>();
        if (!is_instantiated && f != nullptr)
            report(diagnostic_kind::generic_not_instantiated, s.file, f->name,
                   cc::format("nothing instantiates {}, so only its names are checked", s.name));
    }
}

i32 checker::existing_function_instance(symbol_id function, cc::span<type_id const> bindings) const
{
    if (bindings.empty())
        return -1;
    for (auto i = isize(0); i < out.function_instances.size(); ++i)
    {
        auto const& instance = out.function_instances[i];
        if (instance.function != function)
            continue;
        auto const pairs = out.at(instance.bindings);
        auto is_match = true;
        for (auto k = isize(0); is_match && k + 1 < pairs.size(); k += 2)
            is_match = bound_to(bindings, pairs[k]) == pairs[k + 1];
        if (is_match)
            return i32(i);
    }
    return -1;
}

type_id checker::constant_type(type_id element, u32 bits)
{
    // every NaN is one value, so two spellings of it name one instance
    if (out.name_of(element) == "float" && (bits & 0x7f800000u) == 0x7f800000u && (bits & 0x007fffffu) != 0)
        bits = 0x7fc00000u;
    for (auto i = isize(0); i < out.types.size(); ++i)
        if (out.types[i].kind == type_kind::constant && out.types[i].element == element && u32(out.types[i].count) == bits)
            return type_id(i);
    auto spelled = cc::string();
    if (out.name_of(element) == "bool")
        spelled = bits != 0 ? "true" : "false";
    else if (out.name_of(element) == "float")
        spelled = cc::format("{}", scalar{.kind = value_kind::scalar_float, .bits = bits}.as_float());
    else if (out.name_of(element) == "uint")
        spelled = cc::format("{}u", bits);
    else
        spelled = cc::format("{}", i32(bits));
    auto const id = type_id(out.types.size());
    out.types.push_back({.kind = type_kind::constant, .element = element, .count = i32(bits), .spelled = spelled});
    return id;
}

type_id checker::stated_argument_of(function_scope& scope, ast::expr_id expr)
{
    auto const file = scope.file;
    auto const& node = ast_of(file).at(expr).node;
    // a name is a type unless it is a value: a local, a const or a case; anything but a name or a type form is a value
    auto is_value = !node.is<ast::name>() && !node.is<ast::index>() && !node.is<ast::member>();
    if (auto const* const n = node.try_as<ast::name>())
    {
        auto const text = text_of(file, n->where);
        auto const* const found = seen_from(file, text);
        is_value = scope.find_local(text) != nullptr
                || (found != nullptr && !found->empty() && out.at(found->front()).kind == symbol_kind::constant);
    }
    if (auto const* const m = node.try_as<ast::member>())
        is_value = generic_named(file, m->object) == type_id::none && module_named(file, m->object, &scope) < 0;
    if (!is_value)
        return resolve_value_type(file, expr, &scope);
    auto const type = check_expr(scope, expr);
    if (type == checked_module::error_type)
        return checked_module::error_type;
    auto const is_scalar = out.name_of(type) == "int" || out.name_of(type) == "uint" || out.name_of(type) == "bool"
                        || out.name_of(type) == "float";
    auto const evaluated = is_scalar ? evaluate_const(scope.function, file, expr, type) : const_evaluation{};
    if (!evaluated.is_ok || evaluated.leaves.size() != 1)
    {
        report(diagnostic_kind::invalid_constant_argument, file, span_of(file, expr),
               "a generic argument is a type, or a constant int, uint, bool or float");
        return checked_module::error_type;
    }
    return constant_type(type, evaluated.leaves[0].bits);
}
