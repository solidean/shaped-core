#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// The geometry and the tessellation stages (the spec's checking file, "Geometry and tessellation stages").
// Each is an entry point between the vertex and the pixel stage, and each a feature a device grants.

namespace
{
constexpr auto error_type = checked_module::error_type;
// CHK-301: vulkan's guaranteed maxGeometryOutputVertices, and D3D's bound on the scalars of all of them, which is
// vulkan's guaranteed maxGeometryTotalOutputComponents too
constexpr auto k_max_geometry_vertices = 256;
constexpr auto k_max_geometry_scalars = 1024;
// CHK-480: what D3D allows a mesh stage, and vulkan's VK_EXT_mesh_shader guarantees
constexpr auto k_max_mesh_outputs = 256;
constexpr auto k_max_mesh_workgroup = 128;
/// What one task workgroup launches at most on dx12, `DispatchMesh`'s 2^22 workgroups.
constexpr auto k_max_task_meshes = 1 << 22;

/// The scalars a value of `type` is: what D3D and vulkan count of a geometry stage's output vertex.
i32 scalars_of(checked_module const& m, type_id type)
{
    if (auto const* const record = m.builtin_type_of(type))
        return record->leaf_count;
    auto const& t = m.at(type);
    if (t.kind == type_kind::array)
        return t.count * scalars_of(m, t.element);
    auto count = 0;
    if (t.kind == type_kind::structure)
        for (auto const& member : m.at(t.members))
            count += scalars_of(m, member.type);
    return count;
}

} // namespace

i32 checker::max_vertices_of(i32 file, ast::attribute const& a)
{
    auto const arguments = ast_of(file).at(a.arguments);
    auto const* const argument
        = arguments.size() == 1 && text_of(file, arguments[0].name) == "max_vertices" ? &arguments[0] : nullptr;
    auto const text = argument != nullptr && ast::is_valid(argument->value)
                        ? text_of(file, span_of(file, argument->value))
                        : cc::string_view();
    auto const n = argument != nullptr && ast_of(file).at(argument->value).node.is<ast::literal>()
                        && classify_number(text) == number_class::plain_integer
                     ? parse_plain_integer(text)
                     : cc::optional<i32>();
    // CHK-301: vulkan guarantees 256 output vertices; the scalars they carry are judged with the stream
    if (!n.has_value() || n.value() < 1 || n.value() > k_max_geometry_vertices)
    {
        report(diagnostic_kind::invalid_attribute_arguments, file, a.name,
               "@geometry takes how many vertices it appends at most, an int from 1 to 256: `@geometry(max_vertices = "
               "6)`");
        return 1;
    }
    return n.value();
}

checker::mesh_shape checker::mesh_of(i32 file, ast::attribute const& a)
{
    auto result = mesh_shape();
    auto sizes = 0;
    auto const fail = [&]
    {
        report(diagnostic_kind::invalid_attribute_arguments, file, a.name,
               "@mesh takes one to three workgroup sizes, then `vertices = N` and `triangles = M` or `lines = M`, each "
               "from 1 to 256: `@mesh(32, vertices = 64, triangles = 126)`");
        return mesh_shape{.workgroup = {1, 1, 1}, .vertices = 1, .triangles = 1};
    };
    for (auto const& argument : ast_of(file).at(a.arguments))
    {
        auto const value = ast::is_valid(argument.value) && !argument.is_splat ? constant_count(file, argument.value)
                                                                               : cc::optional<i32>();
        if (!value.has_value() || value.value() < 1)
            return fail();
        auto const key = text_of(file, argument.name);
        if (key.empty() && sizes < 3 && result.vertices == 0 && result.triangles == 0)
            result.workgroup[sizes++] = value.value();
        else if (key == "vertices" && result.vertices == 0 && value.value() <= k_max_mesh_outputs)
            result.vertices = value.value();
        else if ((key == "triangles" || key == "lines") && result.triangles == 0 && value.value() <= k_max_mesh_outputs)
        {
            result.triangles = value.value();
            result.is_lines = key == "lines";
        }
        else
            return fail();
    }
    if (sizes == 0 || result.vertices == 0 || result.triangles == 0)
        return fail();
    if (result.workgroup[0] * result.workgroup[1] * result.workgroup[2] > k_max_mesh_workgroup)
    {
        report(diagnostic_kind::invalid_attribute_arguments, file, a.name,
               cc::format("a mesh workgroup holds at most {} invocations", k_max_mesh_workgroup));
        result.workgroup = {1, 1, 1};
    }
    return result;
}

checker::task_shape checker::task_of(i32 file, ast::attribute const& a)
{
    auto result = task_shape();
    auto sizes = 0;
    for (auto const& argument : ast_of(file).at(a.arguments))
    {
        auto const value = ast::is_valid(argument.value) && !argument.is_splat ? constant_count(file, argument.value)
                                                                               : cc::optional<i32>();
        auto const key = text_of(file, argument.name);
        if (value.has_value() && value.value() >= 1 && key.empty() && sizes < 3 && result.meshes == 0)
            result.workgroup[sizes++] = value.value();
        else if (value.has_value() && value.value() >= 1 && value.value() <= k_max_task_meshes && key == "meshes"
                 && result.meshes == 0 && sizes > 0)
            result.meshes = value.value();
        else
        {
            report(diagnostic_kind::invalid_attribute_arguments, file, a.name,
                   cc::format("@task takes one to three workgroup sizes, then optionally how many mesh workgroups "
                              "one launches at most, from 1 to {}: `@task(32, meshes = 64)`",
                              k_max_task_meshes));
            return {};
        }
    }
    if (sizes == 0)
    {
        report(diagnostic_kind::invalid_attribute_arguments, file, a.name,
               "@task takes one to three workgroup sizes: `@task(32)`, `@task(32, meshes = 64)`");
        return {};
    }
    return result;
}

checker::tessellation_mode checker::tessellation_of(i32 file, ast::attribute const& a)
{
    auto result = tessellation_mode();
    auto has_partitioning = false;
    auto has_winding = false;
    auto has_max_factor = false;
    for (auto const& argument : ast_of(file).at(a.arguments))
    {
        auto const key = text_of(file, argument.name);
        if (key == "max_factor")
        {
            // CHK-304: the largest factor the stage hands the tessellator, which D3D's own caps at 64
            auto const count = ast::is_valid(argument.value) && !argument.is_splat ? constant_count(file, argument.value)
                                                                                   : cc::optional<i32>();
            if (has_max_factor || !count.has_value() || count.value() < 1 || count.value() > k_max_tessellation_factor)
            {
                report(diagnostic_kind::invalid_attribute_arguments, file, argument.name,
                       cc::format("@tessellation_control names `max_factor` once, as an integer from 1 to {}",
                                  k_max_tessellation_factor));
                return result;
            }
            result.max_factor = count.value();
            has_max_factor = true;
            continue;
        }
        // the leading-dot case the argument names, or nothing
        auto const* const dot
            = ast::is_valid(argument.value) ? ast_of(file).at(argument.value).node.try_as<ast::leading_dot>() : nullptr;
        auto const value = dot != nullptr ? text_of(file, dot->name) : cc::string_view();
        if ((key == "partitioning" && has_partitioning) || (key == "winding" && has_winding))
        {
            report(diagnostic_kind::invalid_attribute_arguments, file, argument.name,
                   cc::format("@tessellation_control names {} once", key));
            return result;
        }
        if (key == "partitioning" && (value == "integer" || value == "fractional_even" || value == "fractional_odd"))
        {
            result.partitioning = value == "integer"         ? tessellation_partitioning::integer
                                : value == "fractional_even" ? tessellation_partitioning::fractional_even
                                                             : tessellation_partitioning::fractional_odd;
            has_partitioning = true;
        }
        else if (key == "winding" && (value == "clockwise" || value == "counter_clockwise"))
        {
            result.is_clockwise = value == "clockwise";
            has_winding = true;
        }
        else
        {
            has_partitioning = false;
            break;
        }
    }
    // CHK-304: both are named, since every target compiles them into the stage's own text
    if (!has_partitioning || !has_winding)
        report(diagnostic_kind::invalid_attribute_arguments, file, a.name,
               "@tessellation_control takes `partitioning = .integer`, `.fractional_even` or `.fractional_odd`, and "
               "`winding = .clockwise` or `.counter_clockwise`, and may take `max_factor = N`");
    return result;
}

type_id checker::resolve_stream(i32 file, ast::expr_id expr, ast::index const& node, function_scope const* scope)
{
    auto const arguments = ast_of(file).at(node.arguments);
    auto const shape_name = text_of(file, span_of(file, node.object));
    // CHK-488: a mesh output may name a second struct, of what it writes per triangle
    auto const takes = shape_name == "mesh_output" && arguments.size() == 2 ? isize(2) : isize(1);
    auto is_plain = arguments.size() == takes;
    for (auto const& a : arguments)
        is_plain = is_plain && a.name.empty() && !a.is_splat;
    auto const element = is_plain ? resolve_value_type(file, arguments[0].value, scope) : error_type;
    if (element == error_type)
        return element;
    auto primitive = type_id::none;
    if (takes == 2)
    {
        primitive = resolve_value_type(file, arguments[1].value, scope);
        if (primitive == error_type)
            return error_type;
        if (out.at(primitive).kind != type_kind::structure || out.builtin_type_of(primitive) != nullptr)
        {
            report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, expr),
                   "a mesh output's second argument is the struct it writes per triangle: `mesh_output[corner, "
                   "face]`");
            return error_type;
        }
    }
    if (out.at(element).kind != type_kind::structure || out.builtin_type_of(element) != nullptr)
    {
        report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, expr),
               "a stream holds the struct a geometry stage hands the pixel stage: `triangle_stream[varyings]`");
        return error_type;
    }
    auto const shape = text_of(file, span_of(file, node.object));
    // CHK-481: a mesh stage's output is a stream of no primitive shape, written by index rather than appended
    auto const vertices = shape == "point_stream" ? 1 : shape == "line_stream" ? 2 : shape == "triangle_stream" ? 3 : 0;
    return resource_type({.kind = type_kind::stream,
                          .element = element,
                          .primitive = primitive,
                          .count = vertices,
                          .spelled = cc::format("{}[{}]", shape, out.name_of(element))});
}

void checker::judge_primitive_stage(symbol_id id, cc::function_ref<void(cc::string_view)> invalid)
{
    auto const& s = out.at(id);
    auto const& info = out.functions[s.info];
    auto const parameters = out.at(info.parameters);
    auto const name = stage_name(info.entry_stage);

    // the stage inputs, each of this stage, of its type and each once; every other parameter is judged by the stage below
    auto values = cc::vector<parameter const*>();
    auto seen = cc::vector<stage_input>();
    for (auto const& parameter : parameters)
    {
        if (parameter.input == stage_input::none)
        {
            values.push_back(&parameter);
            continue;
        }
        auto const& input = info_of(parameter.input);
        if (input.in_stage != info.entry_stage && (input.also_in & stage_bit(info.entry_stage)) == 0)
            invalid(cc::format("@{} is no input of the {} stage", input.name, name));
        // the domain's type is the factors struct's, which CHK-306 judges below
        else if (parameter.input != stage_input::domain_location && out.name_of(parameter.type) != input.type)
            invalid(cc::format("a @{} parameter is an {}", input.name, input.type));
        for (auto const other : seen)
            if (other == parameter.input)
                invalid(cc::format("@{} is taken twice", input.name));
        seen.push_back(parameter.input);
    }
    // CHK-481: a mesh stage takes stage inputs and its output, and hands the rasterizer nothing else
    if (info.entry_stage == stage::mesh)
    {
        if (info.result != checked_module::void_type)
            invalid("a @mesh fun returns nothing, and writes its vertices and triangles through its output");
        // CHK-486: the payload its task stage hands it, a struct, may stand before its output
        auto const* const output
            = !values.empty() && values.size() <= 2 && values.back() == &parameters.back() ? values.back() : nullptr;
        if (values.size() == 2
            && (out.at(values[0]->type).kind != type_kind::structure || out.builtin_type_of(values[0]->type) != nullptr))
            invalid("a @mesh fun takes, before its output, the payload struct its task stage hands it");
        auto const& t = output != nullptr ? out.at(output->type) : out.at(error_type);
        if (output == nullptr || t.kind != type_kind::stream || t.count != 0)
        {
            invalid("a @mesh fun takes stage inputs, and last its output: `out: mut mesh_output[varyings]`");
            return;
        }
        if (t.access != access_mode::read_write)
            invalid("a mesh stage writes its output, so the output is `mut`");
        auto positions = 0;
        for (auto const& m : out.at(out.at(t.element).members))
        {
            positions += m.is_position ? 1 : 0;
            if (m.is_per_draw)
                invalid(cc::format("what a mesh stage writes has the @per_draw member '{}', and only a vertex stage "
                                   "writes one yet",
                                   m.name));
        }
        if (positions != 1)
            invalid("what a mesh stage writes reaches the rasterizer, so it has one @position field");
        // CHK-488: what it writes per triangle reaches the pixel stage alone, flat
        if (is_valid(t.primitive))
            for (auto const& m : out.at(out.at(t.primitive).members))
                if (m.is_position || m.is_per_draw)
                    invalid(cc::format("what a mesh stage writes per triangle has no @position or @per_draw member, "
                                       "and '{}' is one",
                                       m.name));
        return;
    }

    // CHK-302, CHK-304, CHK-306: what the stage before hands on comes first, as a raster stage's stage struct does
    if (!parameters.empty() && parameters[0].input != stage_input::none)
    {
        invalid(cc::format("a @{} fun takes {} first, and stage inputs after it", name,
                           info.entry_stage == stage::geometry ? "its primitive's vertices" : "the patch"));
        return;
    }

    // an array of the struct the stage before hands on, as long as `lengths` allows
    auto const judge_array = [&](parameter const* p, cc::string_view what) -> type_id
    {
        auto const& t = p != nullptr ? out.at(p->type) : out.at(error_type);
        if (p == nullptr || t.kind != type_kind::array || out.at(t.element).kind != type_kind::structure
            || out.builtin_type_of(t.element) != nullptr)
        {
            invalid(
                cc::format("a @{} fun takes {} first: an array of the struct the stage before it returns", name, what));
            return error_type;
        }
        return p->type;
    };
    // CHK-304, CHK-306: both tessellation stages take the patch, which D3D and vulkan bound alike
    auto const judge_patch = [&](parameter const* p)
    {
        if (auto const patch = judge_array(p, "the patch");
            patch != error_type && (out.at(patch).count < 1 || out.at(patch).count > 32))
            invalid("a patch holds from 1 to 32 control points");
    };
    // a struct that reaches the rasterizer has one @position, of type hpos4
    auto const judge_position = [&](type_id link, cc::string_view what)
    {
        auto positions = 0;
        for (auto const& m : out.at(out.at(link).members))
            if (m.is_position)
            {
                ++positions;
                auto const* const record = out.builtin_type_of(m.type);
                if (record == nullptr || record->name != builtins::k_hpos4)
                    invalid(cc::format("the @position field of {} is an hpos4", what));
            }
        if (positions > 1)
            invalid(cc::format("{} has at most one @position field", what));
        return positions;
    };
    // CHK-393: the pass that proves a @per_draw value the same across a draw judges what a vertex stage returns alone
    auto const refuse_per_draw = [&](type_id link, cc::string_view what)
    {
        for (auto const& m : out.at(out.at(link).members))
            if (m.is_per_draw)
                invalid(cc::format("{} has the @per_draw member '{}', and only a vertex stage writes one yet", what,
                                   m.name));
    };

    if (info.entry_stage == stage::geometry)
    {
        // CHK-302: the primitive's vertices, then the stream it appends to
        if (info.result != checked_module::void_type)
            invalid("a @geometry fun returns nothing, and appends its vertices to its stream");
        if (values.size() != 2)
        {
            invalid("a @geometry fun takes its primitive's vertices, stage inputs, and last the stream it appends to");
            return;
        }
        if (auto const input = judge_array(values[0], "one primitive's vertices"); input != error_type)
        {
            auto const count = out.at(input).count;
            if (count != 1 && count != 2 && count != 3 && count != 4 && count != 6)
                invalid(cc::format("a primitive is 1, 2, 3, 4 or 6 vertices, and {} is none", out.name_of(input)));
        }
        auto const& stream = out.at(values[1]->type);
        if (stream.kind != type_kind::stream || values[1] != &parameters.back())
            invalid("the last parameter of a @geometry fun is the stream it appends to: `mut "
                    "triangle_stream[varyings]`");
        else if (stream.access != access_mode::read_write)
            invalid("a geometry stage appends to its stream, so the stream is `mut`");
        else if (judge_position(stream.element, "what a geometry stage appends") != 1)
            invalid("what a geometry stage appends reaches the rasterizer, so it has one @position field");
        else if (auto const scalars = scalars_of(out, stream.element);
                 info.max_vertices * scalars > k_max_geometry_scalars)
            invalid(cc::format("its {} vertices of {} scalars each are {}, and a geometry stage appends at most {} "
                               "scalars: `max_vertices` is at most {} for {}",
                               info.max_vertices, scalars, info.max_vertices * scalars, k_max_geometry_scalars,
                               k_max_geometry_scalars / scalars, out.name_of(stream.element)));
        if (stream.kind == type_kind::stream)
            refuse_per_draw(stream.element, "what a geometry stage appends");
        return;
    }

    // CHK-305: the factors a control stage returns, whose edges say the domain
    auto const judge_factors = [&](type_id factors) -> i32
    {
        auto edges = 0;
        auto domain = 0;
        auto insides = cc::vector<type_id>();
        for (auto const& m : out.at(out.at(factors).members))
        {
            if (m.factor == tessellation_factor::edge)
            {
                ++edges;
                auto const& t = out.at(m.type);
                auto const is_floats = t.kind == type_kind::array && out.name_of(t.element) == builtins::k_float;
                domain = is_floats && t.count >= 2 && t.count <= 4 ? t.count : 0;
            }
            if (m.factor == tessellation_factor::inside)
                insides.push_back(m.type);
        }
        if (edges != 1 || domain == 0)
        {
            invalid(cc::format("{} is a factors struct, with one @edge_factors member of float[2], float[3] or "
                               "float[4]",
                               out.name_of(factors)));
            return 0;
        }
        // a triangle has one inside factor, a quad two, and isolines none
        auto const wanted = domain == 3 ? cc::string_view("float")
                          : domain == 4 ? cc::string_view("float[2]")
                                        : cc::string_view();
        if (insides.size() != (domain == 2 ? 0 : 1) || (!insides.empty() && out.name_of(insides[0]) != wanted))
        {
            invalid(domain == 2
                        ? cc::string("isolines have no @inside_factors member")
                        : cc::format("a domain of {} edges has one @inside_factors member, of {}", domain, wanted));
            return 0;
        }
        return domain;
    };

    if (info.entry_stage == stage::tessellation_control)
    {
        // CHK-304: the patch, and the factors it returns; the control points pass through as they are
        if (values.size() != 1)
        {
            invalid("a @tessellation_control fun takes the patch, and stage inputs");
            return;
        }
        judge_patch(values[0]);
        if (out.at(info.result).kind != type_kind::structure || out.builtin_type_of(info.result) != nullptr)
            invalid("a @tessellation_control fun returns its patch's factors struct");
        else
            (void)judge_factors(info.result);
        return;
    }

    // CHK-306: the patch, its factors, and where in the domain it runs
    if (values.size() != 2)
    {
        invalid("a @tessellation_evaluation fun takes the patch, the factors struct, and `@domain_location`");
        return;
    }
    judge_patch(values[0]);
    auto const domain = out.at(values[1]->type).kind == type_kind::structure ? judge_factors(values[1]->type) : 0;
    auto has_location = false;
    for (auto const& parameter : parameters)
        if (parameter.input == stage_input::domain_location)
        {
            has_location = true;
            auto const wanted = domain == 3 ? cc::string_view("float3") : cc::string_view("float2");
            if (domain != 0 && out.name_of(parameter.type) != wanted)
                invalid(cc::format("the domain location of {} is a {}",
                                   domain == 3 ? "a triangle" : "quads and isolines", wanted));
        }
    if (!has_location)
        invalid("a @tessellation_evaluation fun takes `@domain_location`, where in the domain it runs");
    if (out.at(info.result).kind != type_kind::structure)
        invalid("a @tessellation_evaluation fun returns the struct the stage after it takes");
    else
    {
        (void)judge_position(info.result, "what a tessellation evaluation stage returns");
        refuse_per_draw(info.result, "what a tessellation evaluation stage returns");
    }
}

type_id checker::check_stream_call(function_scope& scope, ast::expr_id id, ast::call const& call, type_id stream)
{
    auto const file = scope.file;
    auto const& member = ast_of(file).at(call.callee).node.as<ast::member>();
    auto const name = text_of(file, member.name);
    auto const arguments = ast_of(file).at(call.arguments);
    auto const& t = out.at(stream);
    auto const where = span_of(file, id);

    if (t.count == 0)
        return check_mesh_vertex(scope, id, call, stream);
    auto const is_emit = name == "emit";
    if (!is_emit && name != "end_strip")
    {
        report(diagnostic_kind::unknown_member, file, member.name,
               cc::format("{} has no member {}; a stream has `emit(v)` and `end_strip()`", out.name_of(stream), name));
        return error_type;
    }
    auto const is_plain
        = arguments.size() == (is_emit ? 1 : 0) && (!is_emit || (arguments[0].name.empty() && !arguments[0].is_splat));
    if (!is_plain)
    {
        report(diagnostic_kind::no_matching_overload, file, where,
               is_emit ? cc::format("emit takes the one vertex it appends, a {}", out.name_of(t.element))
                       : cc::string("end_strip takes nothing"));
        return error_type;
    }
    if (is_emit && check_expected(scope, arguments[0].value, t.element, "the vertex it appends") != t.element)
        return error_type;

    // the builtin of the stream's shape, which a vertex of any type is handed to
    auto callee = symbol_id::none;
    for (auto const candidate : candidates_of(file, name, stream))
    {
        if (demand(candidate, file, where) != symbol_state::checked)
            continue;
        auto const parameters = out.at(out.functions[out.at(candidate).info].parameters);
        if (!parameters.empty() && takes(parameters[0].type, stream))
            callee = candidate;
    }
    if (!is_valid(callee))
    {
        report(diagnostic_kind::unknown_builtin, file, where, cc::format("{} of {}", name, out.name_of(stream)));
        return error_type;
    }
    auto recorded = call_arguments();
    recorded.written.push_back({.expr = member.object});
    if (is_emit)
        recorded.written.push_back({.expr = arguments[0].value});
    i32 const slots[] = {0};
    record_call(file, id, callee, recorded, slots);
    set_target(file, call.callee, {.kind = target_kind::overload, .symbol = callee});
    set_target(file, id, {.kind = target_kind::overload, .symbol = callee});
    return checked_module::void_type;
}

type_id checker::check_mesh_vertex(function_scope& scope, ast::expr_id id, ast::call const& call, type_id output)
{
    auto const file = scope.file;
    auto const& member = ast_of(file).at(call.callee).node.as<ast::member>();
    auto const arguments = ast_of(file).at(call.arguments);
    auto const& t = out.at(output);
    auto const where = span_of(file, id);

    // CHK-482: the vertex at an index, of the output's own type; CHK-488: a primitive's values, of its second
    auto const name = text_of(file, member.name);
    auto const is_primitive = name == "primitive";
    auto const written_type = is_primitive ? t.primitive : t.element;
    if (is_primitive && !is_valid(written_type))
    {
        report(diagnostic_kind::unknown_member, file, member.name,
               cc::format("{} writes no values per triangle; `mesh_output[{}, face]` names the struct it would",
                          out.name_of(output), out.name_of(t.element)));
        return error_type;
    }
    auto const is_plain = arguments.size() == 2 && arguments[0].name.empty() && !arguments[0].is_splat
                       && arguments[1].name.empty() && !arguments[1].is_splat;
    if (!is_plain)
    {
        report(diagnostic_kind::no_matching_overload, file, where,
               cc::format("{} takes an index and the value written there, a {}", name, out.name_of(written_type)));
        return error_type;
    }
    auto const index_type = type_of_builtin(builtins::k_uint, file, where);
    if (check_expected(scope, arguments[0].value, index_type, "the index it writes at") != index_type)
        return error_type;
    if (check_expected(scope, arguments[1].value, written_type, "the value it writes") != written_type)
        return error_type;

    auto callee = symbol_id::none;
    for (auto const candidate : candidates_of(file, name, output))
    {
        if (demand(candidate, file, where) != symbol_state::checked)
            continue;
        auto const parameters = out.at(out.functions[out.at(candidate).info].parameters);
        if (!parameters.empty() && takes(parameters[0].type, output))
            callee = candidate;
    }
    if (!is_valid(callee))
    {
        report(diagnostic_kind::unknown_builtin, file, where, cc::format("{} of {}", name, out.name_of(output)));
        return error_type;
    }
    auto recorded = call_arguments();
    recorded.written.push_back({.expr = member.object});
    recorded.written.push_back({.expr = arguments[0].value});
    recorded.written.push_back({.expr = arguments[1].value});
    i32 const slots[] = {0, 1};
    record_call(file, id, callee, recorded, slots);
    set_target(file, call.callee, {.kind = target_kind::overload, .symbol = callee});
    set_target(file, id, {.kind = target_kind::overload, .symbol = callee});
    return checked_module::void_type;
}

type_id checker::check_dispatch_mesh(function_scope& scope, ast::expr_id id, ast::call const& call)
{
    auto const file = scope.file;
    auto const arguments = ast_of(file).at(call.arguments);
    auto const where = span_of(file, id);
    auto is_plain = arguments.size() == 4;
    for (auto const& a : arguments)
        is_plain = is_plain && a.name.empty() && !a.is_splat;
    if (!is_plain)
    {
        report(diagnostic_kind::no_matching_overload, file, where,
               "dispatch_mesh takes three workgroup counts and the payload it hands them: `dispatch_mesh(2u, 1u, 1u, "
               "p)`");
        return error_type;
    }
    auto const count_type = type_of_builtin(builtins::k_uint, file, where);
    for (auto i = 0; i < 3; ++i)
        if (check_expected(scope, arguments[i].value, count_type, "a count of mesh workgroups") != count_type)
            return error_type;
    // CHK-485: the payload is a struct of the program, handed to every mesh workgroup it launches
    auto const payload = check_expr(scope, arguments[3].value);
    if (payload == error_type)
        return error_type;
    if (out.at(payload).kind != type_kind::structure || out.builtin_type_of(payload) != nullptr)
    {
        report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, arguments[3].value),
               cc::format("a payload is a struct of the program, and {} is none", out.name_of(payload)));
        return error_type;
    }

    auto callee = symbol_id::none;
    for (auto const candidate : candidates_of(file, "dispatch_mesh", count_type))
        if (demand(candidate, file, where) == symbol_state::checked)
            callee = candidate;
    if (!is_valid(callee))
    {
        report(diagnostic_kind::unknown_builtin, file, where, "dispatch_mesh");
        return error_type;
    }
    // what the entry point hands on, which a pipeline holds against what its mesh stage takes (CHK-487)
    if (is_valid(scope.function) && out.at(scope.function).info >= 0)
    {
        auto& info = out.functions.own(out.at(scope.function).info);
        if (info.entry_stage == stage::task)
        {
            if (is_valid(info.mesh_payload) && info.mesh_payload != payload)
                report(diagnostic_kind::invalid_entry_point, file, where,
                       "a task stage hands its mesh stage one payload struct");
            info.mesh_payload = payload;
        }
    }
    auto recorded = call_arguments();
    for (auto const& a : arguments)
        recorded.written.push_back({.expr = a.value});
    i32 const slots[] = {0, 1, 2};
    record_call(file, id, callee, recorded, slots);
    set_target(file, call.callee, {.kind = target_kind::overload, .symbol = callee});
    set_target(file, id, {.kind = target_kind::overload, .symbol = callee});
    return checked_module::void_type;
}

void checker::judge_mesh_payloads()
{
    for (auto i = isize(0); i < out.pipelines.size(); ++i)
    {
        auto const& p = out.pipelines[i];
        if (!is_valid(p.task) || !is_valid(p.mesh))
            continue;
        auto taken = type_id::none;
        for (auto const& parameter : out.at(out.functions[out.at(p.mesh).info].parameters))
            if (parameter.input == stage_input::none && out.at(parameter.type).kind != type_kind::stream)
                taken = parameter.type;
        auto const handed = out.functions[out.at(p.task).info].mesh_payload;
        if (handed == taken)
            continue;
        auto const& s = out.at(p.symbol);
        auto const where = ast_of(s.file).at(s.declaration).node.as<ast::pipeline_decl>().name;
        auto const detail = !is_valid(handed) ? cc::format("{} hands its mesh stage no payload, and {} takes {}",
                                                           out.at(p.task).name, out.at(p.mesh).name, out.name_of(taken))
                          : !is_valid(taken)
                              ? cc::format("{} hands its mesh stage {}, and {} takes none", out.at(p.task).name,
                                           out.name_of(handed), out.at(p.mesh).name)
                              : cc::format("{} hands its mesh stage {}, and {} takes {}", out.at(p.task).name,
                                           out.name_of(handed), out.at(p.mesh).name, out.name_of(taken));
        report(diagnostic_kind::invalid_pipeline, s.file, where, detail);
    }
}
