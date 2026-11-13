#include <clean-core/sequence/sequence.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>
#include <shaped-graphics-language/check/resources.hh>
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
    {
        if (t.is_template)
            return true;
        if (!is_valid(t.generic))
            return false;
        for (auto const a : out.at(t.arguments))
            if (is_open(a))
                return true;
        return false;
    }
    case type_kind::array:
        return is_valid(t.count_parameter) || is_open(t.element);
    case type_kind::image:
        return is_valid(t.count_parameter);
    case type_kind::scalar_of:
    case type_kind::tangent_of:
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
    if (t.kind == type_kind::structure)
    {
        for (auto const a : out.at(t.arguments))
            collect_type_parameters(a, into);
        return;
    }
    if (t.kind == type_kind::array || t.kind == type_kind::image)
        collect_type_parameters(t.count_parameter, into);
    // `scalar_of[V]` names `V`, whose element is how it is collected below
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

/// The key of the instance of `generic` for `arguments` in `checker::instances`, which a lookup confirms.
u64 instance_key(type_id generic, cc::span<type_id const> arguments)
{
    auto key = u64(0xcbf29ce484222325);
    auto const mix = [&](type_id t)
    {
        key ^= u64(u32(index_of(t)));
        key *= u64(0x100000001b3);
    };
    mix(generic);
    for (auto const a : arguments)
        mix(a);
    return key;
}

bool is_same_list(cc::span<type_id const> a, cc::span<type_id const> b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = isize(0); i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}
} // namespace

type_id checker::existing_instance(type_id generic, cc::span<type_id const> arguments) const
{
    // the template itself, where each argument is its own parameter
    if (is_same_list(out.at(out.at(generic).arguments), arguments))
        return generic;
    auto const* const found = instances.get_ptr(instance_key(generic, arguments));
    if (found == nullptr)
        return type_id::none;
    if (is_same_list(out.at(out.at(*found).arguments), arguments))
        return *found;
    // two argument lists met on one key: the table is the truth
    for (auto i = isize(0); i < out.types.size(); ++i)
        if (out.types[i].kind == type_kind::structure && out.types[i].generic == generic
            && is_same_list(out.at(out.types[i].arguments), arguments))
            return type_id(i);
    return type_id::none;
}

type_id checker::instance_of(type_id generic, cc::span<type_id const> arguments_in)
{
    // by value: substituting a member may intern another instance, and `types`, `members` and `type_lists` then move
    auto const arguments = cc::vector<type_id>::create_copy_of(arguments_in);
    if (auto const found = existing_instance(generic, arguments); is_valid(found))
        return found;
    auto const templated = out.at(generic);
    auto const parameters = cc::vector<type_id>::create_copy_of(out.at(templated.arguments));
    if (parameters.size() != arguments.size())
        return checked_module::error_type;
    auto bindings = cc::vector<type_id>();
    for (auto i = isize(0); i < parameters.size(); ++i)
    {
        bindings.push_back(parameters[i]);
        bindings.push_back(arguments[i]);
    }
    auto members = cc::vector<member_info>::create_copy_of(out.at(templated.members));
    for (auto& m : members)
        m.type = substitute(m.type, bindings);
    // a member's substitution may have interned this very instance through a nested mention of it
    if (auto const found = existing_instance(generic, arguments); is_valid(found))
        return found;
    auto spelled = cc::format("{}[", out.at(templated.symbol).name);
    for (auto i = isize(0); i < arguments.size(); ++i)
        spelled.appendf("{}{}", i == 0 ? "" : ", ", out.name_of(arguments[i]));
    spelled += "]";
    auto const id = type_id(out.types.size());
    out.types.push_back({
        .kind = type_kind::structure,
        .symbol = templated.symbol,
        .members = {.first = u32(out.members.size()), .count = u32(members.size())},
        .element = arguments[0],
        .spelled = cc::move(spelled),
        .generic = generic,
        .arguments = {.first = u32(out.type_lists.size()), .count = u32(arguments.size())},
    });
    out.type_lists.push_back_range(arguments);
    instances[instance_key(generic, arguments)] = id;
    out.members.push_back_range(members);
    return id;
}

type_id checker::symbolic_array_type(type_id element, type_id count_parameter)
{
    return resource_type({.kind = type_kind::array,
                          .element = element,
                          .count_parameter = count_parameter,
                          .spelled = cc::format("{}[{}]", out.name_of(element), out.at(count_parameter).spelled)});
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
        auto arguments = cc::vector<type_id>::create_copy_of(out.at(t.arguments));
        for (auto& a : arguments)
            a = substitute(a, bindings);
        return instance_of(t.is_template ? type : t.generic, arguments);
    }
    case type_kind::array:
    {
        auto const element = substitute(t.element, bindings);
        if (!is_valid(t.count_parameter))
            return array_type(element, t.count);
        // CHK-339: `T[n]` with `n` bound to a constant is an array of that length
        auto const count = bound_to(bindings, t.count_parameter);
        if (is_valid(count) && out.at(count).kind == type_kind::constant)
            return array_type(element, out.at(count).count);
        return symbolic_array_type(element, is_valid(count) ? count : t.count_parameter);
    }
    case type_kind::scalar_of:
        // CHK-423: the scalar of what `V` stands for, or the error type where that is no vector
        return scalar_of_type(substitute(t.element, bindings));
    case type_kind::tangent_of:
        // DIFF-7: the tangent of what `T` stands for, which a struct mints once
        return tangent_type(substitute(t.element, bindings));
    case type_kind::image:
    {
        // CHK-339: `image_2d[f]` with `f` bound to a case is the image of that format
        auto const format = bound_to(bindings, t.count_parameter);
        if (!is_valid(format) || out.at(format).kind != type_kind::constant)
            return type;
        auto image = t;
        image.count_parameter = type_id::none;
        image.format = find_image_format(out.at(out.at(out.at(format).element).cases)[out.at(format).count].name);
        return resource_type(cc::move(image));
    }
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
        auto arguments = cc::vector<type_id>();
        for (auto const a : out.at(t.arguments))
        {
            arguments.push_back(substitute_existing(a, bindings));
            if (!is_valid(arguments.back()))
                return type_id::none;
        }
        return existing_instance(t.is_template ? type : t.generic, arguments);
    }
    case type_kind::scalar_of:
    {
        auto const of = substitute_existing(t.element, bindings);
        return !is_valid(of) || is_open(of) ? type_id::none : scalar_of_closed(of);
    }
    case type_kind::tangent_of:
    {
        auto const of = substitute_existing(t.element, bindings);
        return !is_valid(of) || is_open(of) ? type_id::none : tangent_type_existing(of);
    }
    case type_kind::array:
    {
        auto const element = substitute_existing(t.element, bindings);
        auto count = t.count;
        if (is_valid(t.count_parameter))
        {
            auto const bound = bound_to(bindings, t.count_parameter);
            if (!is_valid(bound) || out.at(bound).kind != type_kind::constant)
                return type_id::none;
            count = out.at(bound).count;
        }
        for (auto i = isize(0); is_valid(element) && i < out.types.size(); ++i)
            if (out.types[i].kind == type_kind::array && out.types[i].element == element && out.types[i].count == count
                && !is_valid(out.types[i].count_parameter))
                return type_id(i);
        return type_id::none;
    }
    default:
        // a function type stands for a closure, which is inlined and never a value of the tree
        return type;
    }
}

bool checker::unify(type_id pattern, type_id actual, cc::vector<type_id>& bindings)
{
    if (pattern == actual)
        return true;
    if (!is_open(pattern) || !is_valid(actual) || actual == checked_module::error_type)
        return false;
    // by value: binding an array's length interns a constant, and `types` then moves
    auto const p = out.at(pattern);
    auto const a = out.at(actual);
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
        if (pattern_generic != actual_generic || p.arguments.count != a.arguments.count)
            return false;
        for (auto i = u32(0); i < p.arguments.count; ++i)
            if (!unify(out.type_lists[p.arguments.first + i], out.type_lists[a.arguments.first + i], bindings))
                return false;
        return true;
    }
    case type_kind::array:
    {
        if (a.kind != type_kind::array)
            return false;
        // CHK-339: an array's length binds a value parameter its count names, as a constant of that parameter's type
        if (is_valid(p.count_parameter) && !is_valid(a.count_parameter) && a.count > 0)
        {
            auto const length = constant_type(out.at(p.count_parameter).element, u32(a.count));
            if (!unify(p.count_parameter, length, bindings))
                return false;
        }
        else if (p.count_parameter != a.count_parameter || a.count != p.count)
            return false;
        return unify(p.element, a.element, bindings);
    }
    case type_kind::image:
    {
        // CHK-339: an image's format binds the `pixel_format` value parameter a pattern's format is, as its case
        if (a.kind != type_kind::image || a.format < 0 || a.shape != p.shape || a.access != p.access
            || a.is_atomic != p.is_atomic)
            return false;
        auto const enumeration = out.at(p.count_parameter).element;
        auto const index = case_index_of(enumeration, k_image_formats[a.format].name);
        return index >= 0 && unify(p.count_parameter, constant_type(enumeration, u32(index)), bindings);
    }
    case type_kind::function:
    {
        if (a.kind != type_kind::function || a.members.count != p.members.count)
            return false;
        for (auto i = u32(0); i < p.members.count; ++i)
            if (!unify(out.members[p.members.first + i].type, out.members[a.members.first + i].type, bindings))
                return false;
        return unify(p.element, a.element, bindings);
    }
    default:
        return false;
    }
}

bool checker::is_closed(cc::span<type_id const> bindings) const
{
    for (auto i = isize(1); i < bindings.size(); i += 2)
        if (is_open(bindings[i]))
            return false;
    return true;
}

void checker::instantiate_generics(isize first_call, isize first_type)
{
    // CHK-340: every instance an inlined body will name exists before flattening, which only looks them up.
    // Starting from each call whose arguments are known, a generic callee's calls are followed with the arguments
    // theirs stand for there; recursion is refused, so this ends.
    auto pending = cc::vector<cc::vector<type_id>>();
    auto done = cc::vector<cc::vector<type_id>>();
    for (auto i = first_call; i < out.call_records.size(); ++i)
        if (auto const b = out.at(out.call_records[i].type_arguments); !b.empty() && is_closed(b))
            pending.push_back(cc::vector<type_id>::create_copy_of(b));
    // the calls with open arguments, the layers below's first, in the order they were recorded
    auto const open_calls = [&](auto&& each)
    {
        for (auto const i : open_calls_below)
            each(out.call_records[i]);
        for (auto i = first_call; i < out.call_records.size(); ++i)
            each(out.call_records[i]);
    };

    while (!pending.empty())
    {
        auto bindings = cc::move(pending.back());
        pending.remove_back();
        if (done.values().any([&](cc::vector<type_id> const& d) { return ast::impl::is_equal(d, bindings); }))
            continue;
        // every open type of the module, as these arguments make it, in the order the types were made
        for (auto const t : open_types_below)
            (void)substitute(t, bindings);
        auto const count = out.types.size();
        for (auto i = first_type; i < count; ++i)
            if (is_open(type_id(i)))
                (void)substitute(type_id(i), bindings);
        open_calls(
            [&](call_record const& r)
            {
                auto const inner = cc::vector<type_id>::create_copy_of(out.at(r.type_arguments));
                if (inner.empty() || is_closed(inner))
                    return;
                auto composed = inner;
                for (auto i = isize(1); i < composed.size(); i += 2)
                    composed[i] = substitute(composed[i], bindings);
                if (is_closed(composed))
                    pending.push_back(cc::move(composed));
            });
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
    note_body_read(function);
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
    if (out.at(element).kind == type_kind::enumeration)
        spelled = cc::format("{}.{}", out.name_of(element), out.at(out.at(element).cases)[isize(bits)].name);
    else if (out.name_of(element) == "bool")
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
        // a value parameter in scope is handed on, and in an instance the constant it stands for
        for (auto i = type_parameter_names.size(); i > 0; --i)
            if (type_parameter_names[i - 1].first == text)
            {
                auto const stands_for = type_parameter_names[i - 1].second;
                if (out.at(stands_for).kind == type_kind::constant || is_valid(out.at(stands_for).element))
                    return stands_for;
                break;
            }
        auto const* const found = seen_from(file, text);
        is_value = scope.find_local(text) != nullptr
                || (found != nullptr && !found->empty() && out.at(found->front()).kind == symbol_kind::constant);
    }
    if (auto const* const m = node.try_as<ast::member>())
        is_value = generic_named(file, m->object) == type_id::none && module_named(file, m->object, &scope) < 0;
    if (!is_value)
        return resolve_value_type(file, expr, &scope);
    // the call has not chosen the parameter that would say which enum a leading dot's case is of
    if (node.is<ast::leading_dot>())
    {
        report(diagnostic_kind::invalid_constant_argument, file, span_of(file, expr),
               "a stated generic argument that is an enum case names its enum, as in `mode.add`");
        return checked_module::error_type;
    }
    auto const type = check_expr(scope, expr);
    if (type == checked_module::error_type)
        return checked_module::error_type;
    if (out.at(type).kind == type_kind::enumeration)
    {
        auto const index = enum_case_argument(file, expr, type, &scope);
        if (index < 0)
        {
            report(diagnostic_kind::invalid_constant_argument, file, span_of(file, expr),
                   "an enum generic argument is a case, `mode.add`, or a const holding one");
            return checked_module::error_type;
        }
        return constant_type(type, u32(index));
    }
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

type_id checker::type_argument_of(i32 file, ast::expr_id expr, function_scope const* scope, type_id parameter)
{
    auto const wants_value = is_valid(out.at(parameter).element);
    auto const where = span_of(file, expr);
    // a parameter in scope passes through, the template's own making the template itself
    if (auto const* const n = ast_of(file).at(expr).node.try_as<ast::name>())
        for (auto i = type_parameter_names.size(); i > 0; --i)
            if (type_parameter_names[i - 1].first == text_of(file, n->where))
            {
                auto const stands_for = type_parameter_names[i - 1].second;
                auto const& s = out.at(stands_for);
                auto const is_value = s.kind == type_kind::constant || is_valid(s.element);
                if (is_value != wants_value || (is_value && s.element != out.at(parameter).element))
                    break;
                return stands_for;
            }
    if (!wants_value && ast_of(file).at(expr).node.is<ast::literal>())
    {
        report(diagnostic_kind::wrong_kind_of_name, file, where,
               cc::format("{} is a type parameter, which takes a type", out.at(parameter).spelled));
        return checked_module::error_type;
    }
    if (!wants_value)
        return resolve_value_type(file, expr, scope);
    if (auto const enumeration = out.at(parameter).element; out.at(enumeration).kind == type_kind::enumeration)
    {
        auto const index = enum_case_argument(file, expr, enumeration, scope);
        if (index < 0)
        {
            report(diagnostic_kind::invalid_constant_argument, file, where,
                   cc::format("{} is a value parameter of {}, which takes one of its cases", out.at(parameter).spelled,
                              out.name_of(enumeration)));
            return checked_module::error_type;
        }
        return constant_type(enumeration, u32(index));
    }
    // a number, a folded one such as `-1.5` among them, or a const, of a value the parameter's type holds
    auto const element = out.at(parameter).element;
    auto const type_name = out.name_of(element);
    auto bits = cc::optional<u32>();
    if (auto const n = number_of(file, expr); n.is_number)
    {
        if (type_name == "float")
            bits = scalar::of(f32(n.is_integer ? f64(n.integer) : n.real)).bits;
        else if (n.is_integer && type_name == "int" && n.integer >= -2147483648 && n.integer <= 2147483647)
            bits = u32(i32(n.integer));
        else if (n.is_integer && type_name == "uint" && n.integer >= 0 && n.integer <= 4294967295)
            bits = u32(n.integer);
    }
    else if (auto const* const literal = ast_of(file).at(expr).node.try_as<ast::literal>();
             literal != nullptr && type_name == "uint")
    {
        // `2u`, which names its type by its suffix
        auto const text = text_of(file, span_of(file, expr));
        auto const digits = text.ends_with("u") ? text.subview({.offset = 0, .size = text.size() - 1}) : text;
        if (classify_number(digits) == number_class::plain_integer)
            if (auto const value = parse_plain_integer(digits); value.has_value() && value.value() >= 0)
                bits = u32(value.value());
    }
    else if (type_name == "int")
    {
        if (auto const count = constant_count(file, expr, scope); count.has_value())
            bits = u32(count.value());
    }
    else if (auto const* const n = ast_of(file).at(expr).node.try_as<ast::name>())
    {
        auto const* const found = seen_from(file, text_of(file, n->where));
        auto const id = found != nullptr && !found->empty() ? found->front() : symbol_id::none;
        if (is_valid(id) && out.at(id).kind == symbol_kind::constant
            && demand(id, file, span_of(file, expr)) == symbol_state::checked)
        {
            set_target(file, expr, {.kind = target_kind::symbol, .symbol = id});
            auto const& c = out.constants[out.at(id).info];
            if (type_name == "float" && c.kind == constant_kind::real)
                bits = scalar::of(f32(c.real)).bits;
            else if (type_name == "uint" && c.kind == constant_kind::integer && c.is_unsigned)
                bits = u32(c.integer);
        }
    }
    if (!bits.has_value())
    {
        report(diagnostic_kind::invalid_constant_argument, file, where,
               cc::format("{} is a value parameter of {}, which takes a literal, a const or a parameter of it",
                          out.at(parameter).spelled, type_name));
        return checked_module::error_type;
    }
    return constant_type(element, bits.value());
}

i32 checker::enum_case_argument(i32 file, ast::expr_id expr, type_id enumeration, function_scope const* scope)
{
    auto const& node = ast_of(file).at(expr).node;
    if (auto const* const dot = node.try_as<ast::leading_dot>())
        return case_index_of(enumeration, text_of(file, dot->name));
    if (auto const* const m = node.try_as<ast::member>())
    {
        auto const* const object = ast_of(file).at(m->object).node.try_as<ast::name>();
        return object != nullptr && text_of(file, object->where) == out.name_of(enumeration)
                 ? case_index_of(enumeration, text_of(file, m->name))
                 : -1;
    }
    auto const* const n = node.try_as<ast::name>();
    if (n == nullptr || (scope != nullptr && scope->find_local(text_of(file, n->where)) != nullptr))
        return -1;
    auto const* const found = seen_from(file, text_of(file, n->where));
    if (found == nullptr || found->empty() || out.at(found->front()).kind != symbol_kind::constant
        || demand(found->front(), file, span_of(file, expr)) != symbol_state::checked)
        return -1;
    auto const& c = out.constants[out.at(found->front()).info];
    return c.kind == constant_kind::enum_case && c.type == enumeration ? c.case_index : -1;
}

type_id checker::scalar_of_closed(type_id of) const
{
    if (!is_valid(of) || of == checked_module::error_type)
        return checked_module::error_type;
    // a builtin of more than one leaf is a vector or a matrix, whose leaves are its scalar
    if (auto const* const record = out.builtin_type_of(of))
    {
        if (record->leaf_count <= 1)
            return checked_module::error_type;
        switch (record->leaf_kind)
        {
        case value_kind::scalar_float:
            return prelude_type("float");
        case value_kind::scalar_half:
            return prelude_type("half");
        case value_kind::scalar_int:
            return prelude_type("int");
        case value_kind::scalar_uint:
            return prelude_type("uint");
        case value_kind::scalar_short:
            return prelude_type("short");
        case value_kind::scalar_ushort:
            return prelude_type("ushort");
        case value_kind::scalar_long:
            return prelude_type("long");
        case value_kind::scalar_ulong:
            return prelude_type("ulong");
        case value_kind::scalar_double:
            return prelude_type("double");
        case value_kind::boolean:
            return prelude_type("bool");
        default:
            return checked_module::error_type;
        }
    }
    // a `@swizzle` struct of one scalar, `vec3` among them, is a vector of it
    auto const& t = out.at(of);
    if (t.kind != type_kind::structure || !t.has_swizzles || t.members.count == 0)
        return checked_module::error_type;
    auto const members = out.at(t.members);
    for (auto const& m : members)
        if (m.type != members[0].type)
            return checked_module::error_type;
    auto const* const scalar = out.builtin_type_of(members[0].type);
    return scalar != nullptr && scalar->leaf_count == 1 ? members[0].type : checked_module::error_type;
}

type_id checker::scalar_of_type(type_id of)
{
    if (!is_open(of))
        return scalar_of_closed(of);
    for (auto i = isize(0); i < out.types.size(); ++i)
        if (out.types[i].kind == type_kind::scalar_of && out.types[i].element == of)
            return type_id(i);
    auto const id = type_id(out.types.size());
    out.types.push_back(
        {.kind = type_kind::scalar_of, .element = of, .spelled = cc::format("scalar_of[{}]", out.name_of(of))});
    return id;
}
