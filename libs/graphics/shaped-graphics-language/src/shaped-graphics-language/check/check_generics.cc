#include <clean-core/sequence/sequence.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

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
        if (!cc::sequence{into}.any([&](type_id p) { return p == type; }))
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

type_id checker::existing_instance(type_id generic, type_id argument) const
{
    if (argument == out.at(generic).element)
        return generic;
    for (auto i = isize(0); i < out.types.size(); ++i)
        if (out.types[i].generic == generic && out.types[i].element == argument)
            return type_id(i);
    return type_id::none;
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
        if (cc::sequence{done}.any([&](cc::vector<type_id> const& d) { return ast::impl::is_equal(d, bindings); }))
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
    auto const* const found = names_seen_from(file).get_ptr(text_of(file, n->where));
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
