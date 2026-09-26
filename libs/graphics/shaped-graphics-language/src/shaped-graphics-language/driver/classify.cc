#include "classify.hh"

#include <clean-core/common/assert.hh>
#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

using namespace sgl;

namespace
{
/// One class per token, filled pass by pass: the syntax first, then declarations, then what the check resolved.
struct classifier
{
    parsed_file const& file;
    ast::file_ast const& ast;
    classify_options const& options;

    /// Parallel to `file.tokens`; a token without a class keeps `has_class` false.
    cc::vector<classified_span> by_token;
    cc::vector<bool> has_class;

    /// The token that starts at `offset`, or -1 when none does.
    [[nodiscard]] isize token_at(u32 offset) const
    {
        auto lo = isize(0);
        auto hi = file.tokens.size();
        while (lo < hi)
        {
            auto const mid = lo + (hi - lo) / 2;
            if (file.tokens[mid].where.offset < offset)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo < file.tokens.size() && file.tokens[lo].where.offset == offset ? lo : -1;
    }

    void set(isize t, token_class cls, bool is_declaration = false, bool is_from_prelude = false)
    {
        if (t < 0 || t >= by_token.size())
            return;
        by_token[t] = {.where = file.tokens[t].where,
                       .cls = cls,
                       .is_declaration = is_declaration,
                       .is_from_prelude = is_from_prelude};
        has_class[t] = true;
    }

    /// A name span is one symbol token, so the token that starts there is the name.
    void set_span(source_span where, token_class cls, bool is_declaration = false, bool is_from_prelude = false)
    {
        if (!where.empty())
            set(token_at(where.offset), cls, is_declaration, is_from_prelude);
    }

    [[nodiscard]] bool is_prelude_file(i32 f) const { return f >= 0 && f < options.prelude_file_count; }

    void classify_tokens()
    {
        for (auto t = isize(0); t < file.tokens.size(); ++t)
        {
            auto const& tok = file.tokens[t];
            switch (tok.kind)
            {
            case token_kind::comment:
                set(t, file.text_of(tok.where).starts_with("///") ? token_class::doc_comment : token_class::comment);
                break;
            case token_kind::quote_open:
            case token_kind::quote_close:
            case token_kind::string_body:
                set(t, token_class::string);
                break;
            case token_kind::op:
            case token_kind::arrow:
            case token_kind::double_arrow:
            case token_kind::dollar:
                set(t, token_class::op);
                break;
            case token_kind::symbol:
                set(t, token_class::name);
                break;
            default:
                break;
            }
        }
    }

    void classify_forms()
    {
        for (auto const& f : file.forms)
        {
            auto const t = is_valid(f.token) ? index_of(f.token) : isize(-1);
            switch (f.kind)
            {
            case form_kind::keyword:
                set(t, token_class::keyword);
                break;
            case form_kind::op:
            case form_kind::prefix_operator:
            case form_kind::postfix_operator:
                set(t, token_class::op);
                break;
            case form_kind::number:
            case form_kind::hash_literal:
            {
                // fused from several tokens, sign included: one span over all of them
                auto const first = token_at(f.where.offset);
                if (first < 0)
                    break;
                set(first, token_class::number);
                by_token[first].where = f.where;
                for (auto k = first + 1; k < file.tokens.size() && file.tokens[k].where.offset < f.where.end(); ++k)
                    has_class[k] = false;
                break;
            }
            default:
                break;
            }
        }
        for (auto const& g : file.groups)
            if (g.kind == group_kind::attribute && is_valid(g.token))
                set(index_of(g.token), token_class::attribute);
    }

    [[nodiscard]] token_class class_of_symbol(check::symbol const& s) const
    {
        switch (s.kind)
        {
        case check::symbol_kind::structure:
            return token_class::struct_;
        case check::symbol_kind::enumeration:
            return token_class::enum_;
        case check::symbol_kind::binding:
            return token_class::binding;
        case check::symbol_kind::pipeline:
            return token_class::pipeline;
        case check::symbol_kind::constant:
            return token_class::constant;
        case check::symbol_kind::function:
            switch (s.role)
            {
            case check::function_role::method:
                return token_class::method;
            case check::function_role::property:
                return token_class::property;
            case check::function_role::constructor:
                return token_class::struct_;
            case check::function_role::free:
            case check::function_role::static_:
                return token_class::function;
            }
            return token_class::function;
        case check::symbol_kind::test:
            return token_class::name;
        case check::symbol_kind::unsupported:
            return token_class::type;
        }
        return token_class::name;
    }

    /// Every field is a parameter until a declaration says it is a member; lambdas and type parameters included.
    void classify_fields()
    {
        auto const is_prelude = is_prelude_file(options.file);
        for (auto const& f : ast.fields)
        {
            auto const text = file.text_of(f.name);
            if (f.name.empty() || text == "_")
                continue;
            set_span(f.name, text == "self" ? token_class::self_ : token_class::parameter, true, is_prelude);
        }
    }

    void classify_members(ast::range_of<ast::decl_id> members, token_class cls, bool is_prelude)
    {
        for (auto const id : ast.at(members))
            if (auto const* fd = ast.at(id).node.try_as<ast::field_decl>(); fd != nullptr && ast::is_valid(fd->field))
                set_span(ast.at(fd->field).name, cls, true, is_prelude);
    }

    void classify_declarations()
    {
        // the checker's symbol for a declaration of this file, which knows a function's role
        auto symbol_of_decl = cc::vector<check::symbol const*>();
        symbol_of_decl.resize_to_filled(ast.decls.size(), nullptr);
        if (options.module != nullptr)
            for (auto const& s : options.module->symbols)
                if (s.file == options.file && ast::is_valid(s.declaration)
                    && ast::index_of(s.declaration) < ast.decls.size() && s.role != check::function_role::constructor)
                    symbol_of_decl[ast::index_of(s.declaration)] = &s;

        auto const is_prelude = is_prelude_file(options.file);
        for (auto i = isize(0); i < ast.decls.size(); ++i)
        {
            auto const& node = ast.decls[i].node;
            if (auto const* d = node.try_as<ast::fun_decl>())
            {
                auto cls = d->receiver != ast::receiver_kind::none || !d->extended_type.empty() ? token_class::method
                                                                                                : token_class::function;
                if (auto const* s = symbol_of_decl[i])
                    cls = class_of_symbol(*s);
                set_span(d->name, cls, true, is_prelude);
                set_span(d->extended_type, token_class::type, false, is_prelude);
                for (auto const& p : ast.at(d->type_parameters))
                    set_span(p.name, token_class::type, true, is_prelude);
            }
            else if (auto const* d = node.try_as<ast::struct_decl>())
            {
                set_span(d->name, token_class::struct_, true, is_prelude);
                classify_members(d->members, token_class::field, is_prelude);
            }
            else if (auto const* d = node.try_as<ast::enum_decl>())
                set_span(d->name, token_class::enum_, true, is_prelude);
            else if (auto const* d = node.try_as<ast::enum_case_decl>())
                set_span(d->name, token_class::enum_case, true, is_prelude);
            else if (auto const* d = node.try_as<ast::type_decl>())
                set_span(d->name, token_class::type, true, is_prelude);
            else if (auto const* d = node.try_as<ast::const_decl>())
                set_span(d->name, token_class::constant, true, is_prelude);
            else if (auto const* d = node.try_as<ast::binding_decl>())
            {
                set_span(d->name, token_class::binding, true, is_prelude);
                classify_members(d->members, token_class::binding_member, is_prelude);
            }
            else if (auto const* d = node.try_as<ast::sampler_decl>())
                set_span(d->name, token_class::binding, true, is_prelude);
            else if (auto const* d = node.try_as<ast::pipeline_decl>())
                set_span(d->name, token_class::pipeline, true, is_prelude);
            else if (auto const* d = node.try_as<ast::property_decl>())
            {
                set_span(d->name, token_class::property, true, is_prelude);
                set_span(d->extended_type, token_class::type, false, is_prelude);
            }
        }
    }

    /// A `let` or a `for` declares its variable, whether or not the check got to it.
    void classify_locals()
    {
        for (auto const& s : ast.stmts)
        {
            auto pattern = ast::expr_id::none;
            auto cls = token_class::local;
            if (auto const* l = s.node.try_as<ast::let_stmt>())
            {
                pattern = l->pattern;
                cls = l->is_mut ? token_class::mutable_local : token_class::local;
            }
            else if (auto const* f = s.node.try_as<ast::for_stmt>())
                pattern = f->variable;
            if (!ast::is_valid(pattern))
                continue;
            if (auto const* n = ast.at(pattern).node.try_as<ast::name>())
                set_span(n->where, cls, true);
        }
    }

    /// What each name, member and `self` resolved to, which only the check knows.
    void classify_uses()
    {
        if (options.module == nullptr || options.file < 0 || options.file >= options.module->files.size())
            return;
        auto const& m = *options.module;
        auto const& tables = m.files[options.file];
        auto const count = cc::min(ast.exprs.size(), tables.target_of.size());
        for (auto i = isize(0); i < count; ++i)
        {
            auto const& e = ast.exprs[i];
            auto where = source_span();
            if (auto const* n = e.node.try_as<ast::name>())
                where = n->where;
            else if (auto const* mb = e.node.try_as<ast::member>())
                where = mb->name;
            else if (auto const* ld = e.node.try_as<ast::leading_dot>())
                where = ld->name;
            else if (e.node.try_as<ast::self_ref>() != nullptr && is_valid(e.form))
                where = file.at(e.form).where;
            else if (e.node.try_as<ast::void_ref>() != nullptr && is_valid(e.form))
            {
                set_span(file.at(e.form).where, token_class::type, false, true);
                continue;
            }
            else
                continue;

            auto const t = token_at(where.offset);
            if (t < 0)
                continue;
            // a declaring occurrence keeps the flag its declaration pass gave it
            auto const is_declaration = has_class[t] && by_token[t].is_declaration;
            auto const& target = tables.target_of[i];
            auto const symbol_prelude
                = [&] { return check::is_valid(target.symbol) && is_prelude_file(m.at(target.symbol).file); };
            switch (target.kind)
            {
            case check::target_kind::none:
                break;
            case check::target_kind::local:
            {
                auto cls = token_class::local;
                auto const stmt = ast::stmt_id(target.index);
                if (ast::is_valid(stmt) && ast::index_of(stmt) < ast.stmts.size())
                    if (auto const* l = ast.at(stmt).node.try_as<ast::let_stmt>(); l != nullptr && l->is_mut)
                        cls = token_class::mutable_local;
                set(t, cls, is_declaration);
                break;
            }
            case check::target_kind::parameter:
                set(t, token_class::parameter, is_declaration);
                break;
            case check::target_kind::symbol:
            case check::target_kind::overload:
            case check::target_kind::constructor:
                if (check::is_valid(target.symbol))
                    set(t,
                        target.kind == check::target_kind::constructor ? token_class::struct_
                                                                       : class_of_symbol(m.at(target.symbol)),
                        is_declaration, symbol_prelude());
                break;
            case check::target_kind::field:
                set(t, token_class::field, is_declaration, symbol_prelude());
                break;
            case check::target_kind::binding_member:
                set(t, token_class::binding_member, is_declaration, symbol_prelude());
                break;
            case check::target_kind::enum_case:
                set(t, token_class::enum_case, is_declaration, symbol_prelude());
                break;
            case check::target_kind::receiver:
                set(t, token_class::self_, is_declaration);
                break;
            }
        }
    }

    cc::vector<classified_span> run()
    {
        by_token.resize_to_defaulted(file.tokens.size());
        has_class.resize_to_filled(file.tokens.size(), false);
        classify_tokens();
        classify_forms();
        classify_fields();
        classify_declarations();
        classify_locals();
        classify_uses();

        auto out = cc::vector<classified_span>();
        for (auto t = isize(0); t < by_token.size(); ++t)
            if (has_class[t])
                out.push_back(by_token[t]);
        return out;
    }
};
} // namespace

cc::vector<classified_span> sgl::classify(parsed_file const& file, ast::file_ast const& ast, classify_options const& options)
{
    CC_ASSERT(options.module == nullptr || options.file >= 0, "a checked module needs the file's position in it");
    return classifier{.file = file, .ast = ast, .options = options}.run();
}

cc::string_view sgl::to_string(token_class c)
{
    switch (c)
    {
    case token_class::keyword:
        return "keyword";
    case token_class::number:
        return "number";
    case token_class::string:
        return "string";
    case token_class::comment:
        return "comment";
    case token_class::doc_comment:
        return "doc-comment";
    case token_class::op:
        return "op";
    case token_class::attribute:
        return "attribute";
    case token_class::struct_:
        return "struct";
    case token_class::enum_:
        return "enum";
    case token_class::type:
        return "type";
    case token_class::function:
        return "function";
    case token_class::method:
        return "method";
    case token_class::property:
        return "property";
    case token_class::field:
        return "field";
    case token_class::binding:
        return "binding";
    case token_class::binding_member:
        return "binding-member";
    case token_class::enum_case:
        return "enum-case";
    case token_class::constant:
        return "constant";
    case token_class::pipeline:
        return "pipeline";
    case token_class::parameter:
        return "parameter";
    case token_class::local:
        return "local";
    case token_class::mutable_local:
        return "mutable-local";
    case token_class::self_:
        return "self";
    case token_class::name:
        return "name";
    }
    CC_UNREACHABLE("unknown token_class");
}
