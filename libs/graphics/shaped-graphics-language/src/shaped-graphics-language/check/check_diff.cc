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

type_id checker::tangent_type(type_id type)
{
    if (!is_valid(type) || type == checked_module::error_type || type == checked_module::void_type)
        return type;
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
    if (is_open(of))
    {
        unsupported(file, span_of(file, expr), "the tangent of a type parameter, which waits for interfaces");
        return checked_module::error_type;
    }
    if (!is_differentiable_type(of))
    {
        report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, expr),
               cc::format("{} holds no float, so nothing of it changes and it has no tangent", out.name_of(of)));
        return checked_module::error_type;
    }
    return tangent_type(of);
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
    auto const& s = out.at(function);
    auto const own = out.at(out.functions[s.info].parameters);
    if (parameter >= own.size() || own[parameter].is_mut || !is_differentiable_type(own[parameter].type))
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
        auto const& info = out.functions[s.info];
        if (info.type_parameters.count > 0 || info.result == checked_module::void_type)
            continue;
        auto const own = cc::vector<parameter>::create_copy_of(out.at(info.parameters));
        if (!ast_of(s.file).at(s.declaration).node.is<ast::fun_decl>())
            continue;

        auto types = cc::vector<type_id>();
        for (auto const& p : own)
            types.push_back(p.type);
        auto signature = derivative_signature{.function = function, .is_reverse = is_reverse};
        auto parameters = cc::vector<parameter>();
        parameters.push_back({.name = "f", .type = function_type(types, info.result)});
        for (auto i = isize(0); i < own.size(); ++i)
        {
            signature.value_slots.push_back(i32(parameters.size()));
            parameters.push_back({.name = own[i].name, .type = own[i].type});
            if (!takes_tangent(function, i))
            {
                signature.tangent_slots.push_back(-1);
                continue;
            }
            signature.tangent_slots.push_back(i32(parameters.size()));
            parameters.push_back(
                {.name = cc::format("d_{}", own[i].name), .type = tangent_type(own[i].type), .is_mut = is_reverse});
        }
        if (is_reverse)
        {
            signature.result_slot = i32(parameters.size());
            parameters.push_back({.name = "dr", .type = tangent_type(info.result)});
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
                                 .result = is_reverse ? checked_module::void_type : tangent_type(info.result),
                                 .bindings = {.first = u32(out.binding_lists.size()), .count = 0}});
        out.parameters.push_back_range(parameters);
        // it has no body of its own: the flattener writes the derivative where the call stands
        notes.push_back({.is_body_checked = true, .is_body_sound = true});
        derivative_signatures[i32(overload)] = cc::move(signature);
        derivative_overloads[2 * i64(function) + (is_reverse ? 1 : 0)] = overload;
        result.push_back(overload);
    }
    return result;
}
} // namespace sgl::check::impl
