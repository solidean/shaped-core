#include <shaped-graphics-language/check/impl/flattener.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// Structs with cases as the flat tree holds them (CHK-543): one carrier struct whatever the case, whose variant region is
// `uint` words that each variant field's value is the bits of.
// Nothing after the flattener knows a struct has cases.

namespace
{
[[nodiscard]] i32 lowest_leaf(u32 leaves)
{
    auto i = 0;
    while (i < 32 && (leaves & (u32(1) << i)) == 0)
        ++i;
    return i;
}

[[nodiscard]] i32 highest_leaf(u32 leaves)
{
    auto i = 31;
    while (i >= 0 && (leaves & (u32(1) << i)) == 0)
        --i;
    return i;
}
} // namespace

// ---- lookups --------------------------------------------------------------------------------------------------------

symbol_id flattener::prelude_function(cc::string_view name, cc::span<type_id const> parameters) const
{
    auto const* const found = c.prelude_names.get_ptr(name);
    if (found == nullptr)
        return symbol_id::none;
    for (auto const s : *found)
    {
        if (c.out.at(s).kind != symbol_kind::function || c.out.at(s).info < 0)
            continue;
        auto const ps = c.out.at(c.out.functions[c.out.at(s).info].parameters);
        auto is_same = ps.size() == parameters.size();
        for (auto i = isize(0); is_same && i < ps.size(); ++i)
            is_same = ps[i].type == parameters[i];
        if (is_same)
            return s;
    }
    return symbol_id::none;
}

symbol_id flattener::conversion(type_id from, type_id to) const
{
    auto const* const found = c.operators_seen_from(file()).get_ptr("as");
    if (found == nullptr)
        return symbol_id::none;
    for (auto const s : *found)
    {
        if (c.out.at(s).info < 0)
            continue;
        auto const& info = c.out.functions[c.out.at(s).info];
        auto const ps = c.out.at(info.parameters);
        if (ps.size() == 1 && ps[0].type == from && info.result == to)
            return s;
    }
    return symbol_id::none;
}

flat_expr_id flattener::uint_literal(u32 value, ast::expr_id from)
{
    return add_expr(c.prelude_type(builtins::k_uint), from, flat_int_literal{.value = i32(value), .is_unsigned = true});
}

flat_expr_id flattener::call_named(cc::string_view name, flat_expr_id argument, ast::expr_id from)
{
    type_id const types[] = {entry.at(argument).type};
    auto const callee = prelude_function(name, types);
    if (!is_valid(callee))
        return fail();
    flat_expr_id const arguments[] = {argument};
    return builtin_call(from, callee, arguments);
}

flat_expr_id flattener::converted(flat_expr_id value, type_id to, ast::expr_id from)
{
    auto const callee = conversion(entry.at(value).type, to);
    if (!is_valid(callee))
        return fail();
    flat_expr_id const arguments[] = {value};
    return builtin_call(from, callee, arguments);
}

flat_expr_id flattener::compared(cc::string_view op, flat_expr_id lhs, flat_expr_id rhs, ast::expr_id from)
{
    type_id const types[] = {entry.at(lhs).type, entry.at(rhs).type};
    auto const callee = c.find_operator(file(), op, types);
    if (!is_valid(callee))
        return fail();
    flat_expr_id const arguments[] = {lhs, rhs};
    return builtin_call(from, callee, arguments);
}

template <class F>
flat_expr_id flattener::over_value(flat_expr_id object, type_id type, ast::expr_id from, bool always_block, F&& make)
{
    if (!is_valid(object))
        return fail();
    if (!always_block && is_substitutable(object))
        return make(object);
    auto const label = add_label("value");
    auto const where = origin{.file = file(), .expr = from};
    auto outer = cc::move(block);
    block = {};
    auto value = object;
    if (!is_substitutable(object))
    {
        auto const local = add_local(local_kind::temporary, "value", entry.at(object).type);
        add_stmt(where, flat_let{.local = local, .value = object});
        value = local_ref(local, from);
    }
    auto const result = make(value);
    add_stmt(where, flat_leave{.target = label, .value = result});
    auto const body = add_list(block);
    block = cc::move(outer);
    return add_expr(type, from, flat_block{.label = label, .body = body});
}

// ---- the words ------------------------------------------------------------------------------------------------------

bool flattener::encode_words(flat_expr_id value, type_id type, ast::expr_id from, cc::vector<flat_expr_id>& words)
{
    if (!is_valid(value) || type == checked_module::error_type)
        return false;
    auto const uint_type = c.prelude_type(builtins::k_uint);
    auto const int_type_ = c.prelude_type(builtins::k_int);
    auto const& t = c.out.at(type);
    auto const name = c.out.name_of(type);
    if (t.kind == type_kind::enumeration && c.out.is_plain_enum(type))
    {
        auto const* const found = c.prelude_names.get_ptr("enum_as_int");
        if (found == nullptr || found->empty())
            return false;
        flat_expr_id const arguments[] = {value};
        words.push_back(converted(builtin_call(from, found->front(), arguments), uint_type, from));
        return true;
    }
    if (type == uint_type)
    {
        words.push_back(value);
        return true;
    }
    if (type == int_type_ || name == "bool" || name == "ushort")
    {
        words.push_back(converted(value, uint_type, from));
        return true;
    }
    if (name == "short")
    {
        words.push_back(converted(converted(value, int_type_, from), uint_type, from));
        return true;
    }
    if (name == "float" || name == "half")
    {
        words.push_back(call_named("reinterpret_as_uint", value, from));
        return true;
    }
    if (c.out.builtin_type_of(type) != nullptr)
    {
        // a vector: each lane its word
        auto const lane = c.lane_type_of(type);
        if (!is_valid(lane))
            return false;
        auto const count = c.out.builtin_type_of(type)->leaf_count;
        for (auto i = 0; i < count; ++i)
            if (!encode_words(add_expr(lane, from, flat_member{.object = again(value, from), .member = i}), lane, from,
                              words))
                return false;
        return true;
    }
    if (t.kind == type_kind::array)
    {
        for (auto i = 0; i < t.count; ++i)
        {
            auto const element = add_expr(t.element, from,
                                          flat_element{.object = again(value, from),
                                                       .index = add_expr(int_type_, from, flat_int_literal{.value = i})});
            if (!encode_words(element, t.element, from, words))
                return false;
        }
        return true;
    }
    if (t.kind != type_kind::structure)
        return false;
    auto const members = c.out.at(t.members);
    for (auto k = isize(0); k < members.size(); ++k)
        if (!encode_words(add_expr(members[k].type, from, flat_member{.object = again(value, from), .member = i32(k)}),
                          members[k].type, from, words))
            return false;
    return true;
}

flat_expr_id flattener::decode_words(cc::span<flat_expr_id const> words, isize& at, type_id type, ast::expr_id from)
{
    auto const uint_type = c.prelude_type(builtins::k_uint);
    auto const int_type_ = c.prelude_type(builtins::k_int);
    auto const& t = c.out.at(type);
    auto const name = c.out.name_of(type);
    auto const next = [&]() -> flat_expr_id { return at < words.size() ? words[at++] : flat_expr_id::none; };
    if (t.kind == type_kind::enumeration && c.out.is_plain_enum(type))
    {
        // an enum is the int its case is held as on every target, which `enum_as_int` takes as it is
        auto const* const found = c.prelude_names.get_ptr("enum_as_int");
        auto const word = next();
        if (found == nullptr || found->empty() || !is_valid(word))
            return fail();
        auto const& s = c.out.at(found->front());
        flat_expr_id const arguments[] = {converted(word, int_type_, from)};
        return add_expr(type, from,
                        flat_call{.callee = found->front(),
                                  .intrinsic = s.intrinsic,
                                  .is_pure = true,
                                  .arguments = add_list(arguments)});
    }
    if (type == uint_type)
        return next();
    if (type == int_type_ || name == "ushort")
        return converted(next(), type, from);
    if (name == "bool")
        return compared("!=", next(), uint_literal(0, from), from);
    if (name == "short")
        return converted(converted(next(), int_type_, from), type, from);
    if (name == "float")
        return call_named("reinterpret_as_float", next(), from);
    if (name == "half")
        return call_named("reinterpret_as_half", next(), from);
    auto parts = cc::vector<flat_expr_id>();
    if (c.out.builtin_type_of(type) != nullptr)
    {
        auto const lane = c.lane_type_of(type);
        if (!is_valid(lane))
            return fail();
        for (auto i = 0; i < c.out.builtin_type_of(type)->leaf_count; ++i)
            parts.push_back(decode_words(words, at, lane, from));
    }
    else if (t.kind == type_kind::array)
        for (auto i = 0; i < t.count; ++i)
            parts.push_back(decode_words(words, at, t.element, from));
    else if (t.kind == type_kind::structure)
        for (auto const& m : c.out.at(t.members))
            parts.push_back(decode_words(words, at, m.type, from));
    else
        return fail();
    for (auto const p : parts)
        if (!is_valid(p))
            return fail();
    return add_expr(type, from, flat_construct{.arguments = add_list(parts)});
}

flat_expr_id flattener::read_variant(flat_expr_id object, type_id carrier, i32 index, ast::expr_id from)
{
    auto const& t = c.out.at(carrier);
    auto const& v = c.out.at(t.variant_fields)[index];
    auto const uint_type = c.prelude_type(builtins::k_uint);
    auto words = cc::vector<flat_expr_id>();
    for (auto w = 0; w < v.word_count; ++w)
        words.push_back(
            add_expr(uint_type, from, flat_member{.object = again(object, from), .member = t.first_word + v.word + w}));
    auto at = isize(0);
    return decode_words(words, at, v.type, from);
}

// ---- which case a value is in -----------------------------------------------------------------------------------------

flat_expr_id flattener::when_test(flat_expr_id object, type_id carrier, i32 case_index, ast::expr_id from)
{
    auto const& t = c.out.at(carrier);
    auto const cases = c.out.at(t.struct_cases);
    auto const struct_file = c.out.at(t.symbol).file;
    auto const& ast = c.ast_of(struct_file);
    auto const* const when = c.find_attribute(struct_file, ast.at(cases[case_index].case_declaration).attributes, "when");
    if (when == nullptr || ast.at(when->arguments).size() != 1)
        return fail();
    auto const condition = ast.at(when->arguments)[0].value;

    // the fields it reads, as the root holds them or a case above it does
    auto names = cc::vector<cc::string_view>();
    auto const reads_all = !c.when_names(struct_file, condition, names);
    struct readable
    {
        cc::string_view name;
        ast::field_id field;
        type_id type;
        i32 member = -1;
        i32 variant = -1;
    };
    auto fields = cc::vector<readable>();
    auto const members = c.out.at(t.members);
    for (auto k = isize(0); k < members.size(); ++k)
        if (!members[k].is_hidden)
            fields.push_back(
                {.name = members[k].name, .field = members[k].field, .type = members[k].type, .member = i32(k)});
    auto const variants = c.out.at(t.variant_fields);
    for (auto up = cases[case_index].parent; up >= 0; up = cases[up].parent)
        for (auto k = isize(0); k < variants.size(); ++k)
            if (variants[k].parent == up)
                fields.push_back(
                    {.name = variants[k].name, .field = variants[k].field, .type = variants[k].type, .variant = i32(k)});

    auto const label = add_label("when");
    auto const where = origin{.file = file(), .expr = from};
    auto outer = cc::move(block);
    block = {};
    auto bound = cc::vector<bound_name>();
    for (auto const& f : fields)
    {
        auto is_read = reads_all;
        for (auto const n : names)
            is_read = is_read || n == f.name;
        if (!is_read)
            continue;
        auto const value = f.member >= 0
                             ? add_expr(f.type, from, flat_member{.object = again(object, from), .member = f.member})
                             : read_variant(object, carrier, f.variant, from);
        auto const local = add_local(local_kind::temporary, f.name, f.type);
        add_stmt(where, flat_let{.local = local, .value = value});
        bound.push_back({.where = {.kind = target_kind::parameter, .index = i32(f.field)}, .local = local});
    }
    frames.push_back({.function = current()->function,
                      .file = struct_file,
                      .result = bool_type(),
                      .chain = current()->chain,
                      .bound = cc::move(bound)});
    auto const holds = flatten_expr(condition);
    frames.remove_back();
    add_stmt(where, flat_leave{.target = label, .value = holds});
    auto const body = add_list(block);
    block = cc::move(outer);
    return add_expr(bool_type(), from, flat_block{.label = label, .body = body});
}

flat_expr_id flattener::cases_test(flat_expr_id object, type_id carrier, u32 tested, ast::expr_id from)
{
    auto const& t = c.out.at(carrier);
    auto const every = t.every_leaf();
    tested &= every;
    if (tested == 0)
        return add_expr(bool_type(), from, flat_bool_literal{.value = false});
    if (tested == every && t.is_covered)
        return add_expr(bool_type(), from, flat_bool_literal{.value = true});
    auto const cases = c.out.at(t.struct_cases);
    auto const has_when = [&](isize i)
    {
        auto const struct_file = c.out.at(t.symbol).file;
        return c.find_attribute(struct_file, c.ast_of(struct_file).at(cases[i].case_declaration).attributes, "when")
            != nullptr;
    };

    // CHK-541: the fewest cases whose leaves together are the tested ones, each tested along its path
    auto const node_test = [&](isize i) -> flat_expr_id
    {
        auto path = cc::vector<isize>();
        for (auto up = i; up >= 0; up = cases[up].parent)
            path.insert_at(0, up);
        auto conditions = cc::vector<flat_expr_id>();
        auto tagged = isize(-1);
        for (auto const j : path)
            if (!has_when(j))
                tagged = j;
        if (tagged >= 0 && t.tag_member >= 0)
        {
            auto const tag = [&]
            {
                return add_expr(c.prelude_type(builtins::k_uint), from,
                                flat_member{.object = again(object, from), .member = t.tag_member});
            };
            auto const lo = lowest_leaf(cases[tagged].leaves);
            auto const hi = highest_leaf(cases[tagged].leaves);
            if (lo == hi)
                conditions.push_back(compared("==", tag(), uint_literal(u32(lo), from), from));
            else
            {
                if (lo > 0)
                    conditions.push_back(compared(">=", tag(), uint_literal(u32(lo), from), from));
                conditions.push_back(compared("<=", tag(), uint_literal(u32(hi), from), from));
            }
        }
        for (auto const j : path)
        {
            if (!has_when(j))
                continue;
            // CHK-546: an unchecked level's first case that holds is the value's, so the ones before it fail
            auto is_ordered = false;
            for (auto k = isize(0); k < cases.size(); ++k)
                is_ordered = is_ordered || (cases[k].parent == cases[j].parent && cases[k].is_unchecked);
            if (is_ordered)
                for (auto k = isize(0); k < j; ++k)
                    if (cases[k].parent == cases[j].parent)
                        conditions.push_back(
                            add_expr(bool_type(), from, flat_not{.operand = when_test(object, carrier, i32(k), from)}));
            conditions.push_back(when_test(object, carrier, i32(j), from));
        }
        if (conditions.empty())
            return add_expr(bool_type(), from, flat_bool_literal{.value = true});
        auto result = conditions[0];
        for (auto k = isize(1); k < conditions.size(); ++k)
            result = add_expr(bool_type(), from, flat_and{.lhs = result, .rhs = conditions[k]});
        return result;
    };
    auto terms = cc::vector<flat_expr_id>();
    auto const walk = [&](auto const& self, i32 parent) -> void
    {
        for (auto i = isize(0); i < cases.size(); ++i)
        {
            if (cases[i].parent != parent || (cases[i].leaves & tested) == 0)
                continue;
            if ((cases[i].leaves & ~tested) == 0)
                terms.push_back(node_test(i));
            else
                self(self, i32(i));
        }
    };
    walk(walk, -1);
    if (terms.empty())
        return add_expr(bool_type(), from, flat_bool_literal{.value = false});
    auto result = terms[0];
    for (auto k = isize(1); k < terms.size(); ++k)
        result = add_expr(bool_type(), from, flat_or{.lhs = result, .rhs = terms[k]});
    return result;
}

// ---- the expressions --------------------------------------------------------------------------------------------------

flat_expr_id flattener::flatten_case_test(ast::expr_id id, ast::case_test const& node)
{
    auto const& where = tables().target_at(id);
    auto const carrier = tables().type_at(node.value);
    if (where.kind != target_kind::struct_cases || !is_valid(carrier))
        return fail();
    auto const tested = u32(where.index);
    auto const object = flatten_expr(node.value);
    return over_value(object, bool_type(), id, false, [&](flat_expr_id o) { return cases_test(o, carrier, tested, id); });
}

flat_expr_id flattener::flatten_variant_read(ast::expr_id id, ast::member const& member)
{
    auto const& where = tables().target_at(id);
    auto const carrier = tables().type_at(member.object);
    auto const type = tables().type_at(id);
    auto const object = flatten_expr(member.object);
    return over_value(object, type, id, false, [&](flat_expr_id o) { return read_variant(o, carrier, where.index, id); });
}

flat_expr_id flattener::guard_cases(flat_expr_id value, type_id carrier, u32 cases, ast::expr_id from, cc::string message)
{
    // CHK-550: a guard `SC_SHADER_UNDEFINED` keeps, which no other compile writes (LEGAL-73)
    return over_value(value, carrier, from, true,
                      [&](flat_expr_id o)
                      {
                          auto const condition = cases_test(o, carrier, cases, from);
                          guard(condition, from, cc::move(message));
                          return again(o, from);
                      });
}

void flattener::guard(flat_expr_id condition, ast::expr_id from, cc::string message)
{
    auto const site = i32(entry.check_sites.size());
    auto const where = origin{.file = file(), .expr = from};
    entry.messages.push_back({.pieces = {cc::move(message)}});
    entry.check_sites.push_back(
        {.from = where, .stops = true, .message = i32(entry.messages.size() - 1), .guards_undefined = true});
    auto const local = add_local(local_kind::var, "holds", bool_type());
    flat_stmt_id const body[] = {make_stmt(where, flat_var{.local = local, .value = condition})};
    flat_check_node const node[] = {{.value = local}};
    entry.check_sites[site].nodes = {.first = u32(entry.check_nodes.size()), .count = 1};
    entry.check_nodes.push_back_range(node);
    entry.check_sites[site].loop_variables = add_list(cc::span<flat_expr_id const>());
    add_stmt(where, flat_check{.site = site, .body = add_list(body)});
}

flat_expr_id flattener::flatten_case_cast(ast::expr_id id, ast::cast const& node)
{
    auto const& where = tables().target_at(id);
    auto const carrier = tables().type_at(id);
    auto const value = flatten_expr(node.value);
    return guard_cases(value, carrier, u32(where.index), id,
                       cc::format("a {} that is not {}, which `as` stated it is", c.out.name_of(carrier),
                                  c.case_names_of(carrier, u32(where.index))));
}

flat_expr_id flattener::construct_case(ast::expr_id id, type_id type, symbol_id callee, cc::span<flat_expr_id const> arguments)
{
    auto const& info = c.out.functions[c.out.at(callee).info];
    auto const& t = c.out.at(type);
    auto const parameters = c.out.at(info.parameters);
    auto const cases = c.out.at(t.struct_cases);
    auto const& leaf = cases[info.case_index];
    auto const struct_file = c.out.at(t.symbol).file;
    auto const& ast = c.ast_of(struct_file);
    auto const members = c.out.at(t.members);
    auto const variants = c.out.at(t.variant_fields);

    auto const argument_of = [&](ast::field_id field) -> flat_expr_id
    {
        for (auto p = isize(0); p < parameters.size() && p < arguments.size(); ++p)
            if (parameters[p].field == field)
                return arguments[p];
        return flat_expr_id::none;
    };
    // CHK-546: the fields the `@when`s on the path fix, which the constructor writes
    auto fixed = cc::vector<cc::pair<cc::string_view, ast::expr_id>>();
    for (auto up = info.case_index; up >= 0; up = cases[up].parent)
        if (auto const* const when = c.find_attribute(struct_file, ast.at(cases[up].case_declaration).attributes, "when");
            when != nullptr && ast.at(when->arguments).size() == 1)
            (void)c.equalities_of(struct_file, ast.at(when->arguments)[0].value, fixed);
    auto const fixed_value = [&](cc::string_view name) -> flat_expr_id
    {
        for (auto const& [n, e] : fixed)
        {
            if (n != name)
                continue;
            // a bare field is the field being true, and its `not` the field being false
            if (ast.at(e).node.is<ast::name>())
                return add_expr(bool_type(), id, flat_bool_literal{.value = true});
            if (auto const* const call = ast.at(e).node.try_as<ast::call>();
                call != nullptr && call->spelling == ast::call_spelling::prefix)
                return add_expr(bool_type(), id, flat_bool_literal{.value = false});
            frames.push_back({.function = current()->function, .file = struct_file, .chain = current()->chain});
            auto const value = flatten_expr(e);
            frames.remove_back();
            return value;
        }
        return flat_expr_id::none;
    };

    auto words = cc::vector<flat_expr_id>();
    for (auto w = 0; w < t.word_count; ++w)
        words.push_back(uint_literal(0, id));
    for (auto const& v : variants)
    {
        if ((v.leaves & leaf.leaves) == 0)
            continue;
        auto value = argument_of(v.field);
        if (!is_valid(value))
            value = fixed_value(v.name);
        if (!is_valid(value))
            return fail();
        auto encoded = cc::vector<flat_expr_id>();
        if (!encode_words(value, v.type, id, encoded) || encoded.size() != v.word_count)
            return fail();
        for (auto w = isize(0); w < encoded.size(); ++w)
            words[v.word + w] = encoded[w];
    }

    auto values = cc::vector<flat_expr_id>();
    for (auto k = isize(0); k < members.size(); ++k)
    {
        auto const& m = members[k];
        if (i32(k) == t.tag_member)
            values.push_back(uint_literal(u32(leaf.leaf), id));
        else if (m.is_hidden)
            values.push_back(words[i32(k) - t.first_word]);
        else
        {
            auto value = argument_of(m.field);
            if (!is_valid(value))
                value = fixed_value(m.name);
            if (!is_valid(value))
                return fail();
            values.push_back(value);
        }
    }
    return add_expr(type, id, flat_construct{.arguments = add_list(values)});
}

flat_case flattener::flatten_struct_case_parts(ast::case_expr const& node, label_id value_block)
{
    auto const carrier = tables().type_at(node.value);
    auto const& t = c.out.at(carrier);
    auto const uint_type = c.prelude_type(builtins::k_uint);
    auto const cases = c.out.at(t.struct_cases);
    auto is_tagged_alone = t.tag_member >= 0;
    auto const struct_file = c.out.at(t.symbol).file;
    for (auto const& k : cases)
        is_tagged_alone
            = is_tagged_alone
           && c.find_attribute(struct_file, c.ast_of(struct_file).at(k.case_declaration).attributes, "when") == nullptr;

    // each arm's cases, those of the arms before it taken away
    struct arm_cases
    {
        ast::case_arm const* arm = nullptr;
        u32 cases = 0;
        bool is_wildcard = false;
    };
    auto arms = cc::vector<arm_cases>();
    auto remaining = t.every_leaf();
    for (auto const& arm : ast().at(node.arms))
    {
        auto const is_wildcard = ast::is_valid(arm.pattern) && ast().at(arm.pattern).node.is<ast::wildcard>();
        auto const& where = tables().target_at(arm.pattern);
        auto const named = is_wildcard ? remaining : where.kind == target_kind::struct_cases ? u32(where.index) : 0;
        arms.push_back({.arm = &arm, .cases = named & remaining, .is_wildcard = is_wildcard});
        remaining &= ~named;
    }

    // without a `_` the last arm that can run is the default, which nothing needs to test
    auto has_wildcard = false;
    auto last_live = isize(-1);
    for (auto i = isize(0); i < arms.size(); ++i)
    {
        has_wildcard = has_wildcard || arms[i].is_wildcard;
        if (!arms[i].is_wildcard && arms[i].cases != 0)
            last_live = i;
    }

    auto result = flat_case{};
    auto const value = flatten_expr(node.value);
    if (is_tagged_alone)
    {
        // CHK-551: the tag says the case, which a target switches on
        result.scrutinee = add_expr(uint_type, node.value, flat_member{.object = value, .member = t.tag_member});
    }
    else
    {
        // the first arm whose cases hold, as its position, which a target switches on
        result.scrutinee = over_value(
            value, int_type(), node.value, true,
            [&](flat_expr_id o)
            {
                auto const where = origin{.file = file(), .expr = node.value};
                auto const label = add_label("arm");
                auto outer = cc::move(block);
                block = {};
                for (auto i = isize(0); i < arms.size(); ++i)
                {
                    if (arms[i].is_wildcard || arms[i].cases == 0 || (!has_wildcard && i == last_live))
                        continue;
                    flat_stmt_id const taken[] = {make_stmt(
                        where, flat_leave{.target = label,
                                          .value = add_expr(int_type(), node.value, flat_int_literal{.value = i32(i)})})};
                    add_stmt(where, flat_if{.condition = cases_test(o, carrier, arms[i].cases, node.value),
                                            .then_body = add_list(taken)});
                }
                add_stmt(where, flat_leave{.target = label,
                                           .value = add_expr(int_type(), node.value, flat_int_literal{.value = -1})});
                auto const body = add_list(block);
                block = cc::move(outer);
                return add_expr(int_type(), node.value, flat_block{.label = label, .body = body});
            });
    }
    type_id const both[] = {entry.at(result.scrutinee).type, entry.at(result.scrutinee).type};
    result.equality = c.find_operator(file(), "==", both);
    if (is_valid(result.equality))
        result.equality_intrinsic = c.out.at(result.equality).intrinsic;

    auto flat_arms = cc::vector<flat_arm>();
    auto has_default = false;
    for (auto i = isize(0); i < arms.size(); ++i)
    {
        auto const body = flatten_arm_body(arms[i].arm->result, value_block);
        if (arms[i].is_wildcard)
        {
            result.default_body = body;
            has_default = true;
            continue;
        }
        // an arm the arms before it took every case of never runs
        if (arms[i].cases == 0)
            continue;
        auto patterns = cc::vector<flat_expr_id>();
        if (is_tagged_alone)
        {
            for (auto const& k : cases)
                if (k.leaf >= 0 && (arms[i].cases & (u32(1) << k.leaf)) != 0)
                    patterns.push_back(uint_literal(u32(k.leaf), arms[i].arm->pattern));
        }
        else
            patterns.push_back(add_expr(int_type(), arms[i].arm->pattern, flat_int_literal{.value = i32(i)}));
        flat_arms.push_back({.patterns = add_list(patterns), .body = body});
    }
    if (!has_default && !flat_arms.empty())
    {
        result.default_body = flat_arms.back().body;
        flat_arms.remove_back();
    }
    result.arms = add_arms(flat_arms);
    return result;
}

bool flattener::flatten_variant_assign(origin from, ast::assign_stmt const& assign)
{
    auto const& member = ast().at(assign.target).node.as<ast::member>();
    auto const& where = tables().target_at(assign.target);
    auto const carrier = tables().type_at(member.object);
    auto const& t = c.out.at(carrier);
    auto const& v = c.out.at(t.variant_fields)[where.index];
    auto const op = c.text_of(file(), c.file_of(file()).at(assign.op).where);

    auto const place = flatten_expr(member.object);
    if (!is_valid(place))
        return false;
    if (has_array_index(place))
    {
        failure_detail = "a field of a case of an array's element as a place, which is not supported yet";
        mark_failed();
        return true;
    }
    auto value = flatten_expr(assign.value);
    if (!is_valid(value))
        return false;
    if (op != "=")
    {
        auto const read = over_value(flatten_expr(member.object), v.type, assign.target, false, [&](flat_expr_id o)
                                     { return read_variant(o, carrier, where.index, assign.target); });
        type_id const types[] = {v.type, entry.at(value).type};
        auto const callee = c.find_operator(file(), op.subview({.offset = 0, .size = op.size() - 1}), types);
        if (!is_valid(callee))
            return false;
        flat_expr_id const operands[] = {read, value};
        value = builtin_call(assign.value, callee, operands);
    }
    value = guard_range(value, v, assign.value);
    if (!is_substitutable(value))
    {
        auto const local = add_local(local_kind::temporary, v.name, v.type);
        add_stmt(from, flat_let{.local = local, .value = value});
        value = local_ref(local, assign.value);
    }
    auto words = cc::vector<flat_expr_id>();
    if (!encode_words(value, v.type, assign.value, words))
        return false;
    auto const uint_type = c.prelude_type(builtins::k_uint);
    for (auto w = isize(0); w < words.size(); ++w)
    {
        auto const object = w == 0 ? place : flatten_expr(member.object);
        auto const word = add_expr(uint_type, assign.target,
                                   flat_member{.object = object, .member = t.first_word + v.word + i32(w)});
        judge_decision_store(word, assign.target);
        add_stmt(from, flat_assign{.place = word, .value = words[w]});
    }
    return true;
}

// ---- fields with a range ----------------------------------------------------------------------------------------------

flat_expr_id flattener::guard_range(flat_expr_id value, member_info const& field, ast::expr_id from)
{
    if (!field.has_range || !is_valid(value))
        return value;
    auto const type = entry.at(value).type;
    auto const is_unsigned = type == c.prelude_type(builtins::k_uint);
    // a literal in the range needs no check, which the check pass made sure of
    if (auto const* const literal = entry.at(value).node.try_as<flat_int_literal>())
    {
        auto const v = literal->is_unsigned ? i64(u32(literal->value)) : i64(literal->value);
        if (v >= field.range_first && v <= field.range_last)
            return value;
    }
    auto const bound
        = [&](i64 v) { return add_expr(type, from, flat_int_literal{.value = i32(v), .is_unsigned = is_unsigned}); };
    // CHK-549: a value stored into a field with a range is one of it, which `SC_SHADER_UNDEFINED` checks (LEGAL-73)
    return over_value(value, type, from, true,
                      [&](flat_expr_id o)
                      {
                          auto const above = compared("<=", bound(field.range_first), o, from);
                          auto const below = compared("<=", again(o, from), bound(field.range_last), from);
                          guard(add_expr(bool_type(), from, flat_and{.lhs = above, .rhs = below}), from,
                                cc::format("a value outside {} to {}, which {} holds", field.range_first,
                                           field.range_last, field.name));
                          return again(o, from);
                      });
}
