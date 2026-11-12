#include <shaped-graphics-language/check/impl/expr_helpers.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
constexpr auto error_type = checked_module::error_type;
constexpr auto void_type = checked_module::void_type;
} // namespace

// ---- overloads -----------------------------------------------------------------------------------------------------

void checker::note_near_misses(i32 file,
                               ast::expr_id call,
                               cc::span<symbol_id const> candidates,
                               call_arguments const& arguments)
{
    // CHK-338: a call in a generic body with its type parameters open is no miss, since each instance checks it again
    if (generic_passes > 0)
        return;
    for (auto const candidate : candidates)
    {
        if (out.at(candidate).state != symbol_state::checked)
            continue;
        auto const parameters
            = cc::vector<parameter>::create_copy_of(out.at(out.functions[out.at(candidate).info].parameters));
        auto const bound = bind_arguments(parameters, arguments);
        auto miss = near_miss{.file = file,
                              .call = call,
                              .candidate = candidate,
                              .reason = bound.failure,
                              .argument = bound.argument,
                              .parameter = bound.parameter};
        for (auto p = isize(0); miss.reason == miss_reason::none && p < parameters.size(); ++p)
        {
            auto const i = bound.slots[p];
            if (i >= 0 && parameters[p].is_mut != arguments.written[i].is_mut)
                miss = {.file = file,
                        .call = call,
                        .candidate = candidate,
                        .reason = miss_reason::mut_mismatch,
                        .argument = i,
                        .parameter = i32(p)};
            else if (i >= 0 && !chain_of(file, parameters[p], arguments, i).has_value())
                miss = {.file = file,
                        .call = call,
                        .candidate = candidate,
                        .reason = miss_reason::no_conversion,
                        .argument = i,
                        .parameter = i32(p)};
        }
        out.near_misses.push_back(miss);
    }
}

cc::string checker::miss_hint(function_scope& scope,
                              ast::expr_id callee,
                              cc::span<symbol_id const> candidates,
                              call_arguments const& arguments,
                              cc::string_view spelling)
{
    auto const file = scope.file;
    auto const is_condition = [&](type_id type)
    {
        auto const* const record = out.builtin_type_of(type);
        return record != nullptr && record->leaf_kind == value_kind::boolean;
    };
    // a dot call writes its receiver ahead of the name, where no mark can stand
    auto const is_dot_call = ast::is_valid(callee) && ast_of(file).at(callee).node.is<ast::member>();
    auto const argument_text = [&](isize i) { return text_of(file, span_of(file, arguments.written[i].expr)); };
    auto rotated_chain = i32(-1);
    auto rotated_types = cc::vector<type_id>();
    for (auto const candidate : candidates)
    {
        if (out.at(candidate).state != symbol_state::checked)
            continue;
        auto const parameters
            = cc::vector<parameter>::create_copy_of(out.at(out.functions[out.at(candidate).info].parameters));
        auto is_generic = false;
        for (auto const& p : parameters)
            is_generic = is_generic || is_open(p.type);
        if (is_generic)
            continue;

        // CHK-316: the marks alone are off, and each place is of its parameter's type exactly
        auto const bound = bind_arguments(parameters, arguments);
        if (bound.failure == miss_reason::none)
        {
            auto flipped = cc::vector<isize>();
            auto is_marks_away = true;
            for (auto p = isize(0); is_marks_away && p < parameters.size(); ++p)
            {
                auto const i = bound.slots[p];
                if (i < 0)
                    continue;
                if (parameters[p].is_mut != arguments.written[i].is_mut)
                {
                    is_marks_away = arguments.types[i] == parameters[p].type;
                    flipped.push_back(p);
                }
                else
                    is_marks_away = chain_of(file, parameters[p], arguments, i).has_value();
            }
            if (is_marks_away && !flipped.empty())
            {
                // a missing mark is the fix only where what it would mark is a place a body could assign
                for (auto const p : flipped)
                {
                    auto why = cc::string();
                    if (parameters[p].is_mut
                        && !judge_place(scope, arguments.written[bound.slots[p]].expr, "a mut argument", &why))
                        return cc::format(": {} is a mut parameter, so its argument is a place marked `mut`, and `{}` "
                                          "is none: {}",
                                          parameters[p].name, argument_text(bound.slots[p]), why);
                }
                if (flipped.size() == 1 && !is_dot_call)
                {
                    auto const& p = parameters[flipped[0]];
                    if (p.is_mut)
                        return cc::format(": {} is a mut parameter, so its argument is a place marked `mut`, as in "
                                          "`mut {}`",
                                          p.name, argument_text(bound.slots[flipped[0]]));
                    return cc::format(": {} is no mut parameter, so its argument takes no `mut`", p.name);
                }

                // several marks, or a dot call, are shown as the whole call written with every mark right
                auto missing = cc::vector<cc::string_view>();
                auto extra = cc::vector<cc::string_view>();
                auto is_toggled = cc::vector<bool>::create_filled(arguments.written.size(), false);
                for (auto const p : flipped)
                {
                    (parameters[p].is_mut ? missing : extra).push_back(parameters[p].name);
                    is_toggled[bound.slots[p]] = true;
                }
                auto const listed = [](cc::span<cc::string_view const> names)
                {
                    auto text = cc::string();
                    for (auto i = isize(0); i < names.size(); ++i)
                    {
                        if (i > 0)
                            text += i + 1 == names.size() ? " and " : ", ";
                        text += names[i];
                    }
                    return text;
                };
                auto call = cc::format("{}(", spelling);
                for (auto i = isize(0); i < arguments.written.size(); ++i)
                {
                    if (i > 0)
                        call += ", ";
                    if (!arguments.names[i].empty())
                        call += cc::format("{} = ", arguments.names[i]);
                    if (arguments.written[i].is_mut != is_toggled[i])
                        call += "mut ";
                    call += argument_text(i);
                }
                call += ")";
                auto said = cc::vector<cc::string>();
                if (!missing.empty())
                    said.push_back(
                        missing.size() == 1
                            ? cc::format("{} is a mut parameter, so its argument is a place marked `mut`", missing[0])
                            : cc::format("{} are mut parameters, so their arguments are places marked `mut`",
                                         listed(missing)));
                if (!extra.empty())
                    said.push_back(
                        extra.size() == 1
                            ? cc::format("{} is no mut parameter, so its argument takes no `mut`", extra[0])
                            : cc::format("{} are no mut parameters, so their arguments take no `mut`", listed(extra)));
                return cc::format(": {}, as in `{}`", said.size() == 1 ? said[0] : said[0] + "; " + said[1], call);
            }
        }

        // CHK-365: `select(a, b, cond)` is WGSL's order, and SGL's condition stands first
        auto const n = arguments.written.size();
        if (n >= 2 && isize(parameters.size()) == n && is_condition(parameters[0].type) && arguments.names.size() == n
            && arguments.types[n - 1] == parameters[0].type)
        {
            // the candidate the moved arguments convert to most directly is the one the call meant
            auto is_rotated = true;
            auto total = i32(0);
            for (auto i = isize(0); is_rotated && i < n; ++i)
            {
                auto const chain = arguments.names[i].empty() && !arguments.written[i].is_mut && !parameters[i].is_mut
                                     ? chain_of(file, parameters[i], arguments, i == 0 ? n - 1 : i - 1)
                                     : cc::nullopt;
                is_rotated = chain.has_value();
                total += chain.has_value() ? chain.value() : 0;
            }
            if (is_rotated && (rotated_chain < 0 || total < rotated_chain))
            {
                rotated_chain = total;
                rotated_types.clear();
                for (auto const& p : parameters)
                    rotated_types.push_back(p.type);
            }
        }
    }
    if (rotated_chain >= 0)
        return cc::format(": its condition comes first, as in {}", signature_text(spelling, rotated_types));

    // CHK-458: a float's builtin with no double overload is one no target computes in 64 bits
    auto const is_kind = [&](type_id type, value_kind kind)
    {
        auto const* const record = out.builtin_type_of(type);
        return record != nullptr && record->leaf_kind == kind;
    };
    auto has_double = false;
    for (auto const type : arguments.types)
        has_double = has_double || is_kind(type, value_kind::scalar_double);
    auto takes_float = false;
    auto takes_double = false;
    for (auto const candidate : candidates)
        if (out.at(candidate).state == symbol_state::checked && out.at(candidate).info >= 0
            && is_valid(out.at(candidate).intrinsic))
            for (auto const& p : out.at(out.functions[out.at(candidate).info].parameters))
            {
                takes_float = takes_float || is_kind(p.type, value_kind::scalar_float);
                takes_double = takes_double || is_kind(p.type, value_kind::scalar_double);
            }
    if (has_double && takes_float && !takes_double)
        return cc::format(": no target computes `{}` of a double, so the prelude has none; `x as float` takes the "
                          "float's",
                          spelling);
    return {};
}

cc::optional<call_arguments> checker::converted_for_plain_vector(i32 file,
                                                                 source_span where,
                                                                 cc::span<symbol_id const> candidates,
                                                                 call_arguments const& arguments)
{
    // the vector a constructor among the candidates builds: `float3`, `int2` or `uint4`, and never `vec3` or `pos3`
    auto const* vector = static_cast<builtins::type_record const*>(nullptr);
    auto vector_type = error_type;
    for (auto const candidate : candidates)
    {
        if (out.at(candidate).role != function_role::constructor || out.at(candidate).info < 0)
            continue;
        auto const result = out.functions[out.at(candidate).info].result;
        auto const* const record = out.builtin_type_of(result);
        auto const family = record != nullptr ? family_of(record->leaf_kind) : cc::string_view();
        if (record == nullptr || record->leaf_count < 2 || record->leaf_count > 4
            || (family != "float" && family != "int" && family != "uint")
            || out.name_of(result) != cc::format("{}{}", family, record->leaf_count))
            continue;
        vector = record;
        vector_type = result;
    }
    if (vector == nullptr)
        return {};

    // each scalar of another kind of one width converts as `as` converts it; a literal converts by CHK-253 already
    auto const scalar = prelude_type(family_of(vector->leaf_kind));
    auto converted = arguments;
    auto is_changed = false;
    for (auto i = isize(0); i < converted.written.size(); ++i)
    {
        auto const* const record = out.builtin_type_of(converted.types[i]);
        auto const family = record != nullptr ? family_of(record->leaf_kind) : cc::string_view();
        if (record == nullptr || record->leaf_count != 1 || converted.numbers[i].is_number
            || converted.written[i].splat_member >= 0 || converted.written[i].is_function
            || record->leaf_kind == vector->leaf_kind || (family != "float" && family != "int" && family != "uint"))
            continue;
        auto const conversion = conversion_of(file, where, converted.types[i], scalar);
        if (!is_valid(conversion))
            continue;
        converted.types[i] = scalar;
        converted.written[i].conversion = conversion;
        is_changed = true;
    }
    if (!is_changed)
        return {};
    for (auto const candidate : candidates)
        if (out.at(candidate).role == function_role::constructor && out.at(candidate).info >= 0
            && out.functions[out.at(candidate).info].result == vector_type
            && match(file, candidate, converted).has_value())
            return converted;
    return {};
}

cc::optional<candidate_match> checker::match(i32 file, symbol_id candidate, call_arguments const& arguments)
{
    // Read in place, by position: a literal argument may compile another function, and `out.parameters` then moves.
    auto const range = out.functions[out.at(candidate).info].parameters;
    // Most candidates of a call fail on their count alone, which is decided before anything is allocated.
    auto required = isize(0);
    for (auto const& p : out.at(range))
        required += p.has_default ? 0 : 1;
    if (arguments.written.size() > isize(range.count) || arguments.written.size() < required)
        return cc::nullopt;
    // what `chain_of` reads of a parameter, copied without its name since a compile may move the original
    auto const shape_of
        = [&](isize p) { return parameter{.type = out.at(range)[p].type, .is_mut = out.at(range)[p].is_mut}; };
    auto const* const record = out.builtin_function(out.at(candidate).intrinsic);
    auto const takes_literal_at_default = record != nullptr && record->takes_literal_at_default;
    auto bound = bind_arguments(out.at(range), arguments);
    if (bound.failure != miss_reason::none)
        return cc::nullopt;
    auto result = candidate_match{.candidate = candidate,
                                  .slots = cc::move(bound.slots),
                                  .chains = cc::vector<i32>::create_filled(arguments.written.size(), 0)};
    // CHK-340: stated generic arguments bind first, and a candidate they do not fit by position or name is pruned
    if (!arguments.stated.empty())
    {
        auto const type_parameters
            = cc::vector<type_id>::create_copy_of(out.at(out.functions[out.at(candidate).info].type_parameters));
        auto positional = isize(0);
        for (auto const& s : arguments.stated)
        {
            auto parameter = type_id::none;
            if (s.name.empty())
                parameter = positional < type_parameters.size() ? type_parameters[positional++] : type_id::none;
            else
                for (auto const q : type_parameters)
                    if (out.at(q).spelled == s.name)
                        parameter = q;
            if (!is_valid(parameter))
                return cc::nullopt;
            // a value parameter takes a constant of its type, and a type parameter a type
            auto const& given = out.at(s.type);
            auto const wants_value = is_valid(out.at(parameter).element);
            if ((given.kind == type_kind::constant) != wants_value
                || (wants_value && given.element != out.at(parameter).element))
                return cc::nullopt;
            for (auto k = isize(0); k + 1 < result.bindings.size(); k += 2)
                if (result.bindings[k] == parameter)
                    return cc::nullopt;
            result.bindings.push_back(parameter);
            result.bindings.push_back(s.type);
        }
    }
    // CHK-340: a parameter whose type names a type parameter binds it to what the argument is, exactly; a function
    // and a literal wait until the rest are bound, since what they meet depends on it
    auto deferred = cc::vector<isize>();
    for (auto p = isize(0); p < isize(range.count); ++p)
    {
        auto const i = result.slots[p];
        if (i < 0 || arguments.undefineds[i])
            continue;
        if (is_open(out.at(range)[p].type))
        {
            // a leading dot names a case of the enum it meets, which a type parameter does not say
            if (!arguments.leading_dots[i].empty())
                return cc::nullopt;
            // CHK-423: `scalar_of[V]` says nothing of `V`, so its argument waits until the rest have bound it
            if (arguments.functions[i] >= 0 || arguments.literals[i] >= 0 || arguments.numbers[i].is_number
                || out.at(out.at(range)[p].type).kind == type_kind::scalar_of)
                deferred.push_back(p);
            else if (out.at(range)[p].is_mut
                         != (arguments.written[i].is_mut || (arguments.written[i].may_be_mut && out.at(range)[p].is_mut))
                     || !unify(out.at(range)[p].type, arguments.types[i], result.bindings))
                return cc::nullopt;
            continue;
        }
        auto const length = chain_of(file, shape_of(p), arguments, i);
        if (!length.has_value())
            return cc::nullopt;
        // CHK-396: `T(x)` of a literal is `x as T`, which takes the literal at its default type alone
        if (length.value() > 0 && arguments.numbers[i].is_number && takes_literal_at_default)
            return cc::nullopt;
        result.chains[i] = length.value();
    }
    // CHK-340: a number fits a set of types, so a type parameter only numbers meet is bound to one all of them fit:
    // the type expected where the call stands, else a float where one of them is written as a float, else the first's
    for (auto const p : deferred)
    {
        auto const i = result.slots[p];
        auto const parameter = out.at(range)[p].type;
        if (!arguments.numbers[i].is_number || out.at(parameter).kind != type_kind::type_parameter)
            continue;
        auto is_bound = false;
        for (auto k = isize(0); k + 1 < result.bindings.size(); k += 2)
            is_bound = is_bound || result.bindings[k] == parameter;
        if (is_bound)
            continue;
        auto to = type_id::none;
        if (is_valid(expected_result) && expected_result != error_type)
        {
            auto trial = result.bindings;
            if (unify(out.functions[out.at(candidate).info].result, expected_result, trial))
                for (auto k = isize(0); k + 1 < trial.size(); k += 2)
                    if (trial[k] == parameter)
                        to = trial[k + 1];
        }
        // a parameter's default, where it is closed and every number meeting it fits, before a number's own type
        if (auto const fallback = out.at(parameter).generic; !is_valid(to) && is_valid(fallback) && !is_open(fallback))
        {
            auto fits = true;
            for (auto const q : deferred)
                if (auto const j = result.slots[q]; out.at(range)[q].type == parameter && arguments.numbers[j].is_number)
                    fits = fits
                        && (out.name_of(fallback) == "float" || out.name_of(fallback) == "half"
                            || arguments.numbers[j].is_integer);
            if (fits)
                to = fallback;
        }
        for (auto const q : deferred)
            if (auto const j = result.slots[q]; !is_valid(to) && out.at(range)[q].type == parameter
                                                && arguments.numbers[j].is_number && !arguments.numbers[j].is_integer)
                to = arguments.types[j];
        if (!is_valid(to))
            to = arguments.types[i];
        result.bindings.push_back(parameter);
        result.bindings.push_back(to);
    }
    for (auto const p : deferred)
    {
        auto const i = result.slots[p];
        if (arguments.functions[i] >= 0)
        {
            // by value: demanding a candidate may push to `function_arguments`
            auto const f = function_arguments[arguments.functions[i]];
            auto const& taking = out.at(out.at(range)[p].type);
            if (taking.kind != type_kind::function)
                return cc::nullopt;
            // a lambda binds what its body gives when it is checked, after the call chose this candidate
            if (f.is_lambda)
            {
                auto const& l = ast_of(file).at(f.expr).node.as<ast::lambda>();
                if (i64(ast_of(file).at(l.parameters).size()) != taking.members.count)
                    return cc::nullopt;
                continue;
            }
            auto chosen = cc::vector<type_id>();
            auto matching = 0;
            for (auto const g : f.functions)
            {
                if (out.at(g).state == symbol_state::in_compilation
                    || demand(g, out.at(g).file, {}) != symbol_state::checked)
                    continue;
                auto const& info = out.functions[out.at(g).info];
                if (info.entry_stage != stage::none || is_valid(out.at(g).intrinsic)
                    || !out.at(info.type_parameters).empty())
                    continue;
                // CHK-318: a function value's parameters are none of them `mut`
                auto types = cc::vector<type_id>();
                auto has_mut = false;
                for (auto const& q : out.at(info.parameters))
                {
                    types.push_back(q.type);
                    has_mut = has_mut || q.is_mut;
                }
                if (has_mut)
                    continue;
                auto trial = result.bindings;
                if (unify(out.at(range)[p].type, function_type(types, info.result), trial))
                {
                    ++matching;
                    chosen = cc::move(trial);
                }
            }
            if (matching != 1)
                return cc::nullopt;
            result.bindings = cc::move(chosen);
            continue;
        }
        auto taking = shape_of(p);
        taking.type = substitute(taking.type, result.bindings);
        // a signature its bindings make no type of, `scalar_of[float]`, is no candidate (CHK-423)
        if (is_open(taking.type) || taking.type == error_type)
            return cc::nullopt;
        auto const length = chain_of(file, taking, arguments, i);
        if (!length.has_value())
            return cc::nullopt;
        result.chains[i] = length.value();
    }
    return result;
}

cc::optional<i32> checker::chain_of(i32 file, parameter const& taking, call_arguments const& arguments, isize i)
{
    // CHK-341: `undefined()` is of whatever type it meets
    if (arguments.undefineds[i])
        return 0;
    // CHK-152: a leading dot meets an enum that has its case, and nothing else
    if (!arguments.leading_dots[i].empty())
        return case_index_of(taking.type, arguments.leading_dots[i]) >= 0 && !taking.is_mut ? cc::optional<i32>(0)
                                                                                            : cc::nullopt;
    // CHK-316: a place is handed over as it is, so it has the parameter's type exactly, and a mark meets a mut parameter
    // CHK-416: a receiver that is a place is marked wherever the parameter is `mut`
    auto const is_mut = arguments.written[i].is_mut || (arguments.written[i].may_be_mut && taking.is_mut);
    if (taking.is_mut || is_mut)
    {
        if (taking.is_mut != is_mut || arguments.types[i] != taking.type)
            return cc::nullopt;
        return 0;
    }
    auto const parameter = taking.type;
    // CHK-318: a function meets a parameter of function type whose signature it has exactly
    if (arguments.functions[i] >= 0)
    {
        if (!is_valid(parameter) || parameter == error_type || out.at(parameter).kind != type_kind::function)
            return cc::nullopt;
        auto const& f = function_arguments[arguments.functions[i]];
        if (f.is_lambda)
        {
            auto const& l = ast_of(file).at(f.expr).node.as<ast::lambda>();
            return i64(ast_of(file).at(l.parameters).size()) == out.at(parameter).members.count ? cc::optional<i32>(0)
                                                                                                : cc::nullopt;
        }
        return is_valid(function_of_type(f.functions, parameter)) ? cc::optional<i32>(0) : cc::nullopt;
    }
    if (arguments.literals[i] >= 0)
        return literal_chain(file, parameter, arguments.literals[i]);
    if (arguments.numbers[i].is_number)
    {
        // CHK-253: a number literal converts to what holds it, and leaving its default type, which is what it was
        // checked as, is one step of its chain
        if (!holds(arguments.numbers[i], parameter))
            return cc::nullopt;
        return parameter == arguments.types[i] ? 0 : 1;
    }
    return takes(parameter, arguments.types[i]) ? cc::optional<i32>(0) : cc::nullopt;
}

cc::vector<symbol_id> checker::functions_named_after(i32 file, type_id to) const
{
    auto result = cc::vector<symbol_id>();
    if (!is_valid(to) || to == error_type || out.at(to).kind != type_kind::structure)
        return result;
    auto const& name = out.at(out.at(to).symbol).name;
    if (auto const* const found = seen_from(file, name))
        for (auto const id : *found)
            if (out.at(id).kind == symbol_kind::function)
                result.push_back(id);
    // CHK-348: a struct another module declares converts by the functions of its name visible there, its constructor
    // among them, as a call on a value of it finds them (CHK-247)
    auto const declaring_file = out.at(out.at(to).symbol).file;
    if (!is_prelude_file(declaring_file) && (is_prelude_file(file) || file_module[declaring_file] != file_module[file]))
        if (auto const* const declared = module_scopes[file_module[declaring_file]].get_ptr(name))
            for (auto const id : *declared)
                if (out.at(id).kind == symbol_kind::function)
                    result.push_back(id);
    return result;
}

cc::optional<i32> checker::literal_chain(i32 file, type_id to, i32 literal)
{
    auto matches = cc::vector<candidate_match>();
    for (auto const candidate : functions_named_after(file, to))
    {
        if (out.at(candidate).state == symbol_state::in_compilation)
            continue;
        if (demand(candidate, file, {}) != symbol_state::checked)
            continue;
        // by value: matching may compile a body that shapes literals of its own, and `literals` then moves
        auto const arguments = literals[literal];
        if (auto m = match(file, candidate, arguments); m.has_value())
            matches.push_back(cc::move(m.value()));
    }
    auto const best = best_of(cc::move(matches));
    if (best.size() != 1)
        return cc::nullopt;
    auto longest = 0;
    for (auto const c : best.front().chains)
        longest = c > longest ? c : longest;
    return longest + 1;
}

cc::vector<candidate_match> checker::best_of(cc::vector<candidate_match> matches) const
{
    // CHK-192: a match of the program's file hides every match of the prelude
    auto is_program_match = false;
    for (auto const& m : matches)
        is_program_match = is_program_match || !is_prelude_file(out.at(m.candidate).file);
    if (is_program_match)
        matches.remove_all_where([&](candidate_match const& m) { return is_prelude_file(out.at(m.candidate).file); });

    // CHK-254: a candidate no worse at any argument and better at one is better; the best is better than every other
    auto const is_better = [](candidate_match const& a, candidate_match const& b)
    {
        auto is_shorter = false;
        for (auto i = isize(0); i < a.chains.size(); ++i)
        {
            if (a.chains[i] > b.chains[i])
                return false;
            is_shorter = is_shorter || a.chains[i] < b.chains[i];
        }
        return is_shorter;
    };
    auto result = cc::vector<candidate_match>();
    for (auto const& m : matches)
    {
        auto is_beaten = false;
        for (auto const& other : matches)
            is_beaten = is_beaten || is_better(other, m);
        if (!is_beaten)
            result.push_back(m);
    }
    if (result.size() < 2)
        return result;

    // CHK-255: only between candidates whose chains agree at every argument
    for (auto const& m : result)
        for (auto i = isize(0); i < m.chains.size(); ++i)
            if (m.chains[i] != result.front().chains[i])
                return result;
    auto const is_of_type = [&](candidate_match const& m)
    { return is_valid(out.at(m.candidate).owner) && out.at(m.candidate).role != function_role::constructor; };
    auto type_scoped = isize(0);
    for (auto const& m : result)
        type_scoped += is_of_type(m) ? 1 : 0;
    if (type_scoped > 0 && type_scoped < result.size())
        result.remove_all_where([&](candidate_match const& m) { return !is_of_type(m); });
    return result;
}

type_id checker::prelude_type(cc::string_view name) const
{
    auto const* const found = prelude_names.get_ptr(name);
    if (found == nullptr || found->empty() || out.at(found->front()).kind != symbol_kind::structure)
        return type_id::none;
    return out.at(found->front()).type;
}

namespace
{
/// Whether a half holds `n` exactly: 11 significant bits at most.
bool is_exact_half(i64 n)
{
    auto magnitude = u64(n < 0 ? -n : n);
    while (magnitude > 2048 && magnitude % 2 == 0)
        magnitude /= 2;
    return magnitude <= 2048;
}
} // namespace

bool checker::holds(number_literal const& n, type_id to) const
{
    if (!is_valid(to))
        return false;
    if (n.is_past_long)
        return to == prelude_type(builtins::k_ulong);
    if (to == prelude_type(builtins::k_int))
        return n.is_integer && n.integer >= -2147483648ll && n.integer <= 2147483647ll;
    if (to == prelude_type(builtins::k_uint))
        return n.is_integer && n.integer >= 0 && n.integer <= 4294967295ll;
    if (to == prelude_type(builtins::k_float))
    {
        // an integer converts only where the float holds it exactly, and a float rounds to nearest within range:
        // below FLT_MAX plus half an ulp, where rounding to nearest would give infinity
        if (n.is_integer)
            return f64(f32(n.integer)) == f64(n.integer);
        auto const magnitude = n.real < 0 ? -n.real : n.real;
        return magnitude < 3.4028235677973366e38;
    }
    if (to == prelude_type(builtins::k_short))
        return n.is_integer && n.integer >= -32768 && n.integer <= 32767;
    if (to == prelude_type(builtins::k_ushort))
        return n.is_integer && n.integer >= 0 && n.integer <= 65535;
    if (to == prelude_type(builtins::k_half))
    {
        // as for float: an integer exactly, and a float below the largest half plus half an ulp, 65504 + 16
        if (n.is_integer)
            return n.integer >= -65504 && n.integer <= 65504 && is_exact_half(n.integer);
        auto const magnitude = n.real < 0 ? -n.real : n.real;
        return magnitude < 65520.0;
    }
    // CHK-450: a literal is held in 64 bits, so a long holds every integer one and a ulong every one not negative
    if (to == prelude_type(builtins::k_long))
        return n.is_integer;
    if (to == prelude_type(builtins::k_ulong))
        return n.is_integer && n.integer >= 0;
    if (to == prelude_type(builtins::k_double))
    {
        // an integer exactly, below 2^63, where the round trip itself would overflow; a float literal is a double
        if (n.is_integer)
        {
            auto const d = f64(n.integer);
            return d < 9223372036854775808.0 && i64(d) == n.integer;
        }
        return n.real - n.real == 0.0;
    }
    return false;
}

void checker::commit_literals(function_scope& scope,
                              call_arguments const& arguments,
                              cc::span<parameter const> parameters,
                              cc::span<i32 const> slots)
{
    auto bindings = cc::vector<type_id>();
    commit_literals(scope, arguments, parameters, slots, bindings);
}

void checker::commit_literals(function_scope& scope,
                              call_arguments const& arguments,
                              cc::span<parameter const> parameters,
                              cc::span<i32 const> slots,
                              cc::vector<type_id>& bindings)
{
    // by value: resolving a literal may compile another function, and `parameters` then moves
    auto copied = cc::vector<parameter>::create_copy_of(parameters);
    // a lambda binds what the others left, so it goes last, and every other argument meets its parameter bound
    for (auto pass = 0; pass < 2; ++pass)
        for (auto p = isize(0); p < copied.size(); ++p)
        {
            auto const i = slots[p];
            if (i < 0)
                continue;
            auto const is_lambda = arguments.functions[i] >= 0 && function_arguments[arguments.functions[i]].is_lambda;
            if (is_lambda != (pass == 1))
                continue;
            copied[p].type = substitute(copied[p].type, bindings);
            if (arguments.undefineds[i])
            {
                set_type(scope.file, arguments.written[i].expr, copied[p].type);
                continue;
            }
            if (!arguments.leading_dots[i].empty())
            {
                auto const type = copied[p].type;
                set_type(scope.file, arguments.written[i].expr, type);
                set_target(scope.file, arguments.written[i].expr,
                           {.kind = target_kind::enum_case,
                            .symbol = out.at(type).symbol,
                            .index = case_index_of(type, arguments.leading_dots[i])});
                continue;
            }
            if (arguments.numbers[i].is_number)
                set_type(scope.file, arguments.written[i].expr, copied[p].type);
            else if (arguments.functions[i] >= 0)
            {
                // by value: checking a lambda may push to `function_arguments`
                auto const f = function_arguments[arguments.functions[i]];
                auto const where = span_of(scope.file, f.expr);
                if (f.is_lambda)
                    (void)check_lambda(scope, f.expr, copied[p].type, &bindings);
                else if (auto const chosen = function_of_type(f.functions, copied[p].type); is_valid(chosen))
                {
                    set_target(scope.file, f.expr, {.kind = target_kind::overload, .symbol = chosen});
                    set_type(scope.file, f.expr, copied[p].type);
                    // the function is called wherever the parameter is, and that is inlined into this caller
                    note_program_call(scope, chosen, where);
                }
            }
            else if (arguments.literals[i] >= 0)
                (void)resolve_literal(scope, arguments.written[i].expr, copied[p].type, arguments.literals[i]);
        }
}

type_id checker::resolve_literal(function_scope& scope, ast::expr_id expr, type_id to, i32 literal)
{
    auto const file = scope.file;
    auto const where = span_of(file, expr);
    if (to == error_type || literals[literal].is_poisoned)
        return error_type;
    auto const candidates = functions_named_after(file, to);
    if (candidates.empty())
    {
        report(diagnostic_kind::type_mismatch, file, where,
               out.at(to).kind == type_kind::structure
                   ? cc::format("{} is opaque, so no literal converts to it", out.name_of(to))
                   : cc::format("a literal converts to a struct, and {} is none", out.name_of(to)));
        return error_type;
    }
    // by value: `literals` may grow while the call resolves
    auto const arguments = literals[literal];
    auto const result
        = resolve_overload(scope, expr, ast::expr_id::none, candidates, arguments, out.at(out.at(to).symbol).name);
    set_type(file, expr, to);
    // CHK-85: a function of the type's name that gives another type is called by name, and converts nothing
    if (result != error_type && result != to)
        report(diagnostic_kind::literal_conversion_result, file, where,
               cc::format("this literal converts by {}, which returns {} and not {}",
                          out.at(tables_of(file).target_at(expr).symbol).name, out.name_of(result), out.name_of(to)));
    return result == error_type ? error_type : to;
}

type_id checker::check_expected(function_scope& scope, ast::expr_id expr, type_id to, cc::string_view what)
{
    auto const file = scope.file;
    if (!ast::is_valid(expr))
        return error_type;
    auto const& node = ast_of(file).at(expr).node;
    // CHK-82: a tuple or an object literal converts to the type expected where it stands
    if (node.is<ast::tuple>() || node.is<ast::object>())
    {
        auto const literal = shape_literal(scope, expr);
        return resolve_literal(scope, expr, to, literal);
    }
    if (node.is<ast::array>() && to != error_type && out.at(to).kind == type_kind::array)
        return check_array_literal(scope, expr, to);
    auto const outer_expected = expected_result;
    expected_result = to;
    // CHK-166: an `if` or a `case` value is the type expected of it, which each branch's value converts to
    if ((node.is<ast::if_expr>() || node.is<ast::case_expr>()) && to != error_type)
        expected_value = to;
    auto const type = check_expr(scope, expr);
    expected_result = outer_expected;
    if (type == error_type || to == error_type)
        return type;
    // CHK-253: a number literal where a number type is expected converts to it where it holds exactly; anything else,
    // a vector included, is no number and the literal is simply of another type
    auto const number = number_of(file, expr);
    cc::string_view const number_types[]
        = {builtins::k_int,  builtins::k_uint, builtins::k_float, builtins::k_short, builtins::k_ushort,
           builtins::k_half, builtins::k_long, builtins::k_ulong, builtins::k_double};
    auto is_number_type = false;
    for (auto const name : number_types)
        is_number_type = is_number_type || to == prelude_type(name);
    if (number.is_number && type != to && is_number_type)
    {
        if (!holds(number, to))
        {
            report(diagnostic_kind::literal_not_representable, file, span_of(file, expr),
                   cc::format("{} does not hold {} exactly", out.name_of(to), text_of(file, span_of(file, expr))));
            return error_type;
        }
        set_type(file, expr, to);
        return to;
    }
    if (type != to)
        tell_apart(report(diagnostic_kind::type_mismatch, file, span_of(file, expr),
                          what.empty() ? cc::format("expected {}, got {}", out.name_of(to), out.name_of(type))
                                       : cc::format("{} is {}, got {}", what, out.name_of(to), out.name_of(type))),
                   to, type);
    return type;
}

bound_arguments checker::bind_arguments(cc::span<parameter const> parameters, call_arguments const& arguments) const
{
    auto result = bound_arguments{.slots = cc::vector<i32>::create_filled(parameters.size(), -1)};
    auto const fail = [&](miss_reason why, i32 argument, i32 parameter)
    {
        result.failure = why;
        result.argument = argument;
        result.parameter = parameter;
        return result;
    };

    // CHK-251: a positional argument after a named one binds only where every argument before it is in its own slot.
    auto is_in_own_slot = true;
    auto seen_named = false;
    for (auto i = i32(0); i < i32(arguments.written.size()); ++i)
    {
        auto const name = arguments.names[i];
        auto p = i32(-1);
        if (name.empty())
        {
            if (seen_named && !is_in_own_slot)
                return fail(miss_reason::positional_out_of_slot, i, -1);
            if (i >= i32(parameters.size()))
                return fail(miss_reason::too_many, i, -1);
            if (parameters[i].is_named_only)
                return fail(miss_reason::positional_to_named_only, i, i);
            p = i;
        }
        else
        {
            seen_named = true;
            // the receiver is filled by its position, never by the name `self`
            for (auto k = i32(0); k < i32(parameters.size()); ++k)
                if (parameters[k].name == name && name != "self")
                    p = k;
            if (p < 0)
                return fail(miss_reason::no_such_parameter, i, -1);
        }
        is_in_own_slot = is_in_own_slot && p == i;
        if (result.slots[p] >= 0)
            return fail(miss_reason::filled_twice, i, p);
        result.slots[p] = i;
    }
    for (auto p = i32(0); p < i32(parameters.size()); ++p)
        if (result.slots[p] < 0 && !parameters[p].has_default)
            return fail(miss_reason::missing_argument, -1, p);
    return result;
}

void checker::judge_wide_literals(isize first)
{
    for (auto i = first; i < wide_literals.size(); ++i)
    {
        auto const w = wide_literals[i];
        auto const type = table_view{.module = &out, .file = w.file, .instance = w.instance}.type_at(w.expr);
        if (!is_valid(type) || type == error_type || holds(number_of(w.file, w.expr), type))
            continue;
        auto const where = span_of(w.file, w.expr);
        report(diagnostic_kind::literal_not_representable, w.file, where,
               cc::format("{} does not hold {}", out.name_of(type), text_of(w.file, where)));
        if (is_valid(w.function) && out.at(w.function).info >= 0)
            notes[out.at(w.function).info].is_body_sound = false;
    }
}

type_id checker::deduce_result(function_scope const& scope, ast::expr_id id, symbol_id callee, cc::vector<type_id>& bindings)
{
    auto const info = out.functions[out.at(callee).info];
    // what its signature names, which is all a synthesized constructor has, and every parameter the function declares,
    // a value parameter that no type names among them
    auto needed = cc::vector<type_id>();
    for (auto const& p : out.at(info.parameters))
        collect_type_parameters(p.type, needed);
    collect_type_parameters(info.result, needed);
    for (auto const q : out.at(info.type_parameters))
        if (!needed.values().any([&](type_id n) { return n == q; }))
            needed.push_back(q);
    if (needed.empty())
        return info.result;
    if (is_valid(expected_result) && expected_result != error_type)
    {
        (void)unify(info.result, expected_result, bindings);
        // a generic body expecting its own type parameter binds it to itself
        auto expected = cc::vector<type_id>();
        collect_type_parameters(expected_result, expected);
        for (auto const q : needed)
        {
            auto is_bound = false;
            for (auto i = isize(0); i + 1 < bindings.size(); i += 2)
                is_bound = is_bound || bindings[i] == q;
            if (!is_bound && expected.values().any([&](type_id e) { return e == q; }))
            {
                bindings.push_back(q);
                bindings.push_back(q);
            }
        }
    }
    for (auto const q : needed)
    {
        auto is_bound = false;
        for (auto i = isize(0); i + 1 < bindings.size(); i += 2)
            is_bound = is_bound || bindings[i] == q;
        // CHK-338: a default binds what nothing else said, as the parameters before it make it
        if (!is_bound && is_valid(out.at(q).generic))
        {
            auto const fallback = substitute(out.at(q).generic, bindings);
            bindings.push_back(q);
            bindings.push_back(fallback);
            is_bound = !is_open(fallback);
        }
        if (!is_bound)
        {
            report(diagnostic_kind::no_matching_overload, scope.file, span_of(scope.file, id),
                   cc::format("{} of {} is said by nothing: neither an argument nor where the call stands",
                              out.name_of(q), out.at(callee).name));
            return error_type;
        }
    }
    // CHK-338: the callee's body is checked with what this call binds, and an inferred result is that instance's
    auto const result = substitute(info.result, bindings);
    auto const instance = instantiate(callee, bindings, scope.file, span_of(scope.file, id));
    if (instance >= 0 && notes[out.at(callee).info].infers_result)
        return out.function_instances[instance].result;
    return result;
}

void checker::record_call(i32 file,
                          ast::expr_id id,
                          symbol_id callee,
                          call_arguments const& arguments,
                          cc::span<i32 const> slots,
                          cc::span<type_id const> bindings)
{
    auto const written = ast::range_of<written_argument>{.first = u32(out.written_arguments.size()),
                                                         .count = u32(arguments.written.size())};
    out.written_arguments.push_back_range(arguments.written);
    // CHK-416: a receiver bound to a `mut` parameter is handed over as `mut x` is, which is what every later pass reads
    auto const has_receiver = !arguments.written.empty() && arguments.written[0].may_be_mut;
    if (has_receiver && is_valid(callee) && out.at(callee).kind == symbol_kind::function)
    {
        auto const parameters = out.at(out.functions[out.at(callee).info].parameters);
        for (auto p = isize(0); p < slots.size() && p < parameters.size(); ++p)
            if (auto const i = slots[p]; i >= 0 && parameters[p].is_mut && arguments.written[i].may_be_mut)
                out.written_arguments.own(written.first + i).is_mut = true;
    }
    auto const slot_range = ast::range_of<i32>{.first = u32(out.call_slots.size()), .count = u32(slots.size())};
    out.call_slots.push_back_range(slots);
    if (current_instance >= 0 && file == out.at(out.function_instances[current_instance].function).file)
        out.function_instances.own(current_instance).tables.call_of[own_slot(id)] = i32(out.call_records.size());
    else
        out.files.own(file).call_of[ast::index_of(id)] = i32(out.call_records.size());
    out.call_records.push_back({.callee = callee,
                                .written = written,
                                .slots = slot_range,
                                .type_arguments = {.first = u32(out.type_lists.size()), .count = u32(bindings.size())}});
    out.type_lists.push_back_range(bindings);
}

void checker::note_program_call(function_scope const& scope, symbol_id callee, source_span where)
{
    auto const file = scope.file;
    // A call of a function of the program is inlined, so it is an edge recursion is looked for along.
    calls.push_back({.caller = scope.function, .callee = callee, .file = file, .where = where});

    // Bindings are an effect: what the callee reads, the caller has to list, and so on up to the entry point.
    auto const listed = out.at(out.functions[out.at(scope.function).info].bindings);
    for (auto const needed : out.at(out.functions[out.at(callee).info].bindings))
    {
        auto is_listed = false;
        for (auto const l : listed)
            is_listed = is_listed || l == needed;
        is_listed = is_listed || (scope.is_test && is_workgroup_binding(needed));
        if (!is_listed && scope.is_test)
        {
            // CHK-228: a test gives a callee its bindings by listing them, and its driver gives them values (CHK-333)
            auto& d = report(diagnostic_kind::binding_not_listed, file, where,
                             cc::format("{} needs {}, and the test does not list it", name_seen_from(file, callee),
                                        name_seen_from(file, needed)));
            d.notes.push_back({.file = file,
                               .where = where,
                               .message = cc::format("`test {{{}}}:` lists it, and the driver that runs the test "
                                                     "gives its values",
                                                     name_seen_from(file, needed))});
        }
        else if (!is_listed)
            report(diagnostic_kind::binding_not_listed, file, where,
                   cc::format("{} needs {}, which is not in the binding list of {}", name_seen_from(file, callee),
                              name_seen_from(file, needed), out.at(scope.function).name));
    }
}

type_id checker::check_logical(function_scope& scope, ast::expr_id id, ast::call const& call)
{
    auto const file = scope.file;
    auto const where = span_of(file, id);
    auto const spelling = text_of(file, file_of(file).at(call.op).where);
    auto const arguments = check_arguments(scope, call.arguments, false);
    if (arguments.is_poisoned)
        return error_type;

    auto const bool_type = type_of_builtin(builtins::k_bool, file, where);
    auto is_match = arguments.types.size() == (spelling == "not" ? 1 : 2);
    for (auto const type : arguments.types)
        is_match = is_match && type == bool_type;
    if (!is_match && bool_type != error_type)
        report(diagnostic_kind::no_matching_overload, file, where,
               signature_text(cc::format("operator {}", spelling), arguments.types));
    return bool_type;
}

type_id checker::check_chain(function_scope& scope, ast::expr_id id, ast::comparison_chain const& chain)
{
    auto const file = scope.file;
    auto const& ast = ast_of(file);
    auto const where = span_of(file, id);
    auto const operands = ast.at(chain.operands);
    auto const operators = ast.at(chain.operators);

    auto types = cc::vector<type_id>();
    for (auto const operand : operands)
        types.push_back(check_expr(scope, operand));

    auto const bool_type = type_of_builtin(builtins::k_bool, file, where);
    for (auto i = isize(0); i < operators.size() && i + 1 < types.size(); ++i)
    {
        if (types[i] == error_type || types[i + 1] == error_type)
            continue;
        type_id const pair[] = {types[i], types[i + 1]};
        auto const spelling = text_of(file, file_of(file).at(operators[i]).where);
        // An operand between two links has the type its first link gave it, so only the new one converts.
        ast::expr_id const both[] = {i == 0 ? operands[0] : ast::expr_id::none, operands[i + 1]};
        auto const chosen = resolve_operator(file, where, spelling, pair, both);
        if (is_valid(chosen) && ast::is_valid(operands[i + 1]))
            types[i + 1] = tables_of(file).type_at(operands[i + 1]);
        if (is_valid(chosen) && out.functions[out.at(chosen).info].result != bool_type && bool_type != error_type)
            report(diagnostic_kind::type_mismatch, file, where,
                   cc::format("a comparison in a chain is a bool, and operator {} gives {}", spelling,
                              out.name_of(out.functions[out.at(chosen).info].result)));
    }
    return bool_type;
}

bool checker::is_out_of_the_running(i32 file, symbol_id candidate, call_arguments const& arguments)
{
    auto const& s = out.at(candidate);
    if (s.kind != symbol_kind::function || s.state != symbol_state::in_compilation || s.info < 0)
        return false;
    return !match(file, candidate, arguments).has_value();
}

symbol_id checker::find_operator(i32 file, cc::string_view spelling, cc::span<type_id const> types) const
{
    auto result = symbol_id::none;
    for (auto const candidate : operators_of(file, spelling))
    {
        if (out.at(candidate).state != symbol_state::checked)
            continue;
        auto const parameters = out.at(out.functions[out.at(candidate).info].parameters);
        auto is_match = parameters.size() == types.size();
        for (auto i = isize(0); is_match && i < parameters.size(); ++i)
            is_match = parameters[i].type == types[i];
        if (is_match && is_valid(result))
            return symbol_id::none;
        if (is_match)
            result = candidate;
    }
    return result;
}

symbol_id checker::resolve_operator(i32 file,
                                    source_span where,
                                    cc::string_view spelling,
                                    cc::span<type_id const> types,
                                    cc::span<ast::expr_id const> operands)
{
    auto arguments = call_arguments();
    for (auto i = isize(0); i < types.size(); ++i)
    {
        auto const expr = i < operands.size() ? operands[i] : ast::expr_id::none;
        add_argument(arguments, {.expr = expr}, types[i], {}, number_of(file, expr));
    }

    auto is_silent = false;
    auto matches = cc::vector<candidate_match>();
    for (auto const candidate : operators_of(file, spelling))
    {
        if (is_out_of_the_running(file, candidate, arguments))
            continue;
        if (demand(candidate, file, where) != symbol_state::checked)
        {
            is_silent = true;
            continue;
        }
        if (auto m = match(file, candidate, arguments); m.has_value())
            matches.push_back(cc::move(m.value()));
    }

    auto const text = call_text(file, cc::format("operator {}", spelling), arguments);
    if (matches.empty())
    {
        if (!is_silent)
            report(diagnostic_kind::no_matching_overload, file, where, text);
        return symbol_id::none;
    }
    auto const best = best_of(cc::move(matches));
    if (best.size() > 1)
    {
        report(diagnostic_kind::ambiguous_overload, file, where, cc::format("{} has {} candidates", text, best.size()));
        return symbol_id::none;
    }
    auto const chosen = best.front().candidate;
    if (!is_valid(out.at(chosen).intrinsic))
    {
        unsupported(file, where, "an operator of the program's own where no call is written");
        return symbol_id::none;
    }
    // a number literal operand takes the type of the parameter it fills, which is what the flat tree writes
    auto const parameters = out.at(out.functions[out.at(chosen).info].parameters);
    for (auto i = isize(0); i < parameters.size() && i < operands.size(); ++i)
        if (arguments.numbers[i].is_number)
            set_type(file, operands[i], parameters[i].type);
    return chosen;
}
