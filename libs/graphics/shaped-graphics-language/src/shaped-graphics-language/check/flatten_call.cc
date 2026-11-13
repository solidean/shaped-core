#include <shaped-graphics-language/check/impl/flattener.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
/// `body`, a one-line string's piece, with its escapes resolved (STR-3, STR-4, STR-44) and `$$` a dollar (STR-27).
/// The tokenizer has reported every malformed escape, which stands for the character after its backslash.
void append_unescaped(cc::string& out, cc::string_view body)
{
    for (auto i = isize(0); i < body.size(); ++i)
    {
        auto const c = body[i];
        if (c == '$' && i + 1 < body.size() && body[i + 1] == '$')
        {
            out += '$';
            ++i;
            continue;
        }
        if (c != '\\' || i + 1 >= body.size())
        {
            out += c;
            continue;
        }
        auto const e = body[++i];
        if (e == 'n')
            out += '\n';
        else if (e == 'r')
            out += '\r';
        else if (e == 't')
            out += '\t';
        else if (e == '0')
            out += '\0';
        else if (e == 'u' && i + 1 < body.size() && body[i + 1] == '{')
        {
            auto code = u32(0);
            auto j = i + 2;
            for (; j < body.size() && body[j] != '}'; ++j)
            {
                auto const h = body[j];
                code = code * 16 + u32(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
            }
            i = j;
            // as UTF-8
            if (code < 0x80)
                out += char(code);
            else if (code < 0x800)
            {
                out += char(0xC0 | (code >> 6));
                out += char(0x80 | (code & 0x3F));
            }
            else if (code < 0x10000)
            {
                out += char(0xE0 | (code >> 12));
                out += char(0x80 | ((code >> 6) & 0x3F));
                out += char(0x80 | (code & 0x3F));
            }
            else
            {
                out += char(0xF0 | (code >> 18));
                out += char(0x80 | ((code >> 12) & 0x3F));
                out += char(0x80 | ((code >> 6) & 0x3F));
                out += char(0x80 | (code & 0x3F));
            }
        }
        else
            out += e;
    }
}
} // namespace

void sgl::check::impl::flattener::flatten_check(origin from, ast::expr_id condition, bool stops, ast::expr_id message)
{
    auto const site = i32(entry.check_sites.size());
    entry.check_sites.push_back({.from = from, .stops = stops});
    auto const outer = cc::move(block);
    block = {};
    auto nodes = cc::vector<flat_check_node>();
    (void)materialize(condition, -1, nodes);
    // a message's values are the check's body too, computed beside the condition and read where it fails
    if (ast::is_valid(message))
    {
        entry.check_sites[site].message = add_message(message);
        entry.check_sites[site].message_values = flatten_message_values(message);
    }
    auto const body = add_list(block);
    block = cc::move(outer);

    // CHK-227: a target writes no assert, so what its condition would write happens on the interpreter alone
    if (stops && writes_outside(entry, body, 0))
    {
        effectful_asserts.push_back({.file = file(), .expr = condition});
        meets_error = true;
        mark_failed();
    }

    auto loop_variables = cc::vector<flat_expr_id>();
    for (auto const index : test_loops)
        loop_variables.push_back(add_expr(entry.at(index).type, ast::expr_id::none, flat_local_ref{.local = index}));
    entry.check_sites[site].nodes = {.first = u32(entry.check_nodes.size()), .count = u32(nodes.size())};
    entry.check_nodes.push_back_range(nodes);
    entry.check_sites[site].loop_variables = add_list(loop_variables);
    add_stmt(from, flat_check{.site = site, .body = body});
}

auto sgl::check::impl::flattener::add_message(ast::expr_id message) -> i32
{
    auto const& source = c.file_of(file());
    auto result = flat_message{.pieces = {cc::string()}};
    auto const& tokens = source.tokens;
    auto const open = index_of(source.at(ast().at(message).form).token);

    // The tokens from `i` up to `end`, or to the closing quote: a body's text, and a new piece after each interpolation.
    // A multi-line string's body has no escapes (STR-21), and `$$` is a dollar in either form (STR-27).
    auto const take = [&](isize i, isize end, bool is_multi_line)
    {
        for (; i < end && tokens[i].kind != token_kind::quote_close; ++i)
        {
            auto const& t = tokens[i];
            if (t.kind == token_kind::string_body)
            {
                auto const body = source.text_of(t.where);
                if (!is_multi_line)
                    append_unescaped(result.pieces.back(), body);
                else
                    for (auto k = isize(0); k < body.size(); ++k)
                    {
                        result.pieces.back() += body[k];
                        if (body[k] == '$' && k + 1 < body.size() && body[k + 1] == '$')
                            ++k;
                    }
            }
            else if (t.kind == token_kind::dollar && i + 1 < tokens.size())
            {
                // past the interpolation: its parenthesized code, or its name and each `.name` fused after it
                if (tokens[i + 1].kind == token_kind::round_open)
                {
                    auto depth = 0;
                    for (++i; i < tokens.size(); ++i)
                    {
                        depth += tokens[i].kind == token_kind::round_open  ? 1
                               : tokens[i].kind == token_kind::round_close ? -1
                                                                           : 0;
                        if (depth == 0)
                            break;
                    }
                }
                else if (tokens[i + 1].kind == token_kind::symbol)
                {
                    ++i;
                    while (i + 2 < tokens.size() && tokens[i + 1].kind == token_kind::dot
                           && tokens[i + 2].kind == token_kind::symbol)
                        i += 2;
                }
                else
                    continue;
                result.pieces.push_back(cc::string());
            }
        }
    };

    // the line that holds the opening quote; lines and their tokens are in source order
    auto const& lines = source.lines;
    auto opening = isize(-1);
    for (auto l = isize(0); l < lines.size() && opening < 0; ++l)
        if (lines[l].token_count > 0 && lines[l].first_token <= u32(open)
            && u32(open) < lines[l].first_token + lines[l].token_count)
            opening = l;
    if (opening < 0 || !lines[opening].opens_string)
        take(open + 1, tokens.size(), false);
    else
    {
        // STR-15 to STR-20: the content lines joined by `\n`, each past the opening line's indentation and 4 columns
        auto const content_columns = lines[opening].indent_columns + 4;
        auto is_first = true;
        auto blanks = 0;
        for (auto l = opening + 1; l < lines.size(); ++l)
        {
            auto const& line = lines[l];
            if (line.kind == line_kind::blank)
            {
                ++blanks;
                continue;
            }
            if (line.kind != line_kind::string_content)
                break;
            if (!is_first)
                for (auto k = 0; k <= blanks; ++k)
                    result.pieces.back() += '\n';
            is_first = false;
            blanks = 0;
            for (auto k = content_columns; k < line.indent_columns; ++k)
                result.pieces.back() += ' ';
            take(isize(line.first_token), isize(line.first_token + line.token_count), true);
        }
    }
    entry.messages.push_back(cc::move(result));
    return i32(entry.messages.size() - 1);
}

auto sgl::check::impl::flattener::flatten_message_values(ast::expr_id message) -> ast::range_of<flat_expr_id>
{
    auto values = cc::vector<flat_expr_id>();
    if (auto const* const literal = ast().at(message).node.try_as<ast::literal>())
        for (auto const part : ast().at(literal->interpolations))
            values.push_back(flatten_expr(part));
    return add_list(values);
}

auto sgl::check::impl::flattener::check_var(ast::expr_id id) -> local_id
{
    auto const local = add_local(local_kind::var, "check", tables().type_at(id));
    add_stmt({.file = file(), .expr = id}, flat_var{.local = local});
    return local;
}

void sgl::check::impl::flattener::assign_check(local_id local, ast::expr_id from, flat_expr_id value)
{
    add_stmt({.file = file(), .expr = from}, flat_assign{.place = local_ref(local, from), .value = value});
}

auto sgl::check::impl::flattener::check_leaf(ast::expr_id id, i32 parent, cc::vector<flat_check_node>& nodes) -> local_id
{
    auto const local = check_var(id);
    nodes.push_back(
        {.kind = check_node_kind::leaf, .from = {.file = file(), .expr = id}, .parent = parent, .value = local});
    assign_check(local, id, flatten_expr(id));
    return local;
}

auto sgl::check::impl::flattener::compare_call(ast::expr_id id, symbol_id callee, local_id lhs, local_id rhs)
    -> flat_expr_id
{
    flat_expr_id const arguments[] = {local_ref(lhs, id), local_ref(rhs, id)};
    if (!is_valid(c.out.at(callee).intrinsic))
    {
        auto const inlined = inline_call(id, callee, arguments);
        return add_expr(c.out.functions[c.out.at(callee).info].result, id,
                        flat_block{.label = inlined.label, .body = inlined.body});
    }
    return builtin_call(id, callee, arguments);
}

auto sgl::check::impl::flattener::is_comparison(cc::string_view op) -> bool
{
    return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
}

auto sgl::check::impl::flattener::materialize(ast::expr_id id, i32 parent, cc::vector<flat_check_node>& nodes) -> local_id
{
    auto const& e = ast().at(id);
    auto const* const call = e.node.try_as<ast::call>();
    auto const spelling
        = call != nullptr && sgl::is_valid(call->op) ? c.text_of(file(), c.file_of(file()).at(call->op).where) : "";
    auto const arguments = call != nullptr ? ast().at(call->arguments) : cc::span<ast::argument const>();
    auto const where = tables().target_at(id);
    auto const from = origin{.file = file(), .expr = id};
    auto const index = i32(nodes.size());

    if (spelling == "not" && arguments.size() == 1)
    {
        auto const local = check_var(id);
        nodes.push_back({.kind = check_node_kind::not_, .from = from, .parent = parent, .value = local});
        auto const operand = materialize(arguments[0].value, index, nodes);
        assign_check(local, id, add_expr(tables().type_at(id), id, flat_not{.operand = local_ref(operand, id)}));
        return local;
    }
    if (call != nullptr && call->is_short_circuit && arguments.size() == 2)
    {
        // `a and b`: b's statements run only where a is true, so a skipped operand's `var` holds nothing.
        auto const is_and = spelling == "and";
        auto const local = check_var(id);
        nodes.push_back({.kind = is_and ? check_node_kind::and_ : check_node_kind::or_,
                         .from = from,
                         .parent = parent,
                         .value = local});
        auto const lhs = materialize(arguments[0].value, index, nodes);
        assign_check(local, id, local_ref(lhs, id));
        auto const outer = cc::move(block);
        block = {};
        auto const rhs = materialize(arguments[1].value, index, nodes);
        assign_check(local, id, local_ref(rhs, id));
        auto const then_body = add_list(block);
        block = cc::move(outer);
        auto const decided
            = is_and ? local_ref(lhs, id) : add_expr(tables().type_at(id), id, flat_not{.operand = local_ref(lhs, id)});
        add_stmt(from, flat_if{.condition = decided, .then_body = then_body});
        return local;
    }
    if (is_comparison(spelling) && arguments.size() == 2 && where.kind == target_kind::overload)
    {
        auto const local = check_var(id);
        nodes.push_back(
            {.kind = check_node_kind::compare, .from = from, .op = cc::string(spelling), .parent = parent, .value = local});
        auto const lhs = check_leaf(arguments[0].value, index, nodes);
        nodes[index].lhs = i32(nodes.size() - 1);
        auto const rhs = check_leaf(arguments[1].value, index, nodes);
        nodes[index].rhs = i32(nodes.size() - 1);
        assign_check(local, id, compare_call(id, where.symbol, lhs, rhs));
        return local;
    }
    if (auto const* const chain = e.node.try_as<ast::comparison_chain>())
        return materialize_chain(id, *chain, parent, nodes);
    return check_leaf(id, parent, nodes);
}

auto sgl::check::impl::flattener::materialize_chain(ast::expr_id id,
                                                    ast::comparison_chain const& chain,
                                                    i32 parent,
                                                    cc::vector<flat_check_node>& nodes) -> local_id
{
    auto const operands = ast().at(chain.operands);
    auto const operators = ast().at(chain.operators);
    auto const local = check_var(id);
    auto const index = i32(nodes.size());
    nodes.push_back(
        {.kind = check_node_kind::chain, .from = {.file = file(), .expr = id}, .parent = parent, .value = local});
    if (operands.size() < 2 || operators.size() + 1 != operands.size())
    {
        mark_failed();
        return local;
    }

    auto left = check_leaf(operands[0], index, nodes);
    auto left_node = i32(nodes.size() - 1);
    auto outers = cc::vector<cc::vector<flat_stmt_id>>();
    auto conditions = cc::vector<flat_expr_id>();
    for (auto i = isize(0); i < operators.size(); ++i)
    {
        auto const link = check_var(id);
        auto const link_node = i32(nodes.size());
        auto const spelling = c.text_of(file(), c.file_of(file()).at(operators[i]).where);
        nodes.push_back({.kind = check_node_kind::compare,
                         .from = {.file = file(), .expr = id},
                         .op = cc::string(spelling),
                         .parent = index,
                         .lhs = left_node,
                         .value = link});
        auto const right = check_leaf(operands[i + 1], index, nodes);
        nodes[link_node].rhs = i32(nodes.size() - 1);

        type_id const types[] = {entry.at(left).type, entry.at(right).type};
        auto const callee = c.find_operator(file(), spelling, types);
        if (!is_valid(callee))
        {
            mark_failed();
            return local;
        }
        assign_check(link, id, compare_call(id, callee, left, right));
        assign_check(local, id, local_ref(link, id));

        // the next link runs inside an `if` of this one
        if (i + 1 < operators.size())
        {
            conditions.push_back(local_ref(link, id));
            outers.push_back(cc::move(block));
            block = {};
        }
        left = right;
        left_node = nodes[link_node].rhs;
    }
    for (auto i = outers.size() - 1; i >= 0; --i)
    {
        auto const then_body = add_list(block);
        block = cc::move(outers[i]);
        add_stmt({.file = file(), .expr = id}, flat_if{.condition = conditions[i], .then_body = then_body});
    }
    return local;
}

auto sgl::check::impl::flattener::int_type() const -> type_id
{
    auto const* const found = c.prelude_names.get_ptr(builtins::k_int);
    if (found == nullptr || found->empty() || c.out.at(found->front()).kind != symbol_kind::structure)
        return type_id::none;
    return c.out.at(found->front()).type;
}

void sgl::check::impl::flattener::collect_patterns(ast::expr_id id, cc::vector<flat_expr_id>& into)
{
    if (!ast::is_valid(id))
        return;
    auto const& e = ast().at(id);
    if (auto const* const call = e.node.try_as<ast::call>();
        call != nullptr && call->is_short_circuit && sgl::is_valid(call->op)
        && c.text_of(file(), c.file_of(file()).at(call->op).where) == "or")
    {
        for (auto const& argument : ast().at(call->arguments))
            collect_patterns(argument.value, into);
        return;
    }
    into.push_back(flatten_expr(id));
}

auto sgl::check::impl::flattener::flatten_arm_body(ast::body const& result, label_id value_block)
    -> ast::range_of<flat_stmt_id>
{
    auto const outer = cc::move(block);
    block = {};
    if (is_valid(value_block))
        current()->value_blocks.push_back(value_block);

    if (result.kind == ast::body_kind::arrow && ast::is_valid(result.value))
    {
        auto const where = origin{.file = file(), .expr = result.value};
        auto const& e = ast().at(result.value);
        auto const is_jump = e.node.is<ast::return_expr>() || e.node.is<ast::break_expr>()
                          || e.node.is<ast::continue_expr>() || e.node.is<ast::yield_expr>()
                          || e.node.is<ast::discard_expr>();
        if (is_jump)
            flatten_expr_stmt(where, result.value);
        else if (is_valid(value_block))
        {
            auto const value = flatten_expr(result.value);
            add_stmt(where, flat_leave{.target = value_block, .value = value});
        }
        else
            flatten_expr_stmt(where, result.value);
    }
    else
        for (auto const stmt : ast().at(result.statements))
            flatten_stmt(stmt);

    if (is_valid(value_block))
        current()->value_blocks.remove_back();
    auto const range = add_list(block);
    block = cc::move(outer);
    return range;
}

auto sgl::check::impl::flattener::flatten_value_case(ast::expr_id id, type_id type, ast::case_expr const& node)
    -> flat_expr_id
{
    auto const value_block = add_label("case");
    auto const parts = flatten_case_parts(node, value_block);
    auto const where = origin{.file = file(), .expr = id};
    flat_stmt_id const statements[] = {make_stmt(where, parts)};
    return add_expr(type, id, flat_block{.label = value_block, .body = add_list(statements)});
}

auto sgl::check::impl::flattener::flatten_value_if(ast::expr_id id, type_id type, ast::if_expr const& node) -> flat_expr_id
{
    auto branches = ast().at(node.branches);
    auto condition = flat_expr_id::none;
    while (branches.size() >= 2 && ast::is_valid(branches.front().condition))
    {
        auto const first_expr = entry.exprs.size();
        condition = flatten_expr(branches.front().condition);
        auto const taken = constant_condition(condition);
        if (!taken.has_value())
            break;
        auto const decided = condition;
        condition = flat_expr_id::none;
        auto const rest = branches.subspan({.offset = 1, .size = branches.size() - 1});
        if (taken.value())
        {
            // EMIT-264: what the branches after it consult is a variant, where a grant can make it false
            auto const& first = branches.front();
            auto const after = ast::index_of(first.condition) * 2 + 1;
            if (could_take(decided, first_expr, false))
            {
                if (rest.size() == 1)
                    probe_consults(after, [&](flattener& p) { (void)p.taken_value(id, type, rest.front().then); });
                else
                    probe_consults(after,
                                   [&](flattener& p)
                                   {
                                       auto const label = p.add_label("if");
                                       p.flatten_if_branches({.file = p.file(), .expr = id}, rest, label,
                                                             flat_expr_id::none);
                                   });
            }
            return granting(first.condition, [&] { return taken_value(id, type, first.then); });
        }
        if (could_take(decided, first_expr, true))
            probe_consults(ast::index_of(branches.front().condition) * 2,
                           [&](flattener& p)
                           {
                               (void)p.granting(branches.front().condition,
                                                [&] { return p.taken_value(id, type, branches.front().then); });
                           });
        branches = rest;
        if (branches.size() == 1)
            return taken_value(id, type, branches.front().then);
    }
    auto const value_block = add_label("if");
    auto const outer = cc::move(block);
    block = {};
    flatten_if_branches({.file = file(), .expr = id}, branches, value_block, condition);
    auto const body = add_list(block);
    block = cc::move(outer);
    return add_expr(type, id, flat_block{.label = value_block, .body = body});
}

auto sgl::check::impl::flattener::taken_value(ast::expr_id id, type_id type, ast::body const& then) -> flat_expr_id
{
    if (then.kind == ast::body_kind::arrow && ast::is_valid(then.value))
    {
        auto const& e = ast().at(then.value);
        auto const is_jump = e.node.is<ast::return_expr>() || e.node.is<ast::break_expr>()
                          || e.node.is<ast::continue_expr>() || e.node.is<ast::yield_expr>()
                          || e.node.is<ast::discard_expr>();
        if (!is_jump)
            return flatten_expr(then.value);
    }
    auto const value_block = add_label("if");
    auto const body = flatten_arm_body(then, value_block);
    return add_expr(type, id, flat_block{.label = value_block, .body = body});
}

auto sgl::check::impl::flattener::constant_condition(flat_expr_id condition) const -> cc::optional<bool>
{
    // CHK-356: a call inlined to a block that does nothing but leave a constant is that constant
    while (is_valid(condition))
    {
        auto const* const b = entry.at(condition).node.try_as<flat_block>();
        if (b == nullptr || b->body.count != 1)
            break;
        auto const* const leave = entry.at(entry.at(b->body)[0]).node.try_as<flat_leave>();
        if (leave == nullptr || leave->target != b->label)
            break;
        condition = leave->value;
    }
    if (!is_valid(condition) || !is_constant(c.out, entry, condition))
        return {};
    auto const o = evaluate_constant(c.out, entry, condition);
    if (o.status != run_status::ok || o.result.leaves.size() != 1 || o.result.leaves[0].kind != value_kind::boolean)
        return {};
    return o.result.leaves[0].as_bool();
}

void sgl::check::impl::flattener::add_taken(origin from, ast::range_of<flat_stmt_id> body)
{
    add_stmt(from, flat_block{.label = add_label("if"), .body = body});
}

void sgl::check::impl::flattener::flatten_if_branches(origin from,
                                                      cc::span<ast::if_branch const> branches,
                                                      label_id value_block,
                                                      flat_expr_id first)
{
    if (branches.size() < 2 || !ast::is_valid(branches.front().condition))
    {
        mark_failed();
        return;
    }
    // a condition flattened before, by `flatten_value_if`, is one no constant decides, so it is never probed below
    auto const first_expr = is_valid(first) ? isize(index_of(first)) + 1 : entry.exprs.size();
    auto const condition = is_valid(first) ? first : flatten_expr(branches.front().condition);
    auto const rest = branches.subspan({.offset = 1, .size = branches.size() - 1});
    // CHK-356: only the side a constant takes is flattened, so nothing judges what the other side would use
    auto const& head = branches.front();
    if (auto const taken = constant_condition(condition); taken.has_value())
    {
        if (taken.value())
        {
            // EMIT-264: what the arms after it consult is a variant, where a grant can make it false
            auto const after = ast::index_of(head.condition) * 2 + 1;
            if (could_take(condition, first_expr, false))
            {
                if (rest.size() == 1)
                    probe_consults(after,
                                   [&](flattener& p) { (void)p.flatten_arm_body(rest.front().then, value_block); });
                else
                    probe_consults(after, [&](flattener& p) { p.flatten_if_branches(from, rest, value_block); });
            }
            return add_taken(from, granting(head.condition, [&] { return flatten_arm_body(head.then, value_block); }));
        }
        if (could_take(condition, first_expr, true))
            probe_consults(
                ast::index_of(head.condition) * 2, [&](flattener& p)
                { (void)p.granting(head.condition, [&] { return p.flatten_arm_body(head.then, value_block); }); });
        if (rest.size() == 1)
            return add_taken(from, flatten_arm_body(rest.front().then, value_block));
        return flatten_if_branches(from, rest, value_block);
    }
    auto const then_body = granting(head.condition, [&] { return flatten_arm_body(head.then, value_block); });
    auto else_body = ast::range_of<flat_stmt_id>();
    if (rest.size() == 1)
        else_body = flatten_arm_body(rest.front().then, value_block);
    else
    {
        auto const outer = cc::move(block);
        block = {};
        flatten_if_branches(from, rest, value_block);
        else_body = add_list(block);
        block = cc::move(outer);
    }
    add_stmt(from, flat_if{.condition = condition, .then_body = then_body, .else_body = else_body});
}

auto sgl::check::impl::flattener::chain_through(ast::expr_id call) -> ast::range_of<call_site>
{
    // The chain is copied first, since `call_sites` grows under the span that names it.
    auto chain = cc::vector<call_site>();
    chain.push_back_range(entry.at(current()->chain));
    chain.push_back({.file = file(), .call = call});
    auto const range = ast::range_of<call_site>{.first = u32(entry.call_sites.size()), .count = u32(chain.size())};
    entry.call_sites.push_back_range(chain);
    return range;
}

void sgl::check::impl::flattener::bind_parameter(parameter const& p, flat_expr_id value, bool keeps_constructions)
{
    if (!is_valid(value))
    {
        mark_failed();
        return;
    }
    auto const where = target{.kind = target_kind::parameter, .index = i32(p.field)};
    auto const& x = entry.at(value);
    auto const* const ref = x.node.try_as<flat_local_ref>();
    if (ref != nullptr && is_substitutable(value))
        current()->bound.push_back({.where = where, .local = ref->local});
    else if (is_substitutable(value) || is_resource_member(value)
             || (keeps_constructions && is_literal_construction(value)))
        current()->bound.push_back({.where = where, .literal = value});
    else
    {
        auto const local = add_local(local_kind::let, p.name, x.type);
        add_stmt(x.from, flat_let{.local = local, .value = value});
        current()->bound.push_back({.where = where, .local = local});
    }
}

void sgl::check::impl::flattener::bind_defaults(cc::span<parameter const> parameters, cc::span<i32 const> slots)
{
    for (auto p = isize(0); p < parameters.size(); ++p)
    {
        if (slots[p] >= 0)
            continue;
        if (!parameters[p].has_default || !ast::is_valid(parameters[p].field))
        {
            mark_failed();
            continue;
        }
        bind_parameter(parameters[p], flatten_expr(ast().at(parameters[p].field).default_value));
    }
}

auto sgl::check::impl::flattener::inline_call(ast::expr_id call, symbol_id callee, cc::span<flat_expr_id const> arguments)
    -> inlined_body
{
    auto slots = cc::vector<i32>();
    for (auto i = isize(0); i < arguments.size(); ++i)
        slots.push_back(i32(i));
    return inline_bound(call, callee, arguments, slots);
}

auto sgl::check::impl::flattener::inline_bound(ast::expr_id call,
                                               symbol_id callee,
                                               cc::span<flat_expr_id const> values,
                                               cc::span<i32 const> slots,
                                               cc::span<i32 const> handed,
                                               cc::span<type_id const> type_arguments) -> inlined_body
{
    // CHK-340: what the callee's type parameters stand for, in the caller's terms made concrete
    auto bindings = cc::vector<type_id>();
    for (auto i = isize(0); i + 1 < type_arguments.size(); i += 2)
    {
        bindings.push_back(type_arguments[i]);
        bindings.push_back(concrete(type_arguments[i + 1]));
    }
    judge_stage(call, callee);
    c.note_body_read(callee);
    auto const& s = c.out.at(callee);
    auto is_open = s.info < 0 || frames.size() > k_max_inline_depth;
    for (auto const& f : frames)
        is_open = is_open || f.function == callee;
    auto const* const node = is_open ? nullptr : &c.ast_of(s.file).at(s.declaration).node;
    auto const* const function = node != nullptr ? node->try_as<ast::fun_decl>() : nullptr;
    auto const* const property = node != nullptr ? node->try_as<ast::property_decl>() : nullptr;
    auto const* const source = function != nullptr ? &function->body : property != nullptr ? &property->body : nullptr;
    // recursion was reported by the check pass, and an entry point that reaches it is never written
    if (source == nullptr)
    {
        mark_failed();
        return {};
    }
    auto const& info = c.out.functions[s.info];
    auto const parameters = c.out.at(info.parameters);
    auto filled = cc::vector<i32>::create_filled(values.size(), -1);
    for (auto p = isize(0); p < slots.size(); ++p)
        if (slots[p] >= 0 && slots[p] < values.size())
            filled[slots[p]] = i32(p);
    auto is_complete = parameters.size() == slots.size();
    for (auto p = isize(0); is_complete && p < slots.size(); ++p)
        is_complete = slots[p] >= 0 || (parameters[p].has_default && ast::is_valid(parameters[p].field));
    for (auto const p : filled)
        is_complete = is_complete && p >= 0;
    if (!is_complete)
    {
        mark_failed();
        return {};
    }

    auto const chain_range = chain_through(call);

    auto const label = add_label(s.name);
    auto const outer = cc::move(block);
    block = {};

    // A parameter is a value: a literal or an immutable local stands for it, and anything else is bound once.
    auto bound = cc::vector<bound_name>();
    auto argument_lets = cc::vector<local_id>();
    for (auto k = isize(0); k < values.size(); ++k)
    {
        auto const i = filled[k];
        auto const where = target{.kind = target_kind::parameter, .index = i32(parameters[i].field)};
        // CHK-319: a parameter of function type stands for the closure its argument handed over
        if (c.out.at(parameters[i].type).kind == type_kind::function)
        {
            if (k >= handed.size() || handed[k] < 0)
            {
                mark_failed();
                continue;
            }
            bound.push_back({.where = where, .closure = handed[k]});
            continue;
        }
        auto const argument = values[k];
        if (!is_valid(argument))
        {
            mark_failed();
            continue;
        }
        // CHK-316: a `mut` parameter is the caller's place, named again wherever the body names the parameter
        if (parameters[i].is_mut)
        {
            bound.push_back({.where = where, .literal = pin_place(argument)});
            continue;
        }
        auto const& x = entry.at(argument);
        auto const* const ref = x.node.try_as<flat_local_ref>();
        if (ref != nullptr && is_substitutable(argument))
            bound.push_back({.where = where, .local = ref->local});
        // CHK-324: a resource stands wherever the parameter is named, since no target holds one in a local
        else if (is_substitutable(argument) || is_resource_member(argument))
            bound.push_back({.where = where, .literal = argument});
        // an element of a binding array at an index computed once, named again wherever the parameter is (CHK-366)
        else if (is_resource(c.out.at(x.type).kind) && x.node.is<flat_element>())
            bound.push_back({.where = where, .literal = pin_place(argument)});
        else
        {
            auto const local = add_local(local_kind::let, parameters[i].name, x.type);
            add_stmt(x.from, flat_let{.local = local, .value = argument});
            bound.push_back({.where = where, .local = local});
            argument_lets.push_back(local);
        }
    }

    // CHK-338: an instance's body was checked with these bindings, and an inferred result is the instance's
    auto const instance = c.existing_function_instance(callee, bindings);
    auto const result = instance >= 0 ? c.out.function_instances[instance].result : info.result;
    raise_float_level(callee);
    frames.push_back({.function = callee,
                      .file = s.file,
                      .result = result,
                      .return_label = label,
                      .chain = chain_range,
                      .bound = cc::move(bound),
                      .bindings = cc::move(bindings),
                      .instance = instance});
    // `self`, the first parameter of a method or a property, is the receiver its body and its defaults read
    auto const has_receiver = s.role == function_role::property
                           || (s.role == function_role::method && function->receiver == ast::receiver_kind::self);
    if (has_receiver)
    {
        auto const first = target{.kind = target_kind::parameter, .index = i32(parameters[0].field)};
        for (isize i = 0, n = current()->bound.size(); i < n; ++i)
            if (current()->bound[i].where == first)
            {
                auto receiver = current()->bound[i];
                receiver.where = {.kind = target_kind::receiver};
                current()->bound.push_back(receiver);
            }
    }
    // EVAL-80: the defaults of the parameters the call left out come after every written argument, in parameter
    // order, and each reads the parameters before it.
    bind_defaults(parameters, slots);
    // a property's `=>:` block has its value through `yield`, which leaves the property's block
    if (property != nullptr && !ast::is_valid(source->value))
        current()->value_blocks.push_back(label);
    // an arrow body of no value is a statement, such as `=> img.store(xy, v)`, which a leave would drop
    if (ast::is_valid(source->value) && result == checked_module::void_type)
        flatten_expr_stmt({.file = s.file, .expr = source->value}, source->value);
    else if (ast::is_valid(source->value))
        flatten_return({.file = s.file, .expr = source->value}, source->value);
    for (auto const stmt : ast().at(source->statements))
        flatten_stmt(stmt);
    frames.remove_back();

    // EMIT-180: an argument the body reads once, outside a loop, is written where it is read, as a builtin's would be;
    // only one that reads nothing the body could write between, so moving it changes no value
    for (auto const local : argument_lets)
        substitute_single_use(local);

    auto const body = add_list(block);
    block = cc::move(outer);
    return {.label = label, .body = body};
}

auto sgl::check::impl::flattener::is_stable(flat_expr_id id, int depth) const -> bool
{
    if (!is_valid(id) || depth > k_max_inline_depth)
        return false;
    auto const& x = entry.at(id).node;
    if (x.is<flat_literal>() || x.is<flat_int_literal>() || x.is<flat_wide_literal>() || x.is<flat_bool_literal>()
        || x.is<flat_enum_value>())
        return true;
    if (auto const* const ref = x.try_as<flat_local_ref>())
        return !entry.at(ref->local).is_mut;
    if (auto const* const call = x.try_as<flat_call>(); call != nullptr && !call->is_pure)
        return false;
    if (!x.is<flat_construct>() && !x.is<flat_call>() && !x.is<flat_member>() && !x.is<flat_element>()
        && !x.is<flat_not>())
        return false;
    auto result = true;
    for_each_operand(entry, entry.at(id),
                     [&](flat_expr_id operand) { result = result && is_stable(operand, depth + 1); });
    return result;
}

void sgl::check::impl::flattener::substitute_single_use(local_id local)
{
    auto at = isize(-1);
    for (auto i = isize(0); i < block.size() && at < 0; ++i)
        if (auto const* const let = entry.at(block[i]).node.try_as<flat_let>(); let != nullptr && let->local == local)
            at = i;
    if (at < 0)
        return;
    auto const value = entry.at(block[at]).node.as<flat_let>().value;
    if (!is_stable(value))
        return;

    // every read of the local, and whether one stands where it may run more than once
    auto uses = cc::vector<flat_expr_id>();
    auto is_repeated = false;
    auto const expr = [&](auto const& self, flat_expr_id id, int loops, int depth) -> void
    {
        if (!is_valid(id) || depth > k_max_inline_depth)
            return;
        auto const& x = entry.at(id);
        if (auto const* const ref = x.node.try_as<flat_local_ref>(); ref != nullptr && ref->local == local)
        {
            uses.push_back(id);
            is_repeated = is_repeated || loops > 0;
        }
        for_each_operand(entry, x, [&](flat_expr_id operand) { self(self, operand, loops, depth + 1); });
        if (auto const* const b = x.node.try_as<flat_block>())
            for (auto const s : entry.at(b->body))
                is_repeated = is_repeated || reads_in(s, local);
    };
    auto const stmt = [&](auto const& self, flat_stmt_id id, int loops, int depth) -> void
    {
        if (depth > k_max_inline_depth)
            return;
        auto const& s = entry.at(id);
        auto const inner = loops + (s.node.is<flat_loop>() || s.node.is<flat_while>() || s.node.is<flat_for>() ? 1 : 0);
        for_each_expr_of(entry, s, [&](flat_expr_id x) { expr(expr, x, inner, depth + 1); });
        for_each_pattern_of(entry, s,
                            [&](ast::range_of<flat_expr_id> patterns)
                            {
                                for (auto const p : entry.at(patterns))
                                    expr(expr, p, inner, depth + 1);
                            });
        for_each_body_of(entry, s,
                         [&](ast::range_of<flat_stmt_id> body)
                         {
                             for (auto const b : entry.at(body))
                                 self(self, b, inner, depth + 1);
                         });
    };
    for (auto i = at + 1; i < block.size(); ++i)
        stmt(stmt, block[i], 0, 0);
    if (uses.size() != 1 || is_repeated)
        return;

    entry.exprs[index_of(uses[0])].node = entry.at(value).node;
    block.remove_at(at);
}

auto sgl::check::impl::flattener::reads_in(flat_stmt_id id, local_id local) const -> bool
{
    // a read inside a block expression: counted as one that may repeat, which keeps the let
    auto found = false;
    auto const& s = entry.at(id);
    for_each_expr_of(entry, s,
                     [&](flat_expr_id x)
                     {
                         if (auto const* const ref = entry.at(x).node.try_as<flat_local_ref>())
                             found = found || ref->local == local;
                     });
    return found || !entry.at(id).node.is<flat_let>();
}
