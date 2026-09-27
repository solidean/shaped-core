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
    // CHK-301: what HLSL's `maxvertexcount` takes, which the targets that have the stage share
    if (!n.has_value() || n.value() < 1 || n.value() > 1024)
    {
        report(diagnostic_kind::invalid_attribute_arguments, file, a.name,
               "@geometry takes how many vertices it appends at most, an int from 1 to 1024: `@geometry(max_vertices = "
               "6)`");
        return 1;
    }
    return n.value();
}

checker::tessellation_mode checker::tessellation_of(i32 file, ast::attribute const& a)
{
    auto result = tessellation_mode();
    auto has_partitioning = false;
    auto has_winding = false;
    for (auto const& argument : ast_of(file).at(a.arguments))
    {
        auto const key = text_of(file, argument.name);
        // the leading-dot case the argument names, or nothing
        auto const* const dot
            = ast::is_valid(argument.value) ? ast_of(file).at(argument.value).node.try_as<ast::leading_dot>() : nullptr;
        auto const value = dot != nullptr ? text_of(file, dot->name) : cc::string_view();
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
               "`winding = .clockwise` or `.counter_clockwise`");
    return result;
}

type_id checker::resolve_stream(i32 file, ast::expr_id expr, ast::index const& node, function_scope const* scope)
{
    auto const arguments = ast_of(file).at(node.arguments);
    auto const element = arguments.size() == 1 && arguments[0].name.empty() && !arguments[0].is_splat
                           ? resolve_value_type(file, arguments[0].value, scope)
                           : error_type;
    if (element == error_type)
        return element;
    if (out.at(element).kind != type_kind::structure || out.builtin_type_of(element) != nullptr)
    {
        report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, expr),
               "a stream holds the struct a geometry stage hands the pixel stage: `triangle_stream[varyings]`");
        return error_type;
    }
    auto const shape = text_of(file, span_of(file, node.object));
    auto const vertices = shape == "point_stream" ? 1 : shape == "line_stream" ? 2 : 3;
    return resource_type({.kind = type_kind::stream,
                          .element = element,
                          .count = vertices,
                          .spelled = cc::format("{}[{}]", shape, out.name_of(element))});
}

void checker::judge_primitive_stage(symbol_id id, cc::function_ref<void(cc::string_view)> invalid)
{
    auto const& s = out.at(id);
    auto const& info = out.functions[s.info];
    auto const parameters = out.at(info.parameters);
    auto const name = stage_name(info.entry_stage);

    // the stage inputs, each of this stage and each once; every other parameter is judged by the stage below
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
        for (auto const other : seen)
            if (other == parameter.input)
                invalid(cc::format("@{} is taken twice", input.name));
        seen.push_back(parameter.input);
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
        if (auto const patch = judge_array(values[0], "the patch");
            patch != error_type && (out.at(patch).count < 1 || out.at(patch).count > 32))
            invalid("a patch holds from 1 to 32 control points");
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
    (void)judge_array(values[0], "the patch");
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
        (void)judge_position(info.result, "what a tessellation evaluation stage returns");
}

type_id checker::check_stream_call(function_scope& scope, ast::expr_id id, ast::call const& call, type_id stream)
{
    auto const file = scope.file;
    auto const& member = ast_of(file).at(call.callee).node.as<ast::member>();
    auto const name = text_of(file, member.name);
    auto const arguments = ast_of(file).at(call.arguments);
    auto const& t = out.at(stream);
    auto const where = span_of(file, id);

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
