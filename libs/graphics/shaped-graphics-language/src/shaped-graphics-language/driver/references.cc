#include "references.hh"

#include <shaped-graphics-language/driver/impl/reference_reader.hh>

using namespace sgl;
using sgl::impl::reader;

namespace
{
/// The span of the name an expression of the AST names, as `classify` reads it; empty for any other expression.
[[nodiscard]] source_span name_span_of(parsed_file const& file, ast::expr const& e)
{
    if (auto const* n = e.node.try_as<ast::name>())
        return n->where;
    if (auto const* mb = e.node.try_as<ast::member>())
        return mb->name;
    if (auto const* ld = e.node.try_as<ast::leading_dot>())
        return ld->name;
    if (e.node.try_as<ast::self_ref>() != nullptr && is_valid(e.form))
        return file.at(e.form).where;
    return {};
}

[[nodiscard]] bool is_name_class(token_class c)
{
    switch (c)
    {
    case token_class::keyword:
    case token_class::control:
    case token_class::number:
    case token_class::string:
    case token_class::comment:
    case token_class::doc_comment:
    case token_class::op:
    case token_class::attribute:
        return false;
    default:
        return true;
    }
}
} // namespace

cc::optional<reference> sgl::reference_at(cc::span<check::module_file const> files,
                                          check::checked_module const& m,
                                          i32 file_index,
                                          u32 offset)
{
    auto const r_ = reader{.files = files, .m = m};
    if (!r_.has_file(file_index))
        return {};
    auto const& file = files[file_index].file;
    auto const& a = files[file_index].ast;

    // a cursor inside a name wins over one right after the name before it
    auto found = cc::optional<classified_span>();
    for (auto const& s : classify(file, a, {.module = &m, .file_index = file_index}))
    {
        if (!is_name_class(s.cls) || s.where.empty() || offset < s.where.offset || offset > s.where.end())
            continue;
        if (!found.has_value() || offset < s.where.end())
            found = s;
    }
    if (!found.has_value())
        return {};

    auto r = reference{.where = found.value().where, .cls = found.value().cls};
    for (auto i = isize(0); i < a.exprs.size(); ++i)
    {
        if (name_span_of(file, a.exprs[i]) != r.where)
            continue;
        auto const id = ast::expr_id(i);
        auto const t = r_.target_at(file_index, id);
        auto const type = r_.type_at(file_index, id);
        if (t.kind != check::target_kind::none)
            r_.resolve_target(r, file_index, t, type);
        else if (a.exprs[i].node.try_as<ast::name>() != nullptr)
            // the name a `let` or a `for` declares, which is no use of anything
            for (auto k = isize(0); k < a.stmts.size(); ++k)
            {
                auto const* l = a.stmts[k].node.try_as<ast::let_stmt>();
                auto const* f = a.stmts[k].node.try_as<ast::for_stmt>();
                if ((l != nullptr && l->pattern == id) || (f != nullptr && f->variable == id))
                    r_.resolve_local(r, file_index, ast::stmt_id(k), type);
            }
        break;
    }
    if (r.signature.empty())
        r_.resolve_declaration(r, file_index);
    if (r.signature.empty())
        return {};
    r.doc = r_.doc_above(r.declaring_file, r.declared);
    return r;
}
