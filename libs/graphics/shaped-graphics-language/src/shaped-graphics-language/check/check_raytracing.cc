#include <clean-core/sequence/sequence.hh>
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
    // an instance is named by its template, which is what a stage is handed
    auto const named = [&](type_id t)
    {
        if (t == checked_module::error_type)
            return cc::string_view();
        return is_valid(out.at(t).generic) ? cc::string_view(out.at(out.at(t).symbol).name) : out.name_of(t);
    };

    // CHK-327: the stage inputs, each once, each of its type
    auto payloads = 0;
    auto rays = 0;
    auto hits = 0;
    auto candidates = 0;
    auto boxes = 0;
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
        else if (type == "triangle_hit" || type == "procedural_hit")
            ++hits;
        else if (type == "triangle_candidate" || type == "procedural_candidate")
            ++candidates;
        else if (type == "procedural_box")
            ++boxes;
        else
            invalid(cc::format("{} is a {}, which no ray-tracing stage is handed", p.name, type));
    }

    auto const result = named(info.result);
    switch (stage)
    {
    case stage::raygen:
        if (payloads + rays + hits + candidates + boxes > 0)
            invalid("a @raygen fun takes stage inputs alone: it starts rays, and no ray has reached it");
        if (info.result != checked_module::void_type)
            invalid("a @raygen fun returns nothing");
        break;
    case stage::miss:
        if (payloads != 1 || hits + candidates + boxes > 0 || rays > 1)
            invalid("a @miss fun takes its ray type's payload as `p: mut T`, and the ray it missed with, `r: ray`, if "
                    "it reads it");
        if (info.result != checked_module::void_type)
            invalid("a @miss fun returns nothing: what it gives back it writes to the payload");
        break;
    case stage::closest_hit:
        if (payloads != 1 || hits != 1 || candidates + rays + boxes > 0)
            invalid("a @closest_hit fun takes the hit, `h: triangle_hit` or `h: procedural_hit[A]`, and its ray type's "
                    "payload, `p: mut T`");
        if (info.result != checked_module::void_type)
            invalid("a @closest_hit fun returns nothing: what it gives back it writes to the payload");
        break;
    case stage::any_hit:
        if (payloads != 1 || candidates != 1 || hits + rays + boxes > 0)
            invalid("an @any_hit fun takes the candidate, `c: triangle_candidate` or `c: procedural_candidate[A]`, and "
                    "its ray type's payload, `p: mut T`");
        if (result != "hit_decision")
            invalid("an @any_hit fun returns its hit_decision");
        break;
    case stage::intersection:
        // CHK-342: an intersection is handed its box alone, no payload, and what it reports is the attributes a hit
        // hands on, which every target takes as a struct
        if (boxes != 1 || payloads + hits + candidates + rays > 0)
            invalid("an @intersection fun takes the box it decides, `b: procedural_box`, and nothing else: no payload "
                    "reaches it");
        if (result != "report")
            invalid("an @intersection fun returns what it reports, `report[A]`");
        else if (out.at(out.at(info.result).element).kind != type_kind::structure
                 || out.builtin_type_of(out.at(info.result).element) != nullptr)
            invalid("the attributes an @intersection fun reports are a struct of the program");
        else if (auto const bytes = out.ray_data_bytes(out.at(info.result).element);
                 bytes > checked_module::max_attribute_bytes)
            invalid(cc::format("the attributes an @intersection fun reports take {} bytes, and a target holds at most "
                               "{}",
                               bytes, checked_module::max_attribute_bytes));
        break;
    case stage::callable:
        // CHK-343: a callable is handed its parameter as the caller's place, and nothing a ray brings
        if (payloads != 1 || hits + candidates + rays + boxes > 0)
            invalid("a @callable fun takes the caller's parameter as `p: mut T`, and nothing else");
        if (info.result != checked_module::void_type)
            invalid("a @callable fun returns nothing: what it gives back it writes to its parameter");
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
    // a trace still, so nothing else reads the set as a value
    return ray_trace{.file = file, .set = set, .ray = -1};
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

type_id checker::payload_of(symbol_id entry) const
{
    for (auto const& p : out.at(out.functions[out.at(entry).info].parameters))
        if (p.is_mut)
            return p.type;
    return type_id::none;
}

symbol_id checker::ray_set_named(i32 file, source_span name)
{
    auto const text = text_of(file, name);
    auto const* const found = names_seen_from(file).get_ptr(text);
    if (found == nullptr || found->empty())
    {
        report(diagnostic_kind::unknown_name, file, name, text);
        return symbol_id::none;
    }
    if (!is_ray_set(found->front()))
    {
        report(diagnostic_kind::invalid_pipeline, file, name, cc::format("{} is no ray set: `rays {}:`", text, text));
        return symbol_id::none;
    }
    return demand(found->front(), file, name) == symbol_state::checked ? found->front() : symbol_id::none;
}

symbol_id checker::ray_entry_named(i32 file, ast::expr_id value, stage wanted)
{
    auto const* const n = ast::is_valid(value) ? ast_of(file).at(value).node.try_as<ast::name>() : nullptr;
    if (n == nullptr)
    {
        report(diagnostic_kind::invalid_pipeline, file, span_of(file, value),
               cc::format("a @{} stage is filled by the name of an entry point", stage_name(wanted)));
        return symbol_id::none;
    }
    auto const text = text_of(file, n->where);
    auto const* const found = names_seen_from(file).get_ptr(text);
    if (found == nullptr || found->empty())
    {
        report(diagnostic_kind::unknown_name, file, n->where, text);
        return symbol_id::none;
    }
    for (auto const candidate : *found)
    {
        if (out.at(candidate).kind != symbol_kind::function)
            continue;
        if (demand(candidate, file, n->where) != symbol_state::checked)
            return symbol_id::none;
        auto const& info = out.functions[out.at(candidate).info];
        if (info.entry_stage == stage::none)
            continue;
        if (!notes[out.at(candidate).info].is_valid_entry)
            return symbol_id::none;
        if (info.entry_stage != wanted)
        {
            report(diagnostic_kind::invalid_pipeline, file, n->where,
                   cc::format("{} is no @{} entry point", text, stage_name(wanted)));
            return symbol_id::none;
        }
        set_target(file, value, {.kind = target_kind::overload, .symbol = candidate});
        return candidate;
    }
    report(diagnostic_kind::invalid_pipeline, file, n->where, cc::format("{} is no entry point", text));
    return symbol_id::none;
}

void checker::compile_hit_group(symbol_id id)
{
    auto const file = out.at(id).file;
    auto const& ast = ast_of(file);
    auto const& p = ast.at(out.at(id).declaration).node.as<ast::pipeline_decl>();
    auto const fail_symbol = [&] { out.symbols[index_of(id)].state = symbol_state::failed; };
    auto is_failed = false;
    auto const fail = [&](source_span where, cc::string detail)
    {
        report(diagnostic_kind::invalid_pipeline, file, where, cc::move(detail));
        is_failed = true;
    };

    auto const set = ray_set_named(file, p.ray_set);
    if (!is_valid(set))
        return fail_symbol();
    auto const rays = out.at(out.at(out.at(set).type).members);
    auto records = cc::vector<symbol_id>::create_filled(rays.size() * 2, symbol_id::none);
    auto intersection = symbol_id::none;
    auto is_procedural = false;

    for (auto const& s : ast.at(p.settings))
    {
        auto const* const n = ast::is_valid(s.path) ? ast.at(s.path).node.try_as<ast::name>() : nullptr;
        auto const where = span_of(file, s.form);
        if (n == nullptr)
        {
            fail(where, "a hit group sets `geometry`, `intersection`, and one line per ray type of its set");
            continue;
        }
        auto const name = text_of(file, n->where);
        auto const& value = ast.at(s.value).node;
        if (name == "geometry")
        {
            auto const* const dot = value.try_as<ast::leading_dot>();
            auto const kind = dot == nullptr ? cc::string_view() : text_of(file, dot->name);
            if (kind != "triangles" && kind != "procedural")
                fail(where, "a hit group's geometry is `.triangles` or `.procedural`");
            is_procedural = kind == "procedural";
            continue;
        }
        if (name == "intersection")
        {
            intersection = ray_entry_named(file, s.value, stage::intersection);
            is_failed = is_failed || !is_valid(intersection);
            continue;
        }
        auto ray = isize(-1);
        for (auto i = isize(0); i < rays.size(); ++i)
            if (rays[i].name == name)
                ray = i;
        if (ray < 0)
        {
            fail(n->where, cc::format("{} is no ray type of {}", name, out.at(set).name));
            continue;
        }
        // `()` leaves the ray type's record empty: every candidate is accepted, and no closest hit runs
        auto elements = ast::range_of<ast::argument>();
        if (auto const* const o = value.try_as<ast::object>())
            elements = o->elements;
        else if (auto const* const t = value.try_as<ast::tuple>())
            elements = t->elements;
        else
        {
            fail(where, "a ray type's record is `(closest_hit = f, any_hit = g)`, either left out");
            continue;
        }
        for (auto const& element : ast.at(elements))
        {
            auto const slot = element.name.empty() ? cc::string_view() : text_of(file, element.name);
            auto const wanted = slot == "closest_hit" ? stage::closest_hit
                              : slot == "any_hit"     ? stage::any_hit
                                                      : stage::none;
            if (wanted == stage::none)
            {
                fail(span_of(file, element.form), "a record names its `closest_hit` and its `any_hit`");
                continue;
            }
            auto& into = records[ray * 2 + (wanted == stage::any_hit ? 1 : 0)];
            if (is_valid(into))
                fail(span_of(file, element.form), cc::format("{}.{} is set twice", name, slot));
            into = ray_entry_named(file, element.value, wanted);
            if (!is_valid(into))
            {
                is_failed = true;
                continue;
            }
            // CHK-330: a record's shaders carry the payload of its ray type
            if (auto const payload = payload_of(into); payload != rays[ray].type)
                fail(span_of(file, element.value),
                     cc::format("{} takes {}, and {}.{} carries {}", text_of(file, span_of(file, element.value)),
                                out.name_of(payload), out.at(set).name, name, out.name_of(rays[ray].type)));
        }
    }
    // CHK-330: a record's shaders are handed what the group's geometry hits, and a procedural group's attributes are
    // the ones its intersection reports
    auto const attributes
        = is_valid(intersection) ? out.at(out.functions[out.at(intersection).info].result).element : type_id::none;
    for (auto i = isize(0); i < records.size(); ++i)
    {
        if (!is_valid(records[i]))
            continue;
        for (auto const& q : out.at(out.functions[out.at(records[i]).info].parameters))
        {
            if (q.is_mut || q.input != stage_input::none)
                continue;
            auto const& t = out.at(q.type);
            auto const is_procedural_type = is_valid(t.generic);
            if (is_procedural_type != is_procedural)
                fail(span_of(file, out.at(id).declaration),
                     cc::format("{} takes {}, and this group's geometry is {}", out.at(records[i]).name,
                                out.name_of(q.type), is_procedural ? "procedural" : "triangles"));
            else if (is_procedural_type && is_valid(attributes) && t.element != attributes)
                fail(span_of(file, out.at(id).declaration),
                     cc::format("{} takes {}, and {} reports {}", out.at(records[i]).name, out.name_of(q.type),
                                out.at(intersection).name, out.name_of(attributes)));
        }
    }
    if (is_procedural && !is_valid(intersection))
        fail(span_of(file, out.at(id).declaration), "a procedural hit group has an intersection");
    if (!is_procedural && is_valid(intersection))
        fail(span_of(file, out.at(id).declaration), "a hit group with an intersection is procedural: `geometry = "
                                                    ".procedural`");
    if (is_failed)
        return fail_symbol();

    out.symbols[index_of(id)].info = i32(out.pipelines.size());
    out.pipelines.push_back({
        .symbol = id,
        .kind = pipeline_kind::hit_group,
        .ray_set = set,
        .records = {.first = u32(out.binding_lists.size()), .count = u32(records.size())},
        .intersection = intersection,
        .is_procedural = is_procedural,
    });
    out.binding_lists.push_back_range(records);
}

void checker::compile_raytracing_pipeline(symbol_id id)
{
    auto const file = out.at(id).file;
    auto const& ast = ast_of(file);
    auto const decl = out.at(id).declaration;
    auto const& p = ast.at(decl).node.as<ast::pipeline_decl>();
    auto const fail_symbol = [&] { out.symbols[index_of(id)].state = symbol_state::failed; };
    auto is_failed = false;
    auto const fail = [&](source_span where, cc::string detail)
    {
        report(diagnostic_kind::invalid_pipeline, file, where, cc::move(detail));
        is_failed = true;
    };
    if (p.is_short_form)
    {
        fail(span_of(file, decl), "a @raytracing pipeline names its shaders in a block of settings");
        return fail_symbol();
    }

    // the ray set first, which every other line is read against
    auto set = symbol_id::none;
    for (auto const& s : ast.at(p.settings))
        if (auto const* const n = ast::is_valid(s.path) ? ast.at(s.path).node.try_as<ast::name>() : nullptr;
            n != nullptr && text_of(file, n->where) == "rays")
        {
            auto const* const value = ast.at(s.value).node.try_as<ast::name>();
            if (value == nullptr)
                fail(span_of(file, s.form), "`rays` names a ray set");
            else
                set = ray_set_named(file, value->where);
        }
    if (!is_valid(set))
    {
        if (!is_failed)
            fail(span_of(file, decl), "a @raytracing pipeline names its ray set: `rays = <set>`");
        return fail_symbol();
    }
    auto const rays = out.at(out.at(out.at(set).type).members);

    auto raygen = symbol_id::none;
    auto misses = cc::vector<symbol_id>::create_filled(rays.size(), symbol_id::none);
    auto groups = cc::vector<symbol_id>();
    auto has_host = false;
    auto depth = cc::optional<i32>();
    for (auto const& s : ast.at(p.settings))
    {
        auto const where = span_of(file, s.form);
        auto const& path = ast.at(s.path).node;
        auto const& value = ast.at(s.value).node;
        if (auto const* const m = path.try_as<ast::member>())
        {
            auto const* const head = ast::is_valid(m->object) ? ast.at(m->object).node.try_as<ast::name>() : nullptr;
            if (head == nullptr || text_of(file, head->where) != "miss")
            {
                fail(where, "a @raytracing pipeline sets `rays`, `raygen`, `miss.<ray>`, `hit_groups` and "
                            "`max_recursion_depth`");
                continue;
            }
            auto const name = text_of(file, m->name);
            auto ray = isize(-1);
            for (auto i = isize(0); i < rays.size(); ++i)
                if (rays[i].name == name)
                    ray = i;
            if (ray < 0)
            {
                fail(m->name, cc::format("{} is no ray type of {}", name, out.at(set).name));
                continue;
            }
            if (is_valid(misses[ray]))
                fail(where, cc::format("miss.{} is set twice", name));
            misses[ray] = ray_entry_named(file, s.value, stage::miss);
            if (!is_valid(misses[ray]))
                is_failed = true;
            else if (auto const payload = payload_of(misses[ray]); payload != rays[ray].type)
                fail(span_of(file, s.value),
                     cc::format("{} takes {}, and {}.{} carries {}", out.at(misses[ray]).name, out.name_of(payload),
                                out.at(set).name, name, out.name_of(rays[ray].type)));
            continue;
        }
        auto const* const n = path.try_as<ast::name>();
        auto const name = n == nullptr ? cc::string_view() : text_of(file, n->where);
        if (name == "rays")
            continue;
        if (name == "raygen")
        {
            raygen = ray_entry_named(file, s.value, stage::raygen);
            is_failed = is_failed || !is_valid(raygen);
            continue;
        }
        if (name == "hit_groups")
        {
            auto elements = ast::range_of<ast::argument>();
            if (auto const* const t = value.try_as<ast::tuple>())
                elements = t->elements;
            else if (auto const* const dot = value.try_as<ast::leading_dot>();
                     dot != nullptr && text_of(file, dot->name) == "host")
            {
                has_host = true;
                continue;
            }
            auto const listed = ast.at(elements);
            auto const count = value.is<ast::name>() ? 1 : listed.size();
            groups.clear();
            for (auto i = isize(0); i < count; ++i)
            {
                auto const v = value.is<ast::name>() ? s.value : listed[i].value;
                auto const& e = ast.at(v).node;
                if (auto const* const dot = e.try_as<ast::leading_dot>();
                    dot != nullptr && text_of(file, dot->name) == "host")
                {
                    // `.host` is last: the host's hit groups follow the listed ones in the table
                    if (i != count - 1)
                        fail(span_of(file, v), "`.host` stands last among the hit groups");
                    has_host = true;
                    continue;
                }
                auto const* const g = e.try_as<ast::name>();
                auto const text = g == nullptr ? cc::string_view() : text_of(file, g->where);
                auto const* const found = g == nullptr ? nullptr : names_seen_from(file).get_ptr(text);
                auto group = symbol_id::none;
                if (found != nullptr)
                    for (auto const candidate : *found)
                        if (out.at(candidate).kind == symbol_kind::pipeline
                            && ast_of(out.at(candidate).file)
                                   .at(out.at(candidate).declaration)
                                   .node.as<ast::pipeline_decl>()
                                   .is_hit_group)
                            group = candidate;
                if (!is_valid(group))
                {
                    fail(span_of(file, v), cc::format("{} is no hit group", text_of(file, span_of(file, v))));
                    continue;
                }
                if (demand(group, file, g->where) != symbol_state::checked)
                {
                    is_failed = true;
                    continue;
                }
                set_target(file, v, {.kind = target_kind::symbol, .symbol = group});
                if (out.pipelines[out.at(group).info].ray_set != set)
                    fail(g->where, cc::format("{} is a hit group for {}, and this pipeline's rays are {}", text,
                                              out.at(out.pipelines[out.at(group).info].ray_set).name, out.at(set).name));
                groups.push_back(group);
            }
            continue;
        }
        if (name == "max_recursion_depth")
        {
            auto const parsed = parse_literal_integer(text_of(file, span_of(file, s.value)));
            if (!parsed.has_value() || parsed.value() < 1 || parsed.value() > 31)
                fail(span_of(file, s.value), "max_recursion_depth is an int literal from 1 to 31");
            else
                depth = i32(parsed.value());
            continue;
        }
        fail(where, "a @raytracing pipeline sets `rays`, `raygen`, `miss.<ray>`, `hit_groups` and "
                    "`max_recursion_depth`");
    }
    if (!is_valid(raygen) && !is_failed)
        fail(span_of(file, decl), "a @raytracing pipeline has a raygen: `raygen = <entry point>`");
    // CHK-331: the depth is derived where the trace graph is known whole, and a bound only the host's groups need
    if (depth.has_value() && !has_host)
        fail(span_of(file, decl), "max_recursion_depth is derived from what the shaders trace; it is declared only "
                                  "beside `.host` hit groups");
    if (!depth.has_value() && has_host && !is_failed)
        fail(span_of(file, decl), "a pipeline with `.host` hit groups declares `max_recursion_depth`, which those "
                                  "groups' traces may not exceed");
    if (is_failed)
        return fail_symbol();

    // CHK-331: one layout serves every shader of the pipeline, so their binding lists agree by position
    auto layout = cc::vector<symbol_id>();
    auto inline_constants = symbol_id::none;
    {
        auto entries = cc::vector<symbol_id>();
        entries.push_back(raygen);
        entries.push_back_range(misses);
        for (auto const group : groups)
        {
            entries.push_back_range(out.at(out.pipelines[out.at(group).info].records));
            entries.push_back(out.pipelines[out.at(group).info].intersection);
        }
        auto lists = cc::vector<cc::vector<symbol_id>>();
        for (auto const entry : entries)
        {
            if (!is_valid(entry))
                continue;
            auto list = cc::vector<symbol_id>();
            for (auto const b : out.at(out.functions[out.at(entry).info].bindings))
            {
                if (out.bindings[out.at(b).info].is_inline)
                {
                    if (is_valid(inline_constants) && inline_constants != b)
                        fail(span_of(file, decl), cc::format("the shaders list two @inline bindings, {} and {}, and a "
                                                             "pipeline has one",
                                                             out.at(inline_constants).name, out.at(b).name));
                    inline_constants = b;
                    continue;
                }
                list.push_back(b);
            }
            lists.push_back(cc::move(list));
        }
        for (auto const& list : lists)
            if (list.size() > layout.size())
                layout = list;
        for (auto const& list : lists)
            for (auto i = isize(0); i < list.size(); ++i)
                if (list[i] != layout[i])
                {
                    fail(span_of(file, decl), cc::format("group {} is {} to one shader and {} to another: the binding "
                                                         "lists agree by position",
                                                         i, out.at(list[i]).name, out.at(layout[i]).name));
                    break;
                }
    }
    if (is_failed)
        return fail_symbol();

    out.symbols[index_of(id)].info = i32(out.pipelines.size());
    auto const first = u32(out.binding_lists.size());
    out.binding_lists.push_back_range(misses);
    out.binding_lists.push_back_range(groups);
    out.binding_lists.push_back_range(layout);
    out.pipelines.push_back({
        .symbol = id,
        .kind = pipeline_kind::raytracing,
        .layout = {.first = first + u32(misses.size() + groups.size()), .count = u32(layout.size())},
        .inline_constants = inline_constants,
        .ray_set = set,
        .raygen = raygen,
        .misses = {.first = first, .count = u32(misses.size())},
        .hit_groups = {.first = first + u32(misses.size()), .count = u32(groups.size())},
        .has_host_hit_groups = has_host,
        .max_recursion_depth = depth.value_or(0),
    });
}

void checker::judge_trace_graphs()
{
    auto const traced_by = [&](symbol_id entry) -> cc::span<flat_traced_ray const>
    {
        for (auto const& e : out.entry_points)
            if (e.function == entry)
                return e.traced_rays;
        return {};
    };
    for (auto& pipeline : out.pipelines)
    {
        if (pipeline.kind != pipeline_kind::raytracing)
            continue;
        auto const file = out.at(pipeline.symbol).file;
        auto const where = span_of(file, out.at(pipeline.symbol).declaration);
        auto const count = out.at(out.at(out.at(pipeline.ray_set).type).members).size();
        // CHK-331: a trace's contribution, multiplier and miss are positions in the pipeline's own set, so a trace of
        // another set's ray type would run against this set's records with the wrong payload
        auto is_foreign = false;
        auto const judge_sets = [&](symbol_id entry)
        {
            if (!is_valid(entry))
                return;
            for (auto const& t : traced_by(entry))
                if (t.set != pipeline.ray_set)
                {
                    report(diagnostic_kind::invalid_pipeline, file, where,
                           cc::format("{} traces {}.{}, and this pipeline's rays are {}", out.at(entry).name,
                                      out.at(t.set).name, out.at(out.at(out.at(t.set).type).members)[t.ray].name,
                                      out.at(pipeline.ray_set).name));
                    is_foreign = true;
                }
        };
        judge_sets(pipeline.raygen);
        for (auto const miss : out.at(pipeline.misses))
            judge_sets(miss);
        for (auto const group : out.at(pipeline.hit_groups))
            for (auto r = isize(0); r < count; ++r)
                judge_sets(out.at(out.pipelines[out.at(group).info].records)[r * 2]);
        if (is_foreign)
        {
            out.symbols[index_of(pipeline.symbol)].state = symbol_state::failed;
            continue;
        }

        // the ray types a trace of each ray type reaches: its miss, and its closest hit in every listed group
        auto next = cc::vector<cc::vector<i32>>::create_filled(count, {});
        auto const add = [&](isize from, symbol_id entry)
        {
            if (!is_valid(entry))
                return;
            for (auto const& t : traced_by(entry))
                if (!cc::sequence{next[from]}.any([&](i32 r) { return r == t.ray; }))
                    next[from].push_back(t.ray);
        };
        for (auto r = isize(0); r < count; ++r)
        {
            add(r, out.at(pipeline.misses)[r]);
            for (auto const group : out.at(pipeline.hit_groups))
                add(r, out.at(out.pipelines[out.at(group).info].records)[r * 2]);
        }

        // CHK-332: the longest chain of traces, which a cycle has none of
        enum class mark : u8
        {
            unseen,
            open,
            done
        };
        auto marks = cc::vector<mark>::create_filled(count, mark::unseen);
        auto longest = cc::vector<i32>::create_filled(count, 1);
        auto is_cyclic = false;
        auto const visit = [&](auto const& self, i32 r) -> void
        {
            if (marks[r] == mark::done || is_cyclic)
                return;
            if (marks[r] == mark::open)
            {
                is_cyclic = true;
                auto const rays = out.at(out.at(out.at(pipeline.ray_set).type).members);
                report(diagnostic_kind::recursive_trace, file, where,
                       cc::format("a trace of {}.{} reaches a shader that traces it again",
                                  out.at(pipeline.ray_set).name, rays[r].name));
                return;
            }
            marks[r] = mark::open;
            for (auto const s : next[r])
            {
                self(self, s);
                longest[r] = cc::max(longest[r], longest[s] + 1);
            }
            marks[r] = mark::done;
        };
        // from every ray type, not only the raygen's: a host's closest hit may trace into a cycle no listed shader
        // reaches, and the declared depth would not bound it
        for (auto r = i32(0); r < i32(count); ++r)
            visit(visit, r);
        if (is_cyclic)
        {
            out.symbols[index_of(pipeline.symbol)].state = symbol_state::failed;
            continue;
        }
        auto depth = 0;
        for (auto const& t : traced_by(pipeline.raygen))
            depth = cc::max(depth, longest[t.ray]);
        if (!pipeline.has_host_hit_groups)
            pipeline.max_recursion_depth = cc::max(depth, 1);
        else if (depth > pipeline.max_recursion_depth)
            report(diagnostic_kind::invalid_pipeline, file, where,
                   cc::format("its listed shaders trace {} deep, past its max_recursion_depth of {}", depth,
                              pipeline.max_recursion_depth));
    }
}

void checker::compile_callables(symbol_id id)
{
    auto const file = out.at(id).file;
    auto const& ast = ast_of(file);
    auto const& p = ast.at(out.at(id).declaration).node.as<ast::pipeline_decl>();
    auto const fail_symbol = [&] { out.symbols[index_of(id)].state = symbol_state::failed; };
    auto is_failed = false;
    if (!p.is_short_form)
    {
        report(diagnostic_kind::invalid_pipeline, file, span_of(file, out.at(id).declaration),
               "a callables table lists its callables: `callables name = (f, g)`");
        return fail_symbol();
    }
    auto entries = cc::vector<symbol_id>();
    auto parameter = type_id::none;
    auto has_host = false;
    auto const elements = ast.at(p.stages);
    for (auto i = isize(0); i < elements.size(); ++i)
    {
        auto const& element = elements[i];
        auto const where = span_of(file, element.form);
        if (auto const* const dot = ast.at(element.value).node.try_as<ast::leading_dot>();
            dot != nullptr && text_of(file, dot->name) == "host")
        {
            // CHK-343: the host's callables follow the listed ones
            if (i != elements.size() - 1)
            {
                report(diagnostic_kind::invalid_pipeline, file, where, "`.host` stands last among the callables");
                is_failed = true;
            }
            has_host = true;
            continue;
        }
        auto const entry = ray_entry_named(file, element.value, stage::callable);
        if (!is_valid(entry))
        {
            is_failed = true;
            continue;
        }
        auto const taking = payload_of(entry);
        if (!is_valid(parameter))
            parameter = taking;
        else if (taking != parameter)
        {
            report(diagnostic_kind::invalid_pipeline, file, where,
                   cc::format("{} takes {}, and this table's callables take {}", out.at(entry).name,
                              out.name_of(taking), out.name_of(parameter)));
            is_failed = true;
        }
        entries.push_back(entry);
    }
    if (entries.empty() && !is_failed)
    {
        report(diagnostic_kind::invalid_pipeline, file, span_of(file, out.at(id).declaration),
               "a callables table lists at least one callable, which says what its callables take");
        is_failed = true;
    }
    if (is_failed)
        return fail_symbol();
    out.symbols[index_of(id)].info = i32(out.pipelines.size());
    out.pipelines.push_back({
        .symbol = id,
        .kind = pipeline_kind::callables,
        .records = {.first = u32(out.binding_lists.size()), .count = u32(entries.size())},
        .callable_parameter = parameter,
        .has_host_callables = has_host,
    });
    out.binding_lists.push_back_range(entries);
}

symbol_id checker::callables_named(i32 file, ast::expr_id expr)
{
    auto const* const n = ast::is_valid(expr) ? ast_of(file).at(expr).node.try_as<ast::name>() : nullptr;
    if (n == nullptr)
        return symbol_id::none;
    auto const* const found = names_seen_from(file).get_ptr(text_of(file, n->where));
    if (found == nullptr || found->empty() || out.at(found->front()).kind != symbol_kind::pipeline)
        return symbol_id::none;
    auto const table = found->front();
    if (!ast_of(out.at(table).file).at(out.at(table).declaration).node.as<ast::pipeline_decl>().is_callables)
        return symbol_id::none;
    return table;
}

type_id checker::check_callable_call(function_scope& scope,
                                     ast::expr_id id,
                                     ast::call const& call,
                                     ast::index const& index,
                                     symbol_id table)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const where = span_of(file, id);
    set_target(file, index.object, {.kind = target_kind::symbol, .symbol = table});
    if (demand(table, file, where) != symbol_state::checked)
        return checked_module::error_type;
    auto const parameter = out.pipelines[out.at(table).info].callable_parameter;
    auto is_sound = true;
    auto const indices = ast.at(index.arguments);
    if (indices.size() != 1 || !indices[0].name.empty() || indices[0].is_splat)
    {
        report(diagnostic_kind::no_matching_overload, file, where, "a callable is picked by one int: `table[i](mut p)`");
        is_sound = false;
    }
    else
        is_sound = check_expected(scope, indices[0].value, type_of_builtin(builtins::k_int, file, where))
                != checked_module::error_type;
    // CHK-344: the parameter is the caller's place, of the type every callable of the table takes
    auto const arguments = ast.at(call.arguments);
    if (arguments.size() != 1 || !arguments[0].is_mut || !arguments[0].name.empty())
    {
        report(diagnostic_kind::no_matching_overload, file, where,
               cc::format("a callable takes one place of {}, handed over as `mut p`", out.name_of(parameter)));
        return checked_module::error_type;
    }
    auto const given = check_expr(scope, arguments[0].value);
    if (given != checked_module::error_type && given != parameter)
    {
        report(diagnostic_kind::type_mismatch, file, span_of(file, arguments[0].value),
               cc::format("{}'s callables take {}, and this is {}", out.at(table).name, out.name_of(parameter),
                          out.name_of(given)));
        is_sound = false;
    }
    else if (given != checked_module::error_type && !judge_place(scope, arguments[0].value, "a callable's parameter"))
        is_sound = false;
    if (!is_sound || given == checked_module::error_type)
        return checked_module::error_type;
    out.callable_calls.push_back({.file = file, .call = id, .table = table});
    return checked_module::void_type;
}

void checker::judge_callables()
{
    // tables pack in declaration order, so a table the host appends to is the last one
    auto last = symbol_id::none;
    for (auto const& p : out.pipelines)
        if (p.kind == pipeline_kind::callables && (!is_valid(last) || index_of(p.symbol) > index_of(last)))
            last = p.symbol;
    for (auto const& p : out.pipelines)
        if (p.kind == pipeline_kind::callables && p.has_host_callables && p.symbol != last)
            report(diagnostic_kind::invalid_pipeline, out.at(p.symbol).file,
                   span_of(out.at(p.symbol).file, out.at(p.symbol).declaration),
                   cc::format("{} takes the host's callables, so it is the module's last callables table: the host's "
                              "follow every listed one",
                              out.at(p.symbol).name));
}
