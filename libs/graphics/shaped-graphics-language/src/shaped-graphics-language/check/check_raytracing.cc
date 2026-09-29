#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// The ray-tracing stages and what their entry points take (the spec's raytracing file).

void checker::judge_ray_stage(symbol_id id, cc::function_ref<void(cc::string_view)> invalid)
{
    auto const& s = out.at(id);
    auto const& info = out.functions[s.info];
    auto const stage = info.entry_stage;
    auto const parameters = out.at(info.parameters);
    auto const named = [&](type_id t) { return t == checked_module::error_type ? cc::string_view() : out.name_of(t); };

    // CHK-327: the stage inputs, each once, each of its type
    auto payloads = 0;
    auto rays = 0;
    auto hits = 0;
    auto candidates = 0;
    for (auto const& p : parameters)
    {
        if (p.input != stage_input::none)
        {
            auto const& input = info_of(p.input);
            if (input.in_stage != stage && (input.also_in & stage_bit(stage)) == 0)
                invalid(cc::format("@{} is an input of the ray-tracing stages", input.name));
            else if (out.name_of(p.type) != input.type)
                invalid(cc::format("a @{} parameter is an {}", input.name, input.type));
            continue;
        }
        // CHK-328: the payload is a `mut` parameter of a struct, the caller's place
        if (p.is_mut)
        {
            ++payloads;
            if (p.type != checked_module::error_type && out.at(p.type).kind != type_kind::structure)
                invalid(cc::format("{} is the payload, which is a struct", p.name));
            continue;
        }
        auto const type = named(p.type);
        if (type == "ray")
            ++rays;
        else if (type == "triangle_hit")
            ++hits;
        else if (type == "triangle_candidate")
            ++candidates;
        else
            invalid(cc::format("{} is a {}, which no ray-tracing stage is handed", p.name, type));
    }

    auto const result = named(info.result);
    switch (stage)
    {
    case stage::raygen:
        if (payloads + rays + hits + candidates > 0)
            invalid("a @raygen fun takes stage inputs alone: it starts rays, and no ray has reached it");
        if (info.result != checked_module::void_type)
            invalid("a @raygen fun returns nothing");
        break;
    case stage::miss:
        if (payloads != 1 || hits + candidates > 0 || rays > 1)
            invalid("a @miss fun takes its ray type's payload as `p: mut T`, and the ray it missed with, `r: ray`, if "
                    "it reads it");
        if (info.result != checked_module::void_type)
            invalid("a @miss fun returns nothing: what it gives back it writes to the payload");
        break;
    case stage::closest_hit:
        if (payloads != 1 || hits != 1 || candidates + rays > 0)
            invalid("a @closest_hit fun takes the hit, `h: triangle_hit`, and its ray type's payload, `p: mut T`");
        if (info.result != checked_module::void_type)
            invalid("a @closest_hit fun returns nothing: what it gives back it writes to the payload");
        break;
    case stage::any_hit:
        if (payloads != 1 || candidates != 1 || hits + rays > 0)
            invalid("an @any_hit fun takes the candidate, `c: triangle_candidate`, and its ray type's payload, `p: mut "
                    "T`");
        if (result != "hit_decision")
            invalid("an @any_hit fun returns its hit_decision");
        break;
    case stage::intersection:
        invalid("an @intersection fun is not built yet");
        break;
    case stage::callable:
        invalid("a @callable fun is not built yet");
        break;
    default:
        break;
    }
}

bool checker::is_ray_set(symbol_id symbol) const
{
    auto const& s = out.at(symbol);
    if (s.kind != symbol_kind::structure || !ast::is_valid(s.declaration))
        return false;
    auto const* const d = ast_of(s.file).at(s.declaration).node.try_as<ast::struct_decl>();
    return d != nullptr && d->is_ray_set;
}

cc::optional<ray_trace> checker::ray_type_of(i32 file, ast::expr_id expr)
{
    auto const* const member = ast_of(file).at(expr).node.try_as<ast::member>();
    if (member == nullptr || !ast::is_valid(member->object))
        return cc::nullopt;
    auto const* const n = ast_of(file).at(member->object).node.try_as<ast::name>();
    if (n == nullptr)
        return cc::nullopt;
    auto const* const found = names_seen_from(file).get_ptr(text_of(file, n->where));
    if (found == nullptr || found->empty() || !is_ray_set(found->front()))
        return cc::nullopt;
    auto const set = found->front();
    if (demand(set, file, span_of(file, expr)) != symbol_state::checked)
        return cc::nullopt;
    auto const members = out.at(out.at(out.at(set).type).members);
    for (auto i = isize(0); i < members.size(); ++i)
        if (members[i].name == text_of(file, member->name))
        {
            set_target(file, member->object, {.kind = target_kind::symbol, .symbol = set});
            return ray_trace{.file = file, .call = ast::expr_id::none, .set = set, .ray = i32(i)};
        }
    report(diagnostic_kind::unknown_member, file, member->name,
           cc::format("{} has no ray type {}", out.at(set).name, text_of(file, member->name)));
    return cc::nullopt;
}

type_id checker::check_pipeline_trace(function_scope& scope, ast::expr_id id, ast::call const& call, ray_trace ray)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const arguments = ast.at(call.arguments);
    auto const where = span_of(file, id);
    auto is_sound = true;

    // world, the ray, the ray type, the payload, then `flags` and `mask` by name
    for (auto i = isize(4); i < arguments.size(); ++i)
    {
        auto const name = text_of(file, arguments[i].name);
        if (name == "flags")
            is_sound = check_expected(scope, arguments[i].value, type_of_builtin("ray_flags", file, where))
                        != checked_module::error_type
                    && is_sound;
        else if (name == "mask")
            is_sound = check_expected(scope, arguments[i].value, type_of_builtin(builtins::k_int, file, where))
                        != checked_module::error_type
                    && is_sound;
        else
        {
            report(diagnostic_kind::no_matching_overload, file, span_of(file, arguments[i].form),
                   "a trace takes the structure, the ray, the ray type and the payload, then `flags` and `mask` by "
                   "name");
            is_sound = false;
        }
    }

    handed.push_back(arguments[0].value);
    auto const world = check_expr(scope, arguments[0].value);
    handed.pop_back();
    if (world != checked_module::error_type && out.at(world).kind != type_kind::acceleration_structure)
    {
        report(diagnostic_kind::type_mismatch, file, span_of(file, arguments[0].value),
               cc::format("a trace runs against an acceleration_structure, and this is {}", out.name_of(world)));
        is_sound = false;
    }
    auto const ray_type = prelude_type("ray");
    is_sound = check_expected(scope, arguments[1].value, ray_type) != checked_module::error_type && is_sound;

    // CHK-329: the payload is the ray type's, handed over as the caller's place
    auto const payload = out.at(out.at(out.at(ray.set).type).members)[ray.ray].type;
    if (!arguments[3].is_mut)
    {
        report(diagnostic_kind::no_matching_overload, file, span_of(file, arguments[3].form),
               "the payload is the caller's place, handed over as `mut p`");
        is_sound = false;
    }
    auto const given = check_expr(scope, arguments[3].value);
    if (given != checked_module::error_type && given != payload)
    {
        report(diagnostic_kind::type_mismatch, file, span_of(file, arguments[3].value),
               cc::format("{}.{} carries {}, and this payload is {}", out.at(ray.set).name,
                          out.at(out.at(out.at(ray.set).type).members)[ray.ray].name, out.name_of(payload),
                          out.name_of(given)));
        is_sound = false;
    }
    else if (given != checked_module::error_type && arguments[3].is_mut
             && !judge_place(scope, arguments[3].value, "a payload"))
        is_sound = false;

    if (!is_sound)
        return checked_module::error_type;
    ray.call = id;
    ray.caller = scope.function;
    out.ray_traces.push_back(ray);
    return checked_module::void_type;
}
