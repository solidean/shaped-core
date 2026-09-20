#include <clean-core/common/utility.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
/// Reads the checked AST of one entry point through the side tables and writes its flat tree.
/// The body is known to be sound, so an unexpected shape is a gap of this pass: it sets `is_failed` and the entry
/// point is dropped, which keeps a half-built tree away from every emitter.
struct flattener
{
    checker const& c;
    i32 file = 0;
    flat_entry_point entry;
    bool is_failed = false;

    /// Which flat local a name of the source stands for, keyed the way `target_of` names it.
    struct bound_local
    {
        target where;
        local_id local = local_id::none;
    };
    cc::vector<bound_local> bound;
    /// The statements of the block being written; a temporary's `let` lands here before the statement that needs it.
    cc::vector<flat_stmt_id> block;

    [[nodiscard]] ast::file_ast const& ast() const { return c.ast_of(file); }
    [[nodiscard]] file_tables const& tables() const { return c.out.files[file]; }

    flat_expr_id fail()
    {
        is_failed = true;
        return flat_expr_id::none;
    }

    local_id add_local(local_kind kind, cc::string_view desired, type_id type)
    {
        is_failed = is_failed || !c.is_sound(type);
        entry.locals.push_back({.kind = kind, .name = entry.names.mint(desired), .type = type});
        return local_id(entry.locals.size() - 1);
    }

    template <class Node>
    flat_expr_id add_expr(type_id type, ast::expr_id from, Node node)
    {
        is_failed = is_failed || !c.is_sound(type);
        entry.exprs.push_back({.type = type, .from = {.file = file, .expr = from}, .node = cc::move(node)});
        return flat_expr_id(entry.exprs.size() - 1);
    }

    template <class Node>
    void add_stmt(origin from, Node node)
    {
        entry.stmts.push_back({.from = from, .node = cc::move(node)});
        block.push_back(flat_stmt_id(entry.stmts.size() - 1));
    }

    ast::range_of<flat_expr_id> add_list(cc::vector<flat_expr_id> const& list)
    {
        auto const range = ast::range_of<flat_expr_id>{.first = u32(entry.expr_lists.size()), .count = u32(list.size())};
        entry.expr_lists.push_back_range(list);
        return range;
    }

    flat_expr_id local_ref(local_id local, ast::expr_id from)
    {
        return add_expr(entry.at(local).type, from, flat_local_ref{.local = local});
    }

    // ---- expressions ------------------------------------------------------------------------------------------------

    flat_expr_id flatten_expr(ast::expr_id id)
    {
        if (!ast::is_valid(id))
            return fail();
        auto const& e = ast().at(id);
        auto const type = tables().type_at(id);
        auto const& where = tables().target_at(id);
        if (!is_valid(type))
            return fail();

        if (e.node.is<ast::literal>())
        {
            auto const value = parse_plain_float(c.text_of(file, c.span_of(file, id)));
            return value.has_value() ? add_expr(type, id, flat_literal{.value = value.value()}) : fail();
        }
        if (e.node.is<ast::name>())
        {
            for (auto const& b : bound)
                if (b.where == where)
                    return local_ref(b.local, id);
            return fail();
        }
        if (auto const* const m = e.node.try_as<ast::member>())
        {
            if (where.kind == target_kind::binding_member)
                return add_expr(type, id, flat_binding_member{.binding = where.symbol, .member = where.index});
            if (where.kind != target_kind::field)
                return fail();
            auto const object = flatten_expr(m->object);
            return add_expr(type, id, flat_member{.object = object, .member = where.index});
        }
        if (auto const* const call = e.node.try_as<ast::call>())
        {
            auto const arguments = add_list(flatten_arguments(call->arguments));
            if (where.kind == target_kind::constructor)
                return add_expr(type, id, flat_construct{.arguments = arguments});
            if (where.kind != target_kind::overload)
                return fail();
            auto const intrinsic = c.out.at(where.symbol).intrinsic;
            if (intrinsic == builtin::none)
                return fail();
            return add_expr(type, id, flat_call{.callee = where.symbol, .intrinsic = intrinsic, .arguments = arguments});
        }
        return fail();
    }

    cc::vector<flat_expr_id> flatten_arguments(ast::range_of<ast::argument> range)
    {
        auto result = cc::vector<flat_expr_id>();
        for (auto const& a : ast().at(range))
        {
            if (!a.is_splat)
            {
                result.push_back(flatten_expr(a.value));
                continue;
            }

            // A splat stands for one member access per field, so its value must be a local: evaluated once.
            auto const value = flatten_expr(a.value);
            if (!is_valid(value))
                continue;
            auto local = local_id::none;
            if (auto const* const ref = entry.at(value).node.try_as<flat_local_ref>())
                local = ref->local;
            else
            {
                local = add_local(local_kind::temporary, "splat", entry.at(value).type);
                add_stmt({.file = file, .expr = a.value}, flat_let{.local = local, .value = value});
            }

            auto const members = c.out.at(c.out.at(entry.at(local).type).members);
            for (auto i = isize(0); i < members.size(); ++i)
                result.push_back(add_expr(members[i].type, a.value,
                                          flat_member{.object = local_ref(local, a.value), .member = i32(i)}));
        }
        return result;
    }

    /// An object that converts to `type`: one value per field, in FIELD order, whatever order the source names them in.
    flat_expr_id flatten_object(ast::expr_id id, type_id type)
    {
        auto const elements = ast().at(ast().at(id).node.as<ast::object>().elements);
        auto values = cc::vector<flat_expr_id>();
        for (auto const& m : c.out.at(c.out.at(type).members))
        {
            ast::argument const* named = nullptr;
            for (auto const& e : elements)
                if (c.text_of(file, e.name) == m.name)
                    named = &e;
            if (named == nullptr)
                return fail();
            auto const is_object = ast::is_valid(named->value) && ast().at(named->value).node.is<ast::object>();
            values.push_back(is_object ? flatten_object(named->value, m.type) : flatten_expr(named->value));
        }
        return add_expr(type, id, flat_construct{.arguments = add_list(values)});
    }

    // ---- statements -------------------------------------------------------------------------------------------------

    void flatten_return(origin from, ast::expr_id value)
    {
        auto const is_object = ast::is_valid(value) && ast().at(value).node.is<ast::object>();
        auto const result = is_object ? flatten_object(value, entry.result) : flatten_expr(value);
        add_stmt(from, flat_return{.value = result});
    }

    void flatten_stmt(ast::stmt_id id)
    {
        auto const& s = ast().at(id);
        auto const from = origin{.file = file, .stmt = id};
        if (auto const* const let = s.node.try_as<ast::let_stmt>())
        {
            auto const* const name
                = ast::is_valid(let->pattern) ? ast().at(let->pattern).node.try_as<ast::name>() : nullptr;
            if (name == nullptr)
            {
                is_failed = true;
                return;
            }
            auto const value = flatten_expr(let->value);
            auto const local = add_local(local_kind::let, c.text_of(file, name->where), tables().type_at(let->pattern));
            bound.push_back({.where = {.kind = target_kind::local, .index = i32(id)}, .local = local});
            add_stmt(from, flat_let{.local = local, .value = value});
            return;
        }

        auto const* const e = s.node.try_as<ast::expr_stmt>();
        auto const* const r
            = e != nullptr && ast::is_valid(e->value) ? ast().at(e->value).node.try_as<ast::return_expr>() : nullptr;
        if (r == nullptr)
        {
            is_failed = true;
            return;
        }
        flatten_return(from, r->value);
    }
};
} // namespace

bool checker::is_sound(type_id type) const
{
    if (!is_valid(type) || type == checked_module::error_type)
        return false;
    for (auto const& m : out.at(out.at(type).members))
        if (!is_sound(m.type))
            return false;
    return true;
}

void checker::flatten_entry_point(symbol_id id)
{
    auto const& s = out.at(id);
    auto const& info = out.functions[s.info];
    auto const& note = notes[s.info];
    if (info.entry_stage == stage::none || !note.is_valid_entry || !note.is_body_sound)
        return;

    auto const parameter = out.at(info.parameters)[0];
    if (!is_sound(parameter.type) || !is_sound(info.result))
        return;

    auto f = flattener{.c = *this, .file = s.file};
    f.entry.entry_stage = info.entry_stage;
    f.entry.name = s.name;
    f.entry.function = id;
    f.entry.input = parameter.type;
    f.entry.result = info.result;
    for (auto const binding : out.at(info.bindings))
        f.entry.bindings.push_back(binding);

    // Every module-level name is taken, so no local can hide a type, a binding or a builtin an emitter writes.
    f.entry.names.reserve(s.name);
    for (auto const& other : out.symbols)
        f.entry.names.reserve(other.name);

    auto const local = f.add_local(local_kind::parameter, parameter.name, parameter.type);
    f.bound.push_back({.where = {.kind = target_kind::parameter, .index = i32(parameter.field)}, .local = local});

    auto const& body = ast_of(s.file).at(s.declaration).node.as<ast::fun_decl>().body;
    if (ast::is_valid(body.value))
        f.flatten_return({.file = s.file, .expr = body.value}, body.value);
    for (auto const stmt : ast_of(s.file).at(body.statements))
        f.flatten_stmt(stmt);

    if (f.is_failed)
        return;
    f.entry.body = {.first = u32(f.entry.stmt_lists.size()), .count = u32(f.block.size())};
    f.entry.stmt_lists.push_back_range(f.block);
    out.entry_points.push_back(cc::move(f.entry));
}
