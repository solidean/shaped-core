#include <clean-core/common/utility.hh>
#include <clean-core/sequence/sequence.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/impl/checker.hh>
#include <shaped-graphics-language/check/impl/expr_helpers.hh>
#include <shaped-graphics-language/check/resources.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
constexpr auto error_type = checked_module::error_type;
constexpr auto void_type = checked_module::void_type;
} // namespace

// ---- bodies ---------------------------------------------------------------------------------------------------------

void checker::check_body(symbol_id id)
{
    // CHK-338: a generic body sees its type parameters, as its signature did, and with them open only its names are
    // judged; each instantiation checks it again with what they stand for
    auto const info = out.at(id).info;
    auto const before = type_parameter_names.size();
    auto const is_generic = info >= 0 && !out.functions[info].type_parameters.empty();
    if (info >= 0)
        for (auto const p : out.at(out.functions[info].type_parameters))
            type_parameter_names.push_back({out.at(p).spelled, p});
    // an instance being checked is no place for its callee's generic pass, which would read its tables
    auto const outer_instance = current_instance;
    current_instance = -1;
    if (is_generic)
        ++generic_passes;
    check_body_of(id);
    if (is_generic)
        --generic_passes;
    current_instance = outer_instance;
    type_parameter_names.resize_down_to(before);
}

void checker::check_body_of(symbol_id id, i32 instance)
{
    auto const file = out.at(id).file;
    auto const& ast = ast_of(file);
    auto const index = out.at(id).info;
    if (instance < 0 && notes[index].is_body_checked)
        return;
    auto const& node = ast.at(out.at(id).declaration).node;
    auto const* const f = node.try_as<ast::fun_decl>();
    auto const* const property = node.try_as<ast::property_decl>();
    if (f == nullptr && property == nullptr)
        return;
    auto const& body = f != nullptr ? f->body : property->body;
    auto const name = f != nullptr ? f->name : property->name;
    if (body.kind == ast::body_kind::none)
        return;
    if (instance < 0)
        notes[index].is_body_checked = true;

    // by value: checking the body may compile another function, and `functions` then moves
    auto const info = out.functions[index];
    // an instance reads its signature with what its type parameters stand for
    auto const bindings = instance >= 0
                            ? cc::vector<type_id>::create_copy_of(out.at(out.function_instances[instance].bindings))
                            : cc::vector<type_id>();
    auto const declared_result = substitute(info.result, bindings);
    auto scope = function_scope{.function = id, .file = file, .result = declared_result};
    // CHK-245: `self` is the receiver of a method and of a property, and its members are read through it alone (CHK-62)
    auto const role = out.at(id).role;
    auto const has_receiver
        = role == function_role::property || (role == function_role::method && f->receiver == ast::receiver_kind::self);
    auto const parameters = out.at(info.parameters);
    for (auto i = isize(0); i < parameters.size(); ++i)
    {
        auto const& p = parameters[i];
        auto const type = substitute(p.type, bindings);
        if (has_receiver && i == 0)
        {
            scope.receiver = type;
            scope.locals.push_back({.name = "self", .where = {.kind = target_kind::receiver}, .type = type});
            continue;
        }
        // CHK-54: a parameter may have the name of a module-level symbol, which it hides in the body.
        if (instance < 0)
            judge_shadowing(file, text_of(file, ast.at(p.field).name), ast.at(p.field).name);
        scope.locals.push_back({
            .name = text_of(file, ast.at(p.field).name),
            .where = {.kind = target_kind::parameter, .index = i32(p.field)},
            .type = type,
            .is_mut = p.is_mut,
        });
    }

    auto const errors_before = error_events;
    // what the body found: an inferred result, and whether it checked without an error
    auto const finish = [&](type_id inferred, bool sets_result)
    {
        auto const is_sound = error_events == errors_before;
        if (instance >= 0)
        {
            auto& own = out.function_instances.own(instance);
            own.result = sets_result ? inferred : declared_result;
            own.is_body_sound = is_sound;
            // an instance that fails fails its function, whose callers then build no tree
            if (!is_sound)
                notes[index].is_body_sound = false;
            return;
        }
        if (sets_result)
            out.functions.own(index).result = inferred;
        notes[index].is_body_sound = is_sound;
    };

    // A property's `=>:` block is a value block: it has a value only through `yield` (AST-81).
    if (property != nullptr && !ast::is_valid(body.value))
    {
        auto const infers = notes[index].infers_result;
        scope.value_blocks.push_back({.value = infers ? type_id::none : info.result, .is_expected = !infers});
        auto const ending = check_statements(scope, body.statements);
        auto const yielded = scope.value_blocks.back();
        scope.value_blocks.remove_back();
        // CHK-236: a property has a value on every path, as a function that returns one does (CHK-125)
        if (ending == flow::falls_through)
            report(diagnostic_kind::missing_return, file, name,
                   cc::format("{} is a property, and a path through its block ends without a yield", out.at(id).name));
        finish(yielded.has_yield && ending != flow::falls_through ? yielded.value : error_type, infers);
        return;
    }

    // CHK-427: the bodies in a target's own text a function's body may hold, read once whatever instances follow
    if (f != nullptr && !planned_text_functions.values().contains(id))
    {
        planned_text_functions.push_back(id);
        plan_target_text(id, file, body.statements);
        judge_text_effects(id, file, ast.at(out.at(id).declaration).attributes);
    }
    auto ending = check_statements(scope, body.statements);
    auto inferred = type_id::none;
    if (ast::is_valid(body.value) && notes[index].infers_result)
    {
        inferred = check_expr(scope, body.value);
        ending = flow::exits;
    }
    else if (ast::is_valid(body.value))
    {
        check_return(scope, span_of(file, body.value), body.value);
        ending = flow::exits;
    }
    // a block without a statement was reported where it was parsed
    auto const has_statements = !body.statements.empty() || ast::is_valid(body.value);
    if (has_statements && ending == flow::falls_through && declared_result != void_type && declared_result != error_type)
        report(diagnostic_kind::missing_return, file, name,
               cc::format("{} returns {}, and a path through its body ends without a return", out.at(id).name,
                          out.name_of(declared_result)));

    finish(inferred, ast::is_valid(body.value) && notes[index].infers_result);
}

void checker::check_defaults(symbol_id id)
{
    auto const file = out.at(id).file;
    auto const& ast = ast_of(file);
    auto const index = out.at(id).info;
    // by value: checking a default may compile another function, and `functions` then moves
    auto const info = out.functions[index];
    auto has_default = false;
    for (auto const& p : out.at(info.parameters))
        has_default = has_default || (ast::is_valid(p.field) && p.has_default);
    if (!has_default)
    {
        notes[index].are_defaults_sound = true;
        return;
    }
    auto scope = function_scope{.function = id, .file = file, .result = info.result};
    // CHK-243: a method's `self` is a parameter before every default
    auto const* const f = ast.at(out.at(id).declaration).node.try_as<ast::fun_decl>();
    auto const has_receiver
        = out.at(id).role == function_role::method && f != nullptr && f->receiver == ast::receiver_kind::self;
    auto const errors_before = error_count();
    auto const parameters = out.at(info.parameters);
    for (auto i = isize(0); i < parameters.size(); ++i)
    {
        auto const& p = parameters[i];
        if (ast::is_valid(p.field) && p.has_default)
            (void)check_expected(scope, ast.at(p.field).default_value, p.type, cc::format("the default of {}", p.name));
        // a parameter is visible to the defaults after it
        if (has_receiver && i == 0)
        {
            scope.receiver = p.type;
            scope.locals.push_back({.name = "self", .where = {.kind = target_kind::receiver}, .type = p.type});
        }
        else if (ast::is_valid(p.field))
            scope.locals.push_back({
                .name = p.name,
                .where = {.kind = target_kind::parameter, .index = i32(p.field)},
                .type = p.type,
            });
    }
    notes[index].are_defaults_sound = error_count() == errors_before;
}

type_id checker::check_index(function_scope& scope, ast::expr_id id, ast::index const& node)
{
    auto const file = scope.file;
    auto const where = span_of(file, id);

    auto const outer = subscripted;
    subscripted = node.object;
    // CHK-367: an image is handed to the `load` or the `store` its subscript stands for
    handed.push_back(node.object);
    auto const object = check_expr(scope, node.object);
    handed.remove_back();
    subscripted = outer;
    if (object == error_type)
        return error_type;
    auto const kind = out.at(object).kind;
    if (kind == type_kind::image || kind == type_kind::texture)
        return check_texel(scope, id, node, object);
    if (auto const lane = lane_type_of(object); is_valid(lane))
        return check_lane(scope, node, object, lane);
    if (kind != type_kind::buffer && kind != type_kind::array)
    {
        unsupported(file, where, "a subscript on anything but a buffer, an image, a texture, an array or a vector");
        return error_type;
    }

    // CHK-287: an array takes one index per dimension it has, `grid[i, j]` for `grid[i][j]`, and a buffer takes one
    auto const arguments = ast_of(file).at(node.arguments);
    auto rank = isize(1);
    if (kind == type_kind::array)
        for (auto t = out.at(object).element; out.at(t).kind == type_kind::array; t = out.at(t).element)
            ++rank;
    auto is_plain = !arguments.empty() && arguments.size() <= rank;
    for (auto const& a : arguments)
        is_plain = is_plain && a.name.empty() && !a.is_splat;
    if (!is_plain)
    {
        report(diagnostic_kind::wrong_kind_of_name, file, where,
               kind == type_kind::buffer
                   ? cc::string("a buffer takes one index: `values[i]`")
                   : cc::format("{} takes one to {} indices, one per dimension", out.name_of(object), rank));
        return error_type;
    }

    auto const int_type = type_of_builtin(builtins::k_int, file, where);
    auto const uint_type = type_of_builtin(builtins::k_uint, file, where);
    auto result = object;
    for (auto const& a : arguments)
    {
        auto const outer_index = binding_index;
        binding_index = kind == type_kind::array && is_resource(out.at(out.at(result).element).kind)
                          ? a.value
                          : ast::expr_id::none;
        auto const index = check_expr(scope, a.value);
        binding_index = outer_index;
        // CHK-287: an `int` or a `uint`, which every target indexes by alike
        if (index != error_type && index != int_type && index != uint_type)
            report(diagnostic_kind::type_mismatch, file, span_of(file, a.value),
                   cc::format("{} is indexed by an int or a uint, and this is a {}",
                              kind == type_kind::buffer ? "a buffer" : "an array", out.name_of(index)));
        // CHK-309: a constant index names one element, and one past the end is refused by every target
        auto const count = out.at(result).count;
        auto at = cc::optional<i64>();
        if (index == int_type)
            if (auto const signed_at = constant_index(file, a.value); signed_at.has_value())
                at = signed_at.value();
        if (index == uint_type)
            at = constant_unsigned_index(file, a.value);
        if (kind == type_kind::array && count > 0 && at.has_value() && (at.value() < 0 || at.value() >= count))
            report(diagnostic_kind::invalid_constant_argument, file, span_of(file, a.value),
                   cc::format("{} is no index into {}, whose elements are 0 ..< {}", at.value(), out.name_of(result),
                              count));
        result = out.at(result).element;
    }
    if (!judge_atomic_use(file, id, result))
        return error_type;
    // an element of a binding array is the resource itself, which a builtin is handed, or a buffer that is subscripted
    if (kind == type_kind::array && is_resource(out.at(result).kind))
    {
        auto is_handed = false;
        for (auto const h : handed)
            is_handed = is_handed || h == id;
        // CHK-366: a buffer is subscripted, or handed to a call as a texture is
        auto const is_buffer = out.at(result).kind == type_kind::buffer;
        if (!is_handed && !(is_buffer && id == subscripted))
        {
            unsupported(
                file, where,
                is_buffer ? cc::string("a buffer as a value; read an element of it, as in `values[i]`")
                          : cc::format("{} as a value; call a builtin on it, as in `t.load(xy)`", out.name_of(result)));
            return error_type;
        }
    }
    return result;
}

type_id checker::check_lane(function_scope& scope, ast::index const& node, type_id vector, type_id lane)
{
    // CHK-395: one int, which picks the lane `v[i]` is, and a constant one names a lane the vector has
    auto const file = scope.file;
    auto const arguments = ast_of(file).at(node.arguments);
    if (arguments.size() != 1 || !arguments[0].name.empty() || arguments[0].is_splat)
    {
        report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, node.object),
               cc::format("{} takes one index, the lane: `v[i]`", out.name_of(vector)));
        return error_type;
    }
    auto const& a = arguments[0];
    auto const int_type = type_of_builtin(builtins::k_int, file, span_of(file, a.value));
    auto const index = check_expr(scope, a.value);
    if (index != error_type && index != int_type)
        report(diagnostic_kind::type_mismatch, file, span_of(file, a.value),
               cc::format("a vector is indexed by an int, and this is a {}", out.name_of(index)));
    auto const lanes = i32(out.at(out.at(vector).members).size());
    if (auto const at = index == int_type ? constant_index(file, a.value) : cc::optional<i32>();
        at.has_value() && (at.value() < 0 || at.value() >= lanes))
        report(diagnostic_kind::invalid_constant_argument, file, span_of(file, a.value),
               cc::format("{} is no lane of {}, whose lanes are 0 ..< {}", at.value(), out.name_of(vector), lanes));
    return lane;
}

type_id checker::check_texel(function_scope& scope, ast::expr_id id, ast::index const& node, type_id image)
{
    auto const file = scope.file;
    // the subscript's arguments are what `load` takes past the image, which stands first as a method's receiver
    auto arguments = check_arguments(scope, node.arguments, false);
    arguments.written.insert_at(0, {.expr = node.object});
    arguments.types.insert_at(0, image);
    arguments.names.insert_at(0, {});
    arguments.numbers.insert_at(0, number_of(file, node.object));
    arguments.literals.insert_at(0, -1);
    arguments.functions.insert_at(0, -1);
    arguments.undefineds.insert_at(0, false);
    arguments.leading_dots.insert_at(0, {});

    auto const& info = out.at(image);
    // CHK-514: a texture's texel reads as its `load`, and nothing stores one
    if (info.kind == type_kind::texture)
    {
        if (id == assigned)
        {
            report(diagnostic_kind::not_assignable, file, span_of(file, id),
                   "a texture is only read, and an image is what a shader stores into");
            return error_type;
        }
        return resolve_overload(scope, id, ast::expr_id::none, candidates_of(file, "load", image), arguments, "load",
                                call_spelling::dot_call);
    }
    auto const texel = type_of_builtin(texel_name_of(info.format), file, span_of(file, id));
    if (texel == error_type)
        return error_type;

    // CHK-373: an `@atomic` image's texel is an atomic, which stands only as a builtin's argument (CHK-297).
    // Its coordinates are what the plain image's `load` takes, which the record left here says to the flat tree.
    if (info.is_atomic)
    {
        auto const atomic = resource_type({.kind = type_kind::atomic, .element = texel});
        if (!judge_atomic_use(file, id, atomic))
            return error_type;
        auto plain = info;
        plain.is_atomic = false;
        auto const plain_image = resource_type(cc::move(plain));
        arguments.types[0] = plain_image;
        auto const loaded = resolve_overload(scope, id, ast::expr_id::none, candidates_of(file, "load", plain_image),
                                             arguments, "load", call_spelling::dot_call);
        return loaded == error_type ? error_type : atomic;
    }

    // A plain assignment stores alone, and its `store` is resolved once its value is known.
    auto const is_place = id == assigned;
    if (is_place && !is_compound_assigned)
    {
        texel_place = id;
        texel_arguments = cc::move(arguments);
        texel_load = -1;
        return texel;
    }
    auto const loaded = resolve_overload(scope, id, ast::expr_id::none, candidates_of(file, "load", image), arguments,
                                         "load", call_spelling::dot_call);
    if (loaded == error_type || !is_place)
        return loaded;
    texel_place = id;
    texel_load = tables_of(file).call_at(id);
    texel_arguments = cc::move(arguments);
    return loaded;
}

void checker::check_texel_store(function_scope& scope, ast::expr_id value, type_id type)
{
    auto const file = scope.file;
    auto const place = texel_place;
    texel_place = ast::expr_id::none;
    // named, since an array's layer is a named argument written before it
    auto arguments = cc::move(texel_arguments);
    add_argument(arguments, {.expr = value}, type, "value");
    auto const image = arguments.types[0];
    auto const stored = resolve_overload(scope, place, ast::expr_id::none, candidates_of(file, "store", image),
                                         arguments, "store", call_spelling::dot_call);
    if (stored == error_type)
        return;
    out.texel_stores.push_back(
        {.file = file, .place = place, .store = tables_of(file).call_at(place), .load = texel_load});
}

// ---- expressions ----------------------------------------------------------------------------------------------------


symbol_id checker::conversion_of(i32 file, source_span where, type_id from, type_id to)
{
    // Overloads of `as` differ in their result as well, so the one that matches both ends is the conversion.
    auto const candidates = operators_of(file, "as");
    for (auto const candidate : candidates)
    {
        if (demand(candidate, file, where) != symbol_state::checked)
            continue;
        auto const& info = out.functions[out.at(candidate).info];
        auto const parameters = out.at(info.parameters);
        if (parameters.size() == 1 && parameters[0].type == from && info.result == to)
            return candidate;
    }
    return symbol_id::none;
}

bool checker::is_texel_type_expr(i32 file, ast::expr_id type) const
{
    auto const* const applied = ast::is_valid(type) ? ast_of(file).at(type).node.try_as<ast::index>() : nullptr;
    return applied != nullptr && is_named(file, applied->object, "texel");
}

type_id checker::check_texel_conversion(i32 file, ast::expr_id id, type_id from, type_id to, ast::expr_id written)
{
    auto const where = span_of(file, id);
    auto const spelled = text_of(file, span_of(file, written));
    auto const* const source = out.builtin_type_of(from);
    auto const* const texel = out.builtin_type_of(to);
    auto const family = source != nullptr ? family_of(source->leaf_kind) : cc::string_view();
    auto const plain_name = source == nullptr || source->leaf_count == 1
                              ? cc::string(family)
                              : cc::format("{}{}", family, source->leaf_count);
    // CHK-391: a scalar or a plain vector, which `vec3`, `pos3` and `hpos4` are not
    if (texel == nullptr || family.empty() || source->leaf_count > 4 || out.name_of(from) != plain_name)
    {
        report(diagnostic_kind::no_matching_overload, file, where,
               cc::format("{} as {}: only a scalar or a plain vector converts to a texel", out.name_of(from), spelled));
        return error_type;
    }
    // CHK-392: a width changes here and a kind never does, which CHK-197's conversion does first
    if (source->leaf_kind != texel->leaf_kind)
    {
        auto const converted = source->leaf_count == 1
                                 ? cc::string(family_of(texel->leaf_kind))
                                 : cc::format("{}{}", family_of(texel->leaf_kind), source->leaf_count);
        auto const message = cc::format("{} as {}: the texel is {}", out.name_of(from), spelled, out.name_of(to));
        // the two-step spelling is offered only where its first step is a conversion that exists
        auto const intermediate = prelude_type(converted);
        if (intermediate != type_id::none && is_valid(conversion_of(file, where, from, intermediate)))
            report(diagnostic_kind::type_mismatch, file, where,
                   cc::format("{}, and a kind converts first: `(v as {}) as {}`", message, converted, spelled));
        else
            report(diagnostic_kind::type_mismatch, file, where,
                   cc::format("{}, and {} converts to no {}", message, out.name_of(from), converted));
        return error_type;
    }
    set_target(file, id, {.kind = target_kind::texel_conversion});
    return to;
}

bool checker::report_as_precedence(i32 file, ast::expr_id id)
{
    auto const& ast = ast_of(file);
    auto const source = cc::string_view(file_of(file).source);
    // `(a + b)` leaves no node, so whether a node was parenthesized is read from the text around it
    auto const is_parenthesized = [&](ast::expr_id e)
    {
        auto const where = span_of(file, e);
        auto before = isize(where.offset) - 1;
        while (before >= 0 && source[before] == ' ')
            --before;
        auto after = isize(where.end());
        while (after < source.size() && source[after] == ' ')
            ++after;
        return before >= 0 && source[before] == '(' && after < source.size() && source[after] == ')';
    };
    auto const operator_of = [&](ast::expr_id e) -> cc::string_view
    {
        auto const* const c = ast.at(e).node.try_as<ast::call>();
        if (c == nullptr || c->spelling != ast::call_spelling::infix || !sgl::is_valid(c->op) || is_parenthesized(e))
            return {};
        auto const op = text_of(file, file_of(file).at(c->op).where);
        for (auto const arithmetic : {"+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>"})
            if (op == arithmetic)
                return op;
        return {};
    };
    auto const cast_of = [&](ast::expr_id e, bool is_root) -> ast::cast const*
    {
        auto const* const c = ast.at(e).node.try_as<ast::cast>();
        return c != nullptr && (is_root || !is_parenthesized(e)) ? c : nullptr;
    };
    // the run reaches through the value of each cast, since `a as T - b as U` is `(a as (T - b)) as U`
    auto first_slip = ast::expr_id::none;
    for (auto e = id; e != ast::expr_id::none;)
    {
        auto const* const c = cast_of(e, e == id);
        if (c == nullptr)
            break;
        if (!operator_of(c->type).empty())
            first_slip = c->type;
        e = c->value;
    }
    if (first_slip == ast::expr_id::none)
        return false;

    // the run as written, its operands and its operators in order, `as` among them
    auto operands = cc::vector<cc::string>();
    auto operators = cc::vector<cc::string_view>();
    auto const flatten = [&](auto const& self, ast::expr_id e, bool is_root) -> void
    {
        if (auto const* const c = cast_of(e, is_root))
        {
            self(self, c->value, false);
            operators.push_back("as");
            self(self, c->type, false);
            return;
        }
        if (auto const op = operator_of(e); !op.empty())
        {
            auto const arguments = ast.at(ast.at(e).node.as<ast::call>().arguments);
            self(self, arguments[0].value, false);
            operators.push_back(op);
            self(self, arguments[1].value, false);
            return;
        }
        auto const text = text_of(file, span_of(file, e));
        operands.push_back(is_parenthesized(e) ? cc::format("({})", text) : cc::string(text));
    };
    flatten(flatten, id, true);

    // the reading the author meant: each `as` takes the operand before it
    auto fixed = cc::string();
    auto current = operands[0];
    for (auto i = isize(0); i < operators.size(); ++i)
    {
        if (operators[i] == "as")
            current = cc::format("({} as {})", current, operands[i + 1]);
        else
        {
            fixed.appendf("{} {} ", current, operators[i]);
            current = operands[i + 1];
        }
    }
    fixed += current;

    auto const where = span_of(file, id);
    auto& d = report(diagnostic_kind::as_precedence, file, where,
                     cc::format("`as` binds looser than `{}`, so this converts to the type `{}`; write `{}`",
                                operator_of(first_slip), text_of(file, span_of(file, first_slip)), fixed));
    d.fixes.push_back({.title = cc::format("Convert each operand first: {}", fixed), .edits = {{where, fixed}}});
    return true;
}

type_id checker::check_cast(function_scope& scope, ast::expr_id id, ast::cast const& node)
{
    auto const file = scope.file;
    auto const where = span_of(file, id);
    if (report_as_precedence(file, id))
        return error_type;
    auto const from = check_expr(scope, node.value);
    auto const to = resolve_value_type(file, node.type);
    if (from == error_type || to == error_type)
        return error_type;
    if (from == to)
        return to;
    if (is_texel_type_expr(file, node.type))
        return check_texel_conversion(file, id, from, to, node.type);

    if (auto const candidate = conversion_of(file, where, from, to); is_valid(candidate))
    {
        set_target(file, id, {.kind = target_kind::overload, .symbol = candidate});
        // A conversion the program declares is a call like any other, inlined where it stands.
        if (!is_valid(out.at(candidate).intrinsic))
            note_program_call(scope, candidate, where);
        return to;
    }
    // CHK-150: an enum's `as int` takes the enum as the int it is held as, as its `==` does
    if (out.is_plain_enum(from) && to == prelude_type(builtins::k_int))
        if (auto const* const found = prelude_names.get_ptr("enum_as_int");
            found != nullptr && demand((*found)[0], file, where) == symbol_state::checked)
        {
            set_target(file, id, {.kind = target_kind::overload, .symbol = (*found)[0]});
            return to;
        }
    report(diagnostic_kind::no_matching_overload, file, where,
           cc::format("{} as {}: no conversion", out.name_of(from), out.name_of(to)));
    return error_type;
}

type_id checker::check_expr(function_scope& scope, ast::expr_id expr)
{
    if (!ast::is_valid(expr))
        return error_type;

    auto const file = scope.file;
    auto const& e = ast_of(file).at(expr);
    auto const where = span_of(file, expr);
    auto const expected = expected_value;
    expected_value = type_id::none;
    judge_attributes(file, e.attributes, {}, "an expression");

    auto const not_yet = [&](cc::string_view construct)
    {
        unsupported(file, where, construct);
        return error_type;
    };

    auto const type = e.node.visit(
        [&](ast::invalid_expr const&) { return error_type; }, [&](ast::literal const& l)
        { return check_literal(scope, expr, l); }, [&](ast::name const& n) { return check_name(scope, expr, n); },
        [&](ast::member const& m) { return check_member(scope, expr, m); },
        [&](ast::call const& c) { return check_call(scope, expr, c); },
        [&](ast::self_ref const&)
        {
            // CHK-245: `self` is the receiver of a method or a property, and names nothing anywhere else
            auto const* const local = is_valid(scope.receiver) ? scope.find_local("self") : nullptr;
            if (local == nullptr)
            {
                report(diagnostic_kind::unknown_name, file, where, "self");
                return error_type;
            }
            set_target(file, expr, local->where);
            if (local->is_captured)
            {
                report_capture(scope, where, *local);
                return error_type;
            }
            return local->type;
        },
        [&](ast::void_ref const&) { return void_type; },
        [&](ast::wildcard const&) { return not_yet("a wildcard as a value"); },
        // CHK-152: a leading dot needs a type the context expects, which today only a `case` pattern gives it
        [&](ast::leading_dot const&) { return not_yet("a leading-dot name outside a case pattern"); },
        [&](ast::index const& node) { return check_index(scope, expr, node); },
        // AST-128: `mut buffer[float]` and its neighbours parse, and the binding model they belong to is unbuilt.
        [&](ast::qualified_type const&) { return not_yet("a resource type"); },
        [&](ast::tuple const&) { return not_yet("a tuple"); },
        [&](ast::array const&) { return check_array_literal(scope, expr, type_id::none); },
        [&](ast::object const&) { return not_yet("an object with no struct to convert to"); },
        [&](ast::comparison_chain const& chain) { return check_chain(scope, expr, chain); }, [&](ast::cast const& node)
        { return check_cast(scope, expr, node); }, [&](ast::membership const&) { return not_yet("in"); },
        [&](ast::ascription const&) { return not_yet("a type ascription"); },
        [&](ast::range const&) { return not_yet("a range"); }, [&](ast::lambda const&) { return not_yet("a lambda"); },
        [&](ast::case_expr const& c) { return check_case(scope, expr, c, true, nullptr, expected); },
        [&](ast::if_expr const& i) { return check_if_value(scope, i, true, nullptr, expected); },
        [&](ast::loop_expr const& loop)
        {
            auto has_break = false;
            return check_loop(scope, expr, loop, true, has_break);
        },
        [&](ast::return_expr const&) { return not_yet("return as a value"); }, [&](ast::yield_expr const&)
        { return not_yet("yield as a value"); }, [&](ast::break_expr const&) { return not_yet("break as a value"); },
        [&](ast::continue_expr const&) { return not_yet("continue as a value"); }, [&](ast::discard_expr const&)
        { return not_yet("discard as a value"); }, [&](ast::struct_type const&) { return not_yet("a type as a value"); },
        [&](ast::function_type const&) { return not_yet("a type as a value"); },
        // reserved, and reported by the AST pass
        [&](ast::with_bindings const&) { return error_type; });

    set_type(file, expr, type);
    return type;
}

type_id checker::check_literal(function_scope& scope, ast::expr_id id, ast::literal const& literal)
{
    auto const file = scope.file;
    auto const where = span_of(file, id);
    if (literal.kind == ast::literal_kind::quoted)
    {
        unsupported(file, where, "a string literal");
        return error_type;
    }
    if (literal.kind == ast::literal_kind::hash)
    {
        unsupported(file, where, "a hash literal");
        return error_type;
    }

    auto const text = text_of(file, where);
    switch (classify_number(text))
    {
    case number_class::plain_integer:
    {
        // CHK-61: held in 64 bits, and refused only where it keeps a default type that does not hold it
        auto const value = parse_literal_integer(text);
        auto const is_past_long = parse_literal_past_long(text).has_value();
        if (!value.has_value() && !is_past_long)
        {
            unsupported(file, where, "an integer literal beyond 64 bits");
            return error_type;
        }
        if (is_past_long || value.value() < -2147483647 - 1 || value.value() > 2147483647)
            wide_literals.push_back({.file = file, .expr = id, .function = scope.function, .instance = current_instance});
        return type_of_builtin(builtins::k_int, file, where);
    }
    case number_class::suffixed:
        return check_suffixed_literal(file, where, text);
    case number_class::other:
        unsupported(file, where, "a number literal with a p exponent");
        return error_type;
    case number_class::plain_float:
        break;
    }
    if (!parse_plain_float(text).has_value())
    {
        unsupported(file, where, "a float literal this large");
        return error_type;
    }
    // CHK-60: at its default type too, a float literal is one `float` holds
    auto const float_type = type_of_builtin(builtins::k_float, file, where);
    if (float_type != error_type && !holds(number_of(file, id), float_type))
    {
        report(diagnostic_kind::literal_not_representable, file, where, cc::format("float does not hold {}", text));
        return error_type;
    }
    return float_type;
}

type_id checker::check_suffixed_literal(i32 file, source_span where, cc::string_view text)
{
    // CHK-357: the suffix names the type, and the literal converts to no other
    auto const s = split_suffix(text).value();
    if (s.width != 32 && s.width != 16 && s.width != 64)
    {
        unsupported(file, where, cc::format("a literal of {} bits, which no type of the prelude is", s.width));
        return error_type;
    }
    auto const is_narrow = s.width == 16;
    auto const is_wide = s.width == 64;
    auto const is_float_body = classify_number(s.body) == number_class::plain_float;
    if (is_float_body && s.letter != 'f')
    {
        report(diagnostic_kind::literal_not_representable, file, where,
               cc::format("{} is a float literal, and its suffix names an integer type", text));
        return error_type;
    }

    auto const name = s.letter == 'f' ? (is_narrow ? builtins::k_half
                                         : is_wide ? builtins::k_double
                                                   : builtins::k_float)
                    : s.letter == 'u' ? (is_narrow ? builtins::k_ushort
                                         : is_wide ? builtins::k_ulong
                                                   : builtins::k_uint)
                                      : (is_narrow ? builtins::k_short
                                         : is_wide ? builtins::k_long
                                                   : builtins::k_int);
    auto const type = type_of_builtin(name, file, where);
    if (type == error_type)
        return error_type;
    if (s.letter == 'f')
    {
        auto const value = is_float_body ? parse_plain_float(s.body) : cc::optional<f64>();
        auto const integer = is_float_body ? cc::optional<i64>() : parse_literal_integer(s.body);
        if (!value.has_value() && !integer.has_value())
        {
            unsupported(file, where, "a float literal this large");
            return error_type;
        }
        auto const real = value.has_value() ? value.value() : f64(integer.value());
        if (!holds({.is_number = true, .real = real}, type))
        {
            report(diagnostic_kind::literal_not_representable, file, where,
                   cc::format("{} does not hold {}", name, text));
            return error_type;
        }
        return type;
    }
    auto const value = parse_literal_integer(s.body);
    auto const past_long = parse_literal_past_long(s.body);
    if (!value.has_value() && !past_long.has_value())
    {
        unsupported(file, where, "an integer literal beyond 64 bits");
        return error_type;
    }
    auto const number = past_long.has_value()
                          ? number_literal{.is_number = true, .is_integer = true, .is_past_long = true}
                          : number_literal{.is_number = true, .is_integer = true, .integer = value.value()};
    if (!holds(number, type))
    {
        report(diagnostic_kind::literal_not_representable, file, where, cc::format("{} does not hold {}", name, text));
        return error_type;
    }
    return type;
}

type_id checker::check_name(function_scope& scope, ast::expr_id id, ast::name const& name)
{
    auto const file = scope.file;
    auto const where = span_of(file, id);
    auto const text = text_of(file, name.where);

    if (auto const* const local = scope.find_local(text))
    {
        // CHK-413: a function's own `const` is the symbol it was compiled to
        if (local->where.kind == target_kind::symbol && !local->is_captured)
        {
            auto const one = cc::vector<symbol_id>{local->where.symbol};
            return check_symbol_value(scope, id, text, &one);
        }
        set_target(file, id, local->where);
        if (local->is_captured)
        {
            report_capture(scope, where, *local);
            return error_type;
        }
        // CHK-366: a resource parameter stands where its binding member would, which is as a call's argument
        if (local->type != error_type && is_resource(out.at(local->type).kind))
        {
            auto is_handed = false;
            for (auto const h : handed)
                is_handed = is_handed || h == id;
            // a buffer and bytes are subscripted, or their length read, as a member's are
            auto const kind = out.at(local->type).kind;
            auto const is_read = (kind == type_kind::buffer || kind == type_kind::bytes) && id == subscripted;
            if (!is_handed && !is_read)
            {
                unsupported(file, where,
                            cc::format("{} as a value; hand it to a call, as in `t.load(xy)`", out.name_of(local->type)));
                return error_type;
            }
        }
        return local->type;
    }
    if (is_type_parameter_name(text))
    {
        auto stands_for = type_id::none;
        for (auto i = type_parameter_names.size(); i > 0 && !is_valid(stands_for); --i)
            if (type_parameter_names[i - 1].first == text)
                stands_for = type_parameter_names[i - 1].second;
        // CHK-338: a value parameter is a value of its type, and in an instance the constant it stands for
        if (out.at(stands_for).kind == type_kind::constant)
        {
            set_target(file, id, {.kind = target_kind::generic_value, .index = i32(index_of(stands_for))});
            return out.at(stands_for).element;
        }
        if (is_valid(out.at(stands_for).element))
            return out.at(stands_for).element;
        report(diagnostic_kind::wrong_kind_of_name, file, where,
               cc::format("{} is a type parameter, which names a type and no value", text));
        return error_type;
    }

    auto const* const found = seen_from(file, text);
    if ((found == nullptr || found->empty()) && used_module(file, text) >= 0)
    {
        // CHK-347: a module is reached through its names, and is no value itself
        report(diagnostic_kind::wrong_kind_of_name, file, where,
               cc::format("{} is a module, and only `{}.name` reaches what it declares", text, text));
        return error_type;
    }
    return check_symbol_value(scope, id, text, found);
}

type_id checker::check_symbol_value(function_scope& scope,
                                    ast::expr_id id,
                                    cc::string_view text,
                                    cc::vector<symbol_id> const* found)
{
    auto const file = scope.file;
    auto const where = span_of(file, id);
    if (found == nullptr || found->empty())
    {
        report(diagnostic_kind::unknown_name, file, where, text);
        return error_type;
    }

    auto const symbol = found->front();
    set_target(file, id, {.kind = target_kind::symbol, .symbol = symbol});
    switch (out.at(symbol).kind)
    {
    case symbol_kind::structure:
    case symbol_kind::enumeration:
    case symbol_kind::alias:
        unsupported(file, where, "a type as a value");
        break;
    case symbol_kind::function:
        unsupported(file, where, "a function as a value");
        break;
    case symbol_kind::binding:
        unsupported(file, where, "a binding as a value");
        break;
    case symbol_kind::pipeline:
        report(diagnostic_kind::wrong_kind_of_name, file, where,
               cc::format("{} is a pipeline, which the host acquires and no shader reads", text));
        break;
    case symbol_kind::constant:
        // CHK-219: a const is its value, and one that did not check is silent here, as a failed symbol always is.
        if (demand(symbol, file, where) == symbol_state::checked)
        {
            note_option(file, where, out.constants[out.at(symbol).info]);
            return out.at(symbol).type;
        }
        break;
    case symbol_kind::sampler:
    {
        // CHK-314: a file-scope sampler is handed to a builtin, as a sampler member is
        if (demand(symbol, file, where) != symbol_state::checked)
            break;
        auto is_handed = false;
        for (auto const h : handed)
            is_handed = is_handed || h == id;
        if (!is_valid(scope.function) || !is_handed)
        {
            unsupported(file, where, "a sampler as a value; hand it to a builtin, as in `t.sample(uv, smp)`");
            break;
        }
        return out.at(symbol).type;
    }
    }
    return error_type;
}

type_id checker::check_member(function_scope& scope, ast::expr_id id, ast::member const& member)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const name = text_of(file, member.name);

    auto const find_member = [&](ast::range_of<member_info> range)
    {
        auto const members = out.at(range);
        for (auto i = isize(0); i < members.size(); ++i)
            if (members[i].name == name)
                return i;
        return isize(-1);
    };

    // CHK-348: `m.name` is the name `name` of module `m`, which stands for it wherever a bare name could
    if (module_named(file, member.object, &scope) >= 0)
        return check_symbol_value(scope, id, text_of(file, span_of(file, id)), symbols_named(file, id, &scope));

    // `constants.view_projection`: a binding is no value, so the object is looked at before it is checked
    if (auto const* const found = symbols_named(file, member.object, &scope))
    {
        if (out.at(found->front()).kind == symbol_kind::binding)
        {
            auto const binding = found->front();
            auto const object_where = span_of(file, member.object);
            set_target(file, member.object, {.kind = target_kind::symbol, .symbol = binding});
            if (demand(binding, file, object_where) != symbol_state::checked)
                return error_type;
            // A const's value is checked outside every function, and a binding member is read only inside one.
            if (!is_valid(scope.function))
            {
                unsupported(file, span_of(file, id), "a binding member outside a function");
                return error_type;
            }

            auto is_listed = false;
            for (auto const listed : out.at(out.functions[out.at(scope.function).info].bindings))
                is_listed = is_listed || listed == binding;
            // CHK-295: a test's run holds its own workgroup memory, which no host fills
            is_listed = is_listed || (scope.is_test && is_workgroup_binding(binding));
            // CHK-228: a binding the test does not list, and its function does, is a value of the function's run
            auto is_captured = false;
            if (scope.is_test && !is_listed && is_valid(scope.enclosing))
                for (auto const listed : out.at(out.functions[out.at(scope.enclosing).info].bindings))
                    is_captured = is_captured || listed == binding;
            if (is_captured)
                report(diagnostic_kind::test_captures_runtime_value, file, object_where,
                       cc::format("{} is a binding of {}, and a test runs on its own", name_seen_from(file, binding),
                                  out.at(scope.enclosing).name));
            else if (!is_listed && scope.is_test)
                report(diagnostic_kind::binding_not_listed, file, object_where,
                       cc::format("{} is a binding, and the test does not list it", name_seen_from(file, binding)));
            else if (!is_listed)
                report(diagnostic_kind::binding_not_listed, file, object_where,
                       cc::format("{} is not in the binding list of {}", name_seen_from(file, binding),
                                  out.at(scope.function).name));

            auto const index = find_member(out.bindings[out.at(binding).info].members);
            if (index < 0)
            {
                if (!member.name.empty())
                    report(diagnostic_kind::unknown_member, file, member.name,
                           cc::format("the binding {} has no member {}", out.at(binding).name, name));
                return error_type;
            }
            set_target(file, id, {.kind = target_kind::binding_member, .symbol = binding, .index = i32(index)});
            auto const type = out.at(out.bindings[out.at(binding).info].members)[index].type;
            // CHK-366: a buffer is subscripted, or handed to a call as a texture is
            auto is_buffer_handed = false;
            for (auto const h : handed)
                is_buffer_handed = is_buffer_handed || h == id;
            if (type != error_type && out.at(type).kind == type_kind::buffer && id != subscripted && !is_buffer_handed)
            {
                unsupported(file, span_of(file, id),
                            "a buffer as a value; read an element of it, as in `values[i]`, or hand it to a call");
                return error_type;
            }
            if (!judge_atomic_use(file, id, type))
                return error_type;
            // CHK-299: a binding array is read by element, which a builtin is handed
            if (type != error_type && out.at(type).kind == type_kind::array && holds_resource(type) && id != subscripted)
            {
                report(diagnostic_kind::wrong_kind_of_name, file, span_of(file, id),
                       cc::format("{}.{} is a binding array, read by element: `{}[i]`", out.at(binding).name,
                                  out.at(out.bindings[out.at(binding).info].members)[index].name,
                                  text_of(file, span_of(file, id))));
                return error_type;
            }
            auto is_handed = false;
            for (auto const h : handed)
                is_handed = is_handed || h == id;
            // CHK-460: a `bytes` whose length is read is named as a subscripted buffer is
            auto const is_measured = out.at(type).kind == type_kind::bytes && id == subscripted;
            if (type != error_type && is_resource(out.at(type).kind) && out.at(type).kind != type_kind::buffer
                && !is_handed && !is_measured)
            {
                unsupported(file, span_of(file, id),
                            cc::format("{} as a value; call a builtin on it, as in `t.load(xy)`", out.name_of(type)));
                return error_type;
            }
            return type;
        }

        // `light_kind.point`: an enum is no value either (CHK-148), so its name never reaches `check_expr`
        // an alias of an enum names its cases as the enum's own name does (CHK-14)
        auto const enumeration = out.at(found->front()).kind == symbol_kind::alias
                                   ? declared_type_of(found->front(), file, span_of(file, member.object))
                                   : found->front();
        if (is_valid(enumeration) && out.at(enumeration).kind == symbol_kind::enumeration)
        {
            auto const object_where = span_of(file, member.object);
            set_target(file, member.object, {.kind = target_kind::symbol, .symbol = found->front()});
            if (demand(enumeration, file, object_where) != symbol_state::checked)
                return error_type;

            auto const type = out.at(enumeration).type;
            auto const cases = out.at(out.at(type).cases);
            auto index = isize(-1);
            for (auto i = isize(0); i < cases.size(); ++i)
                if (cases[i].name == name)
                    index = i;
            if (index < 0)
            {
                if (!member.name.empty())
                    report(diagnostic_kind::unknown_member, file, member.name,
                           cc::format("the enum {} has no case {}", out.at(enumeration).name, name));
                return error_type;
            }
            set_target(file, id, {.kind = target_kind::enum_case, .symbol = enumeration, .index = i32(index)});
            return type;
        }
    }

    // CHK-460: `values.length` names a buffer without reading an element of it, as a subscript does
    auto const outer = subscripted;
    if (name == "length")
        subscripted = member.object;
    auto const object = check_expr(scope, member.object);
    subscripted = outer;
    if (object == error_type || member.name.empty())
        return error_type;

    // CHK-288: an array's length is a constant of its type, and it has no other member
    if (out.at(object).kind == type_kind::array)
    {
        // CHK-491: an unbounded array's length is the views a group binds, which sg writes per dispatch or draw
        if (name == "length" && out.at(object).count == 0)
        {
            auto const where = tables_of(file).target_at(member.object);
            if (where.kind != target_kind::binding_member)
            {
                unsupported(file, span_of(file, id),
                            "the length of an unbounded binding array named otherwise than "
                            "as its binding's member");
                return error_type;
            }
            set_target(file, id, {.kind = target_kind::buffer_length});
            return type_of_builtin(builtins::k_int, file, span_of(file, id));
        }
        if (name != "length")
        {
            report(diagnostic_kind::unknown_member, file, member.name,
                   cc::format("{} has no member {}; an array's one member is its length", out.name_of(object), name));
            return error_type;
        }
        set_target(file, id, {.kind = target_kind::array_length, .index = out.at(object).count});
        return type_of_builtin(builtins::k_int, file, span_of(file, id));
    }

    // CHK-460: a buffer's length is the bound memory's
    if ((out.at(object).kind == type_kind::buffer || out.at(object).kind == type_kind::bytes) && name == "length")
    {
        set_target(file, id, {.kind = target_kind::buffer_length});
        return type_of_builtin(builtins::k_uint, file, span_of(file, id));
    }

    // CHK-249: `a.foo` is the field where there is one, and a call of `foo` with `a` otherwise
    auto const& type = out.at(object);
    auto index = find_member(type.members);
    // CHK-409: one letter of the second alphabet is its field, as a field's own letter is
    if (index < 0 && name.size() == 1 && type.has_swizzles)
        for (auto i = isize(0); i < type.swizzle_alias.size(); ++i)
            if (type.swizzle_alias[i] == name[0])
                index = decltype(index)(i);
    if (index >= 0)
    {
        set_target(file, id, {.kind = target_kind::field, .symbol = type.symbol, .index = i32(index)});
        return out.at(type.members)[index].type;
    }
    // CHK-386: a swizzle is found where a field is, so no function in scope ever stands for `v.xy`
    auto const letters = swizzle_of(object, name);
    if (letters.count > 0)
    {
        set_target(file, id, {.kind = target_kind::swizzle, .symbol = type.symbol, .index = letters.packed()});
        auto const element = out.at(type.members)[letters.fields[0]].type;
        return type_of_builtin(vector_name_of(element, letters.count), file, span_of(file, id));
    }
    auto const candidates = candidates_of(file, name, object);
    if (candidates.empty())
    {
        report(diagnostic_kind::unknown_member, file, member.name,
               cc::format("{} has no member {}{}", out.name_of(object), name, why_no_swizzle(object, name)));
        return error_type;
    }
    auto arguments = call_arguments();
    add_argument(arguments, {.expr = member.object}, object, {}, number_of(file, member.object));
    return resolve_overload(scope, id, ast::expr_id::none, candidates, arguments, name, call_spelling::dot_read);
}

swizzle checker::swizzle_of(type_id object, cc::string_view name) const
{
    if (object == error_type || !out.at(object).has_swizzles || name.size() < 2 || name.size() > 4)
        return {};
    auto const fields = out.at(out.at(object).members);
    // CHK-409: the letters of one swizzle are all the fields' own, or all of the second alphabet
    auto const& alias = out.at(object).swizzle_alias;
    for (auto const is_alias : {false, true})
    {
        if (is_alias && alias.empty())
            break;
        auto result = swizzle{.count = i8(name.size())};
        auto is_found = true;
        for (auto i = isize(0); is_found && i < name.size(); ++i)
        {
            auto found = isize(-1);
            for (auto f = isize(0); f < fields.size(); ++f)
                if (is_alias ? alias[f] == name[i] : fields[f].name.size() == 1 && fields[f].name[0] == name[i])
                    found = f;
            is_found = found >= 0;
            result.fields[i] = i8(found);
        }
        if (is_found)
            return result;
    }
    return {};
}

type_id checker::lane_type_of(type_id object) const
{
    if (object == error_type || out.at(object).kind != type_kind::structure || !out.at(object).has_swizzles)
        return type_id::none;
    auto const fields = out.at(out.at(object).members);
    if (fields.empty())
        return type_id::none;
    for (auto const& f : fields)
        if (f.type != fields[0].type)
            return type_id::none;
    return fields[0].type;
}

cc::string checker::why_no_swizzle(type_id object, cc::string_view name) const
{
    if (object == error_type || !out.at(object).has_swizzles || name.size() < 2)
        return {};
    // CHK-385: the detail names the letter that is no field, or the letter one too many
    auto letters = cc::string();
    for (auto const& f : out.at(out.at(object).members))
        letters += f.name;
    auto const& alias = out.at(object).swizzle_alias;
    auto const is_in = [](cc::string_view set, char c)
    {
        for (auto const x : set)
            if (x == c)
                return true;
        return false;
    };
    auto const spelled = alias.empty() ? letters : cc::format("{} or {}", letters, alias);
    for (auto const c : name)
        if (!is_in(letters, c) && !is_in(alias, c))
            return cc::format(": its swizzle letters are {}, and {} is none", spelled, c);
    // CHK-409: every letter names a field, so the two alphabets are mixed
    if (name.size() <= 4)
        return cc::format(": a swizzle takes its letters from {} alone, and {} mixes them", spelled, name);
    return cc::format(": a swizzle reads at most four fields, and {} is its fifth letter", name[4]);
}
