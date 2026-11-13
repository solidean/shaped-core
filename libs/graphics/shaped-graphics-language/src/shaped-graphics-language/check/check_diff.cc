#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

// PROTOTYPE of libs/graphics/shaped-graphics-language/docs/spec/incubator/autodiff.md.
// What `fwd_diff(f, …)` and `bwd_diff(f, …)` take depends on `f`, which no generic declaration can say: a parameter
// of no differentiable type, or one marked `@no_diff`, takes no tangent, and a position's tangent is a direction.
// So a call that names a function is resolved against a concrete overload written here for that function.

namespace sgl::check::impl
{
bool checker::is_differentiable_type(type_id type) const
{
    if (!is_valid(type) || type == checked_module::error_type || type == checked_module::void_type)
        return false;
    if (auto const* const r = out.builtin_type_of(type))
        return r->leaf_kind == value_kind::scalar_float || r->leaf_kind == value_kind::scalar_half;
    auto const& info = out.at(type);
    if (info.kind == type_kind::array)
        return is_differentiable_type(info.element);
    if (info.kind != type_kind::structure)
        return false;
    for (auto const& member : out.at(info.members))
        if (is_differentiable_type(member.type))
            return true;
    return false;
}

type_id checker::tangent_type_existing(type_id type) const
{
    if (out.builtin_type_of(type) != nullptr)
    {
        auto const name = out.name_of(type);
        if (name == "pos3")
            return prelude_type("vec3");
        if (name == builtins::k_hpos4)
            return prelude_type("float4");
        return type;
    }
    if (out.at(type).kind == type_kind::structure && !is_valid(out.at(type).tangent_of))
    {
        auto const* const known = tangent_types.get_ptr(i32(type));
        return known != nullptr ? *known : type_id::none;
    }
    return out.at(type).kind == type_kind::array ? type_id::none : type;
}

type_id checker::tangent_type(type_id type)
{
    if (!is_valid(type) || type == checked_module::error_type || type == checked_module::void_type)
        return type;
    // DIFF-7: a type parameter's tangent is what each instance makes of it
    if (is_open(type))
    {
        for (auto i = isize(0); i < out.types.size(); ++i)
            if (out.types[i].kind == type_kind::tangent_of && out.types[i].element == type)
                return type_id(i);
        auto const id = type_id(out.types.size());
        out.types.push_back(
            {.kind = type_kind::tangent_of, .element = type, .spelled = cc::format("tangent[{}]", out.name_of(type))});
        return id;
    }
    if (out.builtin_type_of(type) != nullptr)
    {
        auto const name = out.name_of(type);
        if (name == "pos3")
            return prelude_type("vec3");
        if (name == builtins::k_hpos4)
            return prelude_type("float4");
        return type;
    }
    // by value: minting below moves `types`
    auto const info = out.at(type);
    if (info.kind == type_kind::array)
    {
        auto const element = tangent_type(info.element);
        return element == info.element || is_valid(info.count_parameter) ? type : array_type(element, info.count);
    }
    if (info.kind != type_kind::structure || is_open(type) || is_valid(info.tangent_of))
        return type;
    if (auto const* const known = tangent_types.get_ptr(i32(type)))
        return *known;

    auto members = cc::vector<member_info>();
    auto is_own = true;
    for (auto const& m : cc::vector<member_info>::create_copy_of(out.at(info.members)))
    {
        if (!is_differentiable_type(m.type))
        {
            is_own = false;
            continue;
        }
        auto const t = tangent_type(m.type);
        is_own = is_own && t == m.type;
        members.push_back({.name = m.name, .type = t});
    }
    if (is_own)
    {
        tangent_types[i32(type)] = type;
        return type;
    }
    // its own type, which no declaration names: the planner spells it from `spelled`, and nothing constructs it but
    // `tangent[S]()`, so it carries none of the declared fields' attributes or defaults
    auto const id = type_id(out.types.size());
    out.types.push_back({
        .kind = type_kind::structure,
        .symbol = info.symbol,
        .members = {.first = u32(out.members.size()), .count = u32(members.size())},
        .spelled = cc::format("tangent[{}]", out.name_of(type)),
        .tangent_of = type,
    });
    out.members.push_back_range(members);
    tangent_types[i32(type)] = id;
    return id;
}

type_id checker::resolve_tangent(i32 file, ast::expr_id expr, ast::index const& node, function_scope const* scope)
{
    auto const arguments = ast_of(file).at(node.arguments);
    if (arguments.size() != 1 || !arguments[0].name.empty() || arguments[0].is_splat)
    {
        report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, expr), "tangent takes one type: `tangent[pos3]`");
        return checked_module::error_type;
    }
    auto const of = resolve_value_type(file, arguments[0].value, scope);
    if (of == checked_module::error_type)
        return of;
    if (!is_open(of) && !is_differentiable_type(of))
    {
        report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, expr),
               cc::format("{} holds no float, so nothing of it changes and it has no tangent", out.name_of(of)));
        return checked_module::error_type;
    }
    return tangent_type(of);
}

void checker::declare_tangent(i32 file, type_id primal, ast::attribute const& declared, bool is_generic)
{
    auto const arguments = ast_of(file).at(declared.arguments);
    if (arguments.size() != 1 || !arguments[0].name.empty() || arguments[0].is_splat
        || !ast::is_valid(arguments[0].value))
    {
        report(diagnostic_kind::invalid_attribute_arguments, file, declared.name,
               "@tangent on a struct names the struct its change is: `@tangent(splat_change)`");
        return;
    }
    if (is_generic)
    {
        unsupported(file, declared.name, "@tangent on a generic struct");
        return;
    }
    auto const at = span_of(file, arguments[0].value);
    // its own name would be a cycle of the struct with itself, which says less than what is wrong
    if (text_of(file, at) == out.name_of(primal))
    {
        report(diagnostic_kind::wrong_kind_of_name, file, at,
               cc::format("{0} is no other struct of the program, and the change of {0} is one", out.name_of(primal)));
        return;
    }
    auto const tangent = resolve_value_type(file, arguments[0].value, nullptr);
    if (tangent == checked_module::error_type)
        return;
    auto const primal_name = cc::string(out.name_of(primal));
    auto const tangent_name = cc::string(out.name_of(tangent));
    auto const tangent_info = out.at(tangent);
    if (out.builtin_type_of(tangent) != nullptr || tangent_info.kind != type_kind::structure || tangent_info.is_template
        || is_valid(tangent_info.generic) || tangent == primal)
    {
        report(
            diagnostic_kind::wrong_kind_of_name, file, at,
            cc::format("{} is no other struct of the program, and the change of {} is one", tangent_name, primal_name));
        return;
    }
    if (auto const* const other = declared_primals.get_ptr(i32(tangent)))
    {
        report(diagnostic_kind::wrong_kind_of_name, file, at,
               cc::format("{} is the tangent of {} already, and each struct's tangent is its own", tangent_name,
                          out.name_of(*other)));
        return;
    }

    // each field of the tangent is the change of the field of its name, of that field's tangent type
    auto const primal_members = cc::vector<member_info>::create_copy_of(out.at(out.at(primal).members));
    auto is_sound = true;
    for (auto const& field : cc::vector<member_info>::create_copy_of(out.at(tangent_info.members)))
    {
        auto const* changed = static_cast<member_info const*>(nullptr);
        for (auto const& m : primal_members)
            if (m.name == field.name)
                changed = &m;
        if (changed == nullptr)
        {
            report(diagnostic_kind::wrong_kind_of_name, file, at,
                   cc::format("{}.{} names no field of {}, and each field of a tangent is the change of the field of "
                              "its name",
                              tangent_name, field.name, primal_name));
            is_sound = false;
        }
        else if (!is_differentiable_type(changed->type))
        {
            report(diagnostic_kind::type_mismatch, file, at,
                   cc::format("{}.{} is {}, which holds no float, so nothing changes it and {} holds no change of it",
                              primal_name, field.name, out.name_of(changed->type), tangent_name));
            is_sound = false;
        }
        else if (auto const expected = tangent_type(changed->type); expected != field.type)
        {
            report(diagnostic_kind::type_mismatch, file, at,
                   cc::format("{}.{} is {}, where a change of {}.{}, {}, is {}", tangent_name, field.name,
                              out.name_of(field.type), primal_name, field.name, out.name_of(changed->type),
                              out.name_of(expected)));
            is_sound = false;
        }
    }
    // a tangent changes by a tangent of its own type, which a position or an int in it would not
    if (is_sound && tangent_type(tangent) != tangent)
    {
        report(diagnostic_kind::type_mismatch, file, at,
               cc::format("{} is not its own tangent, which a tangent's fields of floats, vectors and directions are",
                          tangent_name));
        is_sound = false;
    }
    if (!is_sound)
        return;
    tangent_types[i32(primal)] = tangent;
    declared_primals[i32(tangent)] = primal;
}

type_id checker::primal_of_tangent(type_id tangent) const
{
    if (!is_valid(tangent) || tangent == checked_module::error_type)
        return type_id::none;
    if (auto const& info = out.at(tangent); is_valid(info.tangent_of))
        return info.tangent_of;
    auto const* const declared = declared_primals.get_ptr(i32(tangent));
    return declared != nullptr ? *declared : type_id::none;
}

i32 checker::tangent_member(type_id primal, i32 member)
{
    auto const t = tangent_type(primal);
    if (t == primal)
        return member;
    auto const name = out.at(out.at(primal).members)[member].name;
    auto const members = out.at(out.at(t).members);
    for (auto i = isize(0); i < members.size(); ++i)
        if (members[i].name == name)
            return i32(i);
    return -1;
}

bool checker::takes_tangent(symbol_id function, isize parameter) const
{
    auto const own = out.at(out.functions[out.at(function).info].parameters);
    return parameter < own.size() && takes_tangent(function, parameter, own[parameter].type);
}

bool checker::takes_tangent(symbol_id function, isize parameter, type_id type) const
{
    auto const& s = out.at(function);
    auto const own = out.at(out.functions[s.info].parameters);
    if (parameter >= own.size() || own[parameter].is_mut || !is_differentiable_type(type))
        return false;
    auto const* const fun = ast_of(s.file).at(s.declaration).node.try_as<ast::fun_decl>();
    if (fun == nullptr)
        return true;
    auto const fields = ast_of(s.file).at(fun->parameters);
    return parameter >= fields.size() || find_attribute(s.file, fields[parameter].attributes, "no_diff") == nullptr;
}

cc::vector<symbol_id> checker::derivative_candidates(cc::span<symbol_id const> generic,
                                                     call_arguments const& arguments,
                                                     i32 file,
                                                     source_span where)
{
    auto result = cc::vector<symbol_id>();
    if (generic.empty() || arguments.written.empty() || arguments.functions.empty() || arguments.functions[0] < 0)
        return result;
    auto const& named = function_arguments[arguments.functions[0]];
    if (named.is_lambda)
        return result;
    auto const& declared = out.at(generic[0]);
    auto const is_reverse = declared.name == "bwd_diff";
    for (auto const function : named.functions)
    {
        if (auto const* const known = derivative_overloads.get_ptr(2 * i64(function) + (is_reverse ? 1 : 0)))
        {
            result.push_back(*known);
            continue;
        }
        if (demand(function, file, where) != symbol_state::checked)
            continue;
        auto const& s = out.at(function);
        auto const info = out.functions[s.info];
        if (info.result == checked_module::void_type)
            continue;
        auto const own = cc::vector<parameter>::create_copy_of(out.at(info.parameters));
        if (!ast_of(s.file).at(s.declaration).node.is<ast::fun_decl>())
            continue;

        // DIFF-4: a generic's instance is the one the call's values say, each bound as a call of it binds it, and a
        // value that takes a tangent is followed by it
        auto const is_generic = info.type_parameters.count > 0;
        auto bindings = cc::vector<type_id>();
        auto closed = cc::vector<type_id>();
        auto is_deduced = true;
        auto k = isize(1);
        for (auto i = isize(0); i < own.size(); ++i)
        {
            if (!is_generic)
            {
                closed.push_back(own[i].type);
                continue;
            }
            is_deduced = is_deduced && k < arguments.types.size() && unify(own[i].type, arguments.types[k], bindings);
            closed.push_back(is_deduced ? substitute(own[i].type, bindings) : checked_module::error_type);
            k += is_deduced && takes_tangent(function, i, closed.back()) ? 2 : 1;
        }
        if (is_generic)
        {
            for (auto const q : out.at(info.type_parameters))
            {
                auto is_bound = false;
                for (auto b = isize(0); b + 1 < bindings.size(); b += 2)
                    is_bound = is_bound || bindings[b] == q;
                is_deduced = is_deduced && is_bound;
            }
            if (!is_deduced)
                continue;
            if (instantiate(function, bindings, file, where) < 0)
                continue;
        }
        auto const instance = is_generic ? existing_function_instance(function, bindings) : -1;
        auto const function_result = instance >= 0 && notes[s.info].infers_result
                                       ? out.function_instances[instance].result
                                       : (is_generic ? substitute(info.result, bindings) : info.result);

        auto signature = derivative_signature{.function = function,
                                              .is_reverse = is_reverse,
                                              .bindings = bindings,
                                              .result = function_result};
        auto parameters = cc::vector<parameter>();
        parameters.push_back({.name = "f", .type = function_type(closed, function_result)});
        for (auto i = isize(0); i < own.size(); ++i)
        {
            signature.value_slots.push_back(i32(parameters.size()));
            parameters.push_back({.name = own[i].name, .type = closed[i]});
            if (!takes_tangent(function, i, closed[i]))
            {
                signature.tangent_slots.push_back(-1);
                continue;
            }
            signature.tangent_slots.push_back(i32(parameters.size()));
            parameters.push_back(
                {.name = cc::format("d_{}", own[i].name), .type = tangent_type(closed[i]), .is_mut = is_reverse});
        }
        if (is_reverse)
        {
            signature.result_slot = i32(parameters.size());
            parameters.push_back({.name = "dr", .type = tangent_type(function_result)});
        }

        // of kind `test`, as a const's value scope is: a signature that no pass over the program's functions visits
        auto const overload = symbol_id(out.symbols.size());
        out.symbols.push_back({.file = declared.file,
                               .declaration = declared.declaration,
                               .kind = symbol_kind::test,
                               .state = symbol_state::checked,
                               .name = declared.name,
                               .info = i32(out.functions.size())});
        out.functions.push_back({.symbol = overload,
                                 .parameters = {.first = u32(out.parameters.size()), .count = u32(parameters.size())},
                                 .result = is_reverse ? checked_module::void_type : tangent_type(function_result),
                                 .bindings = {.first = u32(out.binding_lists.size()), .count = 0}});
        out.parameters.push_back_range(parameters);
        // it has no body of its own: the flattener writes the derivative where the call stands
        notes.push_back({.is_body_checked = true, .is_body_sound = true});
        derivative_signatures[i32(overload)] = cc::move(signature);
        // a generic's overload is its instance's, so each call deduces its own
        if (!is_generic)
            derivative_overloads[2 * i64(function) + (is_reverse ? 1 : 0)] = overload;
        result.push_back(overload);
    }
    return result;
}
} // namespace sgl::check::impl
