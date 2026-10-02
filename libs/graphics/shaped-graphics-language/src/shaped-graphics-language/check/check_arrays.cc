#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>
#include <shaped-graphics-language/check/resources.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// Fixed-size arrays (the spec's checking file, "Arrays"): the type `T[N]`, its literal, its elements and its length.

namespace
{
constexpr auto error_type = checked_module::error_type;
} // namespace

type_id checker::array_type(type_id element, i32 count)
{
    // written outermost first, `float[3, 5]` for three arrays of five, as a type position reads it
    auto base = element;
    auto dimensions = count > 0 ? cc::format("{}", count) : cc::string();
    while (out.at(base).kind == type_kind::array)
    {
        auto const inner = out.at(base).count;
        dimensions += inner > 0 ? cc::format(", {}", inner) : cc::string(", ");
        base = out.at(base).element;
    }
    return resource_type({.kind = type_kind::array,
                          .element = element,
                          .count = count,
                          .spelled = cc::format("{}[{}]", out.name_of(base), dimensions)});
}

bool checker::is_type_name(i32 file, ast::expr_id expr) const
{
    auto const& e = ast_of(file).at(expr);
    // `texture_2d[float4][64]`, or a second group that `resolve_array` refuses by name
    if (e.node.is<ast::index>())
        return true;
    auto const* const n = e.node.try_as<ast::name>();
    if (n != nullptr)
    {
        auto const text = text_of(file, n->where);
        if (text == "sampler" || text == "comparison_sampler")
            return true;
        for (auto const& shape : k_shapes)
            if (shape.depth == text)
                return true;
    }
    // CHK-348: a type of a module, `m.name`, as well as one of the file
    auto const* const found = symbols_named(file, expr, nullptr);
    if (found == nullptr)
        return false;
    auto const kind = out.at(found->front()).kind;
    return kind == symbol_kind::structure || kind == symbol_kind::enumeration;
}

cc::optional<i32> checker::constant_count(i32 file, ast::expr_id expr)
{
    auto const& e = ast_of(file).at(expr);
    auto const text = text_of(file, span_of(file, expr));
    if (e.node.is<ast::literal>())
        return classify_number(text) == number_class::plain_integer ? parse_plain_integer(text) : cc::optional<i32>();
    auto const* const n = e.node.try_as<ast::name>();
    if (n == nullptr)
        return {};
    auto const* const found = names_seen_from(file).get_ptr(text);
    if (found == nullptr || found->empty() || out.at(found->front()).kind != symbol_kind::constant)
        return {};
    auto const id = found->front();
    set_target(file, expr, {.kind = target_kind::symbol, .symbol = id});
    if (demand(id, file, span_of(file, expr)) != symbol_state::checked)
        return {};
    auto const& c = out.constants[out.at(id).info];
    if (c.kind != constant_kind::integer)
        return {};
    note_option(file, span_of(file, expr), c);
    return c.integer;
}

cc::optional<i32> checker::constant_index(i32 file, ast::expr_id expr) const
{
    auto const& e = ast_of(file).at(expr);
    if (e.node.is<ast::literal>())
    {
        auto const text = text_of(file, span_of(file, expr));
        return classify_number(text) == number_class::plain_integer ? parse_plain_integer(text) : cc::optional<i32>();
    }
    // the name already checked, so what it names is known, and a local that hides a `const` is no constant
    if (!e.node.is<ast::name>())
        return {};
    auto const& where = out.files[file].target_at(expr);
    if (where.kind != target_kind::symbol || out.at(where.symbol).kind != symbol_kind::constant
        || out.at(where.symbol).state != symbol_state::checked)
        return {};
    auto const& c = out.constants[out.at(where.symbol).info];
    return c.kind == constant_kind::integer ? cc::optional<i32>(c.integer) : cc::optional<i32>();
}

type_id checker::resolve_array(i32 file, ast::expr_id expr, ast::index const& node, function_scope const* scope)
{
    auto const where = span_of(file, expr);
    auto const& ast = ast_of(file);

    // CHK-285: `float[3][5]` would read as five arrays of three to a C reader, so one group holds every dimension
    if (auto const* const inner = ast::is_valid(node.object) ? ast.at(node.object).node.try_as<ast::index>() : nullptr)
        if (!is_named(file, inner->object, "buffer") && !is_named(file, inner->object, "atomic")
            && resolve_resource_applied(file, node.object, *inner) == type_id::none)
        {
            report(diagnostic_kind::wrong_kind_of_name, file, where,
                   "an array of arrays is one group of dimensions, outermost first: `float[3, 5]`");
            return checked_module::error_type;
        }

    auto const element = resolve_type(file, node.object, scope);
    if (element == checked_module::error_type)
        return element;
    if (element == checked_module::void_type)
    {
        report(diagnostic_kind::type_mismatch, file, where, "an array of void holds nothing");
        return checked_module::error_type;
    }

    auto const arguments = ast.at(node.arguments);
    // CHK-286: `T[]` is a binding array whose length the host binds, which only a binding member may be
    if (arguments.empty())
        return array_type(element, 0);

    auto counts = cc::vector<i32>();
    for (auto const& a : arguments)
    {
        auto const count = !a.name.empty() || a.is_splat || !ast::is_valid(a.value) ? cc::optional<i32>()
                                                                                    : constant_count(file, a.value);
        if (!count.has_value() || count.value() < 1)
        {
            report(diagnostic_kind::invalid_constant_argument, file,
                   ast::is_valid(a.value) ? span_of(file, a.value) : where,
                   "an array's length is an int constant of at least 1: a literal or a `const`");
            return checked_module::error_type;
        }
        counts.push_back(count.value());
    }
    // the last dimension is the innermost
    auto result = element;
    for (auto i = counts.size(); i-- > 0;)
        result = array_type(result, counts[i]);
    return result;
}

type_id checker::check_array_literal(function_scope& scope, ast::expr_id expr, type_id to)
{
    auto const file = scope.file;
    auto const where = span_of(file, expr);
    auto const elements = ast_of(file).at(ast_of(file).at(expr).node.as<ast::array>().elements);
    for (auto const& e : elements)
        if (!e.name.empty() || e.is_splat || !ast::is_valid(e.value))
        {
            report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, e.form),
                   "an array literal holds values alone: no name and no splat");
            return error_type;
        }
    if (elements.empty())
    {
        report(diagnostic_kind::type_mismatch, file, where, "an array holds at least one element");
        return error_type;
    }

    // CHK-290: where an array is expected, every element is what it holds, and there are exactly as many
    if (is_valid(to))
    {
        auto const& t = out.at(to);
        if (t.count != elements.size())
        {
            report(
                diagnostic_kind::type_mismatch, file, where,
                cc::format("{} holds {} elements, and this literal has {}", out.name_of(to), t.count, elements.size()));
            return error_type;
        }
        auto is_sound = true;
        for (auto const& e : elements)
            is_sound = check_expected(scope, e.value, t.element) == t.element && is_sound;
        if (!is_sound)
            return error_type;
        set_type(file, expr, to);
        return to;
    }

    // and elsewhere it is an array of its first element's type
    auto const first = check_expr(scope, elements[0].value);
    if (first == error_type)
        return error_type;
    if (out.at(first).kind == type_kind::void_)
    {
        report(diagnostic_kind::type_mismatch, file, span_of(file, elements[0].value), "an array of void holds nothing");
        return error_type;
    }
    auto is_sound = true;
    for (auto i = isize(1); i < elements.size(); ++i)
        is_sound = check_expected(scope, elements[i].value, first, "an element of this array") == first && is_sound;
    if (!is_sound)
        return error_type;
    auto const result = array_type(first, i32(elements.size()));
    set_type(file, expr, result);
    return result;
}

type_id checker::check_filled(function_scope& scope, ast::expr_id id, ast::call const& call)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const& member = ast.at(call.callee).node.as<ast::member>();
    auto const type = resolve_type(file, member.object, &scope);
    if (type == error_type)
    {
        (void)check_arguments(scope, call.arguments, false);
        return error_type;
    }
    auto const where = span_of(file, id);
    if (out.at(type).kind != type_kind::array || text_of(file, member.name) != "filled")
    {
        (void)check_arguments(scope, call.arguments, false);
        report(diagnostic_kind::unknown_member, file, member.name,
               cc::format("{} has no function {}; an array type's one function is `filled`", out.name_of(type),
                          text_of(file, member.name)));
        return error_type;
    }
    auto const arguments = ast.at(call.arguments);
    if (arguments.size() != 1 || !arguments[0].name.empty() || arguments[0].is_splat)
    {
        report(diagnostic_kind::no_matching_overload, file, where,
               cc::format("{}.filled takes the one value every element holds", out.name_of(type)));
        return error_type;
    }
    if (check_expected(scope, arguments[0].value, out.at(type).element, "the value it is filled with")
        != out.at(type).element)
        return error_type;
    set_target(file, id, {.kind = target_kind::array_filled});
    return type;
}

bool checker::holds_resource(type_id type) const
{
    auto const& t = out.at(type);
    if (is_resource(t.kind))
        return true;
    if (t.kind == type_kind::array)
        return holds_resource(t.element);
    for (auto const& m : out.at(t.members))
        if (m.type != type && holds_resource(m.type))
            return true;
    return false;
}

cc::string checker::array_path(type_id type) const
{
    if (type == checked_module::error_type || out.builtin_type_of(type) != nullptr)
        return {};
    auto const& t = out.at(type);
    if (t.kind == type_kind::array)
        return cc::format(": {}", out.name_of(type));
    if (t.kind != type_kind::structure)
        return {};
    for (auto const& m : out.at(t.members))
    {
        if (m.type == type || m.factor != tessellation_factor::none)
            continue;
        if (auto inner = array_path(m.type); !inner.empty())
            return cc::format(".{}{}", m.name, inner);
    }
    return {};
}

void checker::judge_edge_arrays(i32 file, source_span where, type_id type)
{
    // a patch and a geometry stage's vertices are arrays of the struct that crosses, and a stream holds it
    while (type != checked_module::error_type && out.builtin_type_of(type) == nullptr
           && (out.at(type).kind == type_kind::array || out.at(type).kind == type_kind::stream))
        type = out.at(type).element;
    if (type == checked_module::error_type || out.builtin_type_of(type) != nullptr
        || out.at(type).kind != type_kind::structure)
        return;
    if (auto const path = array_path(type); !path.empty())
        unsupported(file, where,
                    cc::format("an array crossing a stage edge, in {}{}, which takes a location per element no target "
                               "gives yet",
                               out.name_of(type), path));
}

i32 checker::workgroup_size_of(type_id type) const
{
    // a type's size, rounded up to its alignment, as WGSL strides an array of it
    struct measured
    {
        i32 size = 0;
        i32 alignment = 1;
    };
    auto const round_up = [](i32 n, i32 to) { return (n + to - 1) / to * to; };
    auto const measure = [&](auto const& self, type_id t) -> measured
    {
        if (auto const* const record = out.builtin_type_of(t))
        {
            // a bool has no place in a host's block, and takes four bytes where the shader alone holds it
            if (record->wgsl_layout.size == 0)
                return {.size = 4 * record->leaf_count, .alignment = 4};
            return {.size = record->wgsl_layout.size, .alignment = record->wgsl_layout.alignment};
        }
        auto const& info = out.at(t);
        // an atomic holds a `uint` or an `int` (CHK-296)
        if (info.kind == type_kind::enumeration || info.kind == type_kind::atomic)
            return {.size = 4, .alignment = 4};
        if (info.kind == type_kind::array)
        {
            auto const element = self(self, info.element);
            return {.size = round_up(element.size, element.alignment) * info.count, .alignment = element.alignment};
        }
        auto result = measured();
        for (auto const& m : out.at(info.members))
        {
            auto const member = self(self, m.type);
            result.size = round_up(result.size, member.alignment) + member.size;
            result.alignment = member.alignment > result.alignment ? member.alignment : result.alignment;
        }
        result.size = round_up(result.size, result.alignment);
        return result;
    };
    return measure(measure, type).size;
}

i32 checker::storage_size_of(type_id type) const
{
    struct measured
    {
        i32 size = 0;
        i32 alignment = 1;
    };
    auto const round_up = [](i32 n, i32 to) { return (n + to - 1) / to * to; };
    auto const measure = [&](auto const& self, type_id t) -> measured
    {
        if (auto const* const record = out.builtin_type_of(t))
            return {.size = record->hlsl_layout.size, .alignment = is_16_bit(record->leaf_kind) ? 2 : 4};
        auto const& info = out.at(t);
        if (info.kind != type_kind::structure)
            return {.size = 4, .alignment = 4};
        auto result = measured();
        for (auto const& m : out.at(info.members))
        {
            if (m.type == checked_module::void_type || m.type == checked_module::error_type)
                continue;
            auto const member = self(self, m.type);
            result.size = round_up(result.size, member.alignment) + member.size;
            result.alignment = member.alignment > result.alignment ? member.alignment : result.alignment;
        }
        result.size = round_up(result.size, result.alignment);
        return result;
    };
    return measure(measure, type).size;
}

bool checker::holds_atomic(type_id type) const
{
    auto const& t = out.at(type);
    return t.kind == type_kind::atomic || (t.kind == type_kind::array && holds_atomic(t.element));
}

type_id checker::resolve_atomic(i32 file, ast::expr_id expr, ast::index const& node, function_scope const* scope)
{
    auto const arguments = ast_of(file).at(node.arguments);
    auto const element = arguments.size() == 1 && arguments[0].name.empty() && !arguments[0].is_splat
                           ? resolve_type(file, arguments[0].value, scope)
                           : checked_module::error_type;
    auto const name = element == checked_module::error_type ? cc::string_view() : out.name_of(element);
    // CHK-296: the widths and kinds every target has atomics of
    if (name != builtins::k_uint && name != builtins::k_int)
    {
        report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, expr),
               "an atomic holds a `uint` or an `int`: `atomic[uint]`");
        return checked_module::error_type;
    }
    return resource_type({.kind = type_kind::atomic, .element = element});
}

bool checker::judge_atomic_use(i32 file, ast::expr_id id, type_id type)
{
    if (type == checked_module::error_type || !holds_atomic(type) || id == subscripted)
        return true;
    for (auto const h : handed)
        if (h == id && out.at(type).kind == type_kind::atomic)
            return true;
    report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, id),
           "an atomic is read by `.load()` and written by `.store(v)` or one of its updates, and is never a value");
    return false;
}
