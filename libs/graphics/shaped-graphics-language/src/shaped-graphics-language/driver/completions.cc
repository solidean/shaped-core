#include "references.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/set.hh>
#include <shaped-graphics-language/driver/impl/reference_reader.hh>

using namespace sgl;
using sgl::impl::is_word_char;
using sgl::impl::reader;

namespace
{
constexpr cc::string_view k_keywords[] = {
    "fun",    "struct", "enum",     "binding", "pipeline", "sampler", "const",   "type",  "let",
    "mut",    "if",     "else",     "for",     "in",       "while",   "loop",    "scope", "case",
    "return", "break",  "continue", "yield",   "discard",  "test",    "require", "use",   "module",
    "and",    "or",     "not",      "true",    "false",    "self",    "as",
};

struct completer
{
    reader r;
    i32 file_index;
    u32 offset;
    cc::string_view prefix;
    cc::vector<completion> out;
    cc::set<cc::string> seen;

    void add(cc::string_view label, token_class cls, cc::string detail)
    {
        if (label.empty() || !label.starts_with(prefix) || seen.contains(label))
            return;
        seen.insert(cc::string(label));
        out.push_back({.label = cc::string(label), .cls = cls, .detail = cc::move(detail)});
    }

    [[nodiscard]] static token_class class_of(check::symbol const& s)
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
        case check::symbol_kind::sampler:
            return token_class::binding;
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
        case check::symbol_kind::alias:
            return token_class::type;
        }
        return token_class::name;
    }

    /// Declared `@internal` in the prelude, which a program does not call.
    [[nodiscard]] bool is_internal(check::symbol const& s) const
    {
        auto const* d = r.decl_of(s);
        if (d == nullptr)
            return false;
        for (auto const& at : r.files[s.file].ast.at(d->attributes))
            if (r.files[s.file].file.text_of(at.name) == "internal")
                return true;
        return false;
    }

    void add_members_of(check::symbol_id id, bool is_binding)
    {
        auto const& s = r.m.at(id);
        auto members = cc::span<check::member_info const>();
        if (is_binding && s.info >= 0 && s.info < r.m.bindings.size())
            members = r.m.at(r.m.bindings[s.info].members);
        else if (auto const* t = r.type_of_symbol(id); !is_binding && t != nullptr)
            members = r.m.at(t->members);
        for (auto const& member : members)
            add(member.name, is_binding ? token_class::binding_member : token_class::field,
                cc::format("{}.{} : {}", s.name, member.name, r.type_name(member.type)));
    }

    void add_cases_of(check::symbol_id id)
    {
        auto const& s = r.m.at(id);
        if (auto const* t = r.type_of_symbol(id))
            for (auto const& c : r.m.at(t->cases))
                add(c.name, token_class::enum_case, cc::format("{}.{} = {}", s.name, c.name, c.value));
    }

    /// The functions of `owner`'s type scope: its methods and properties, and its static functions when `statics`.
    void add_functions_of(check::symbol_id owner, bool statics)
    {
        for (auto const& s : r.m.symbols)
        {
            if (s.owner != owner || s.kind != check::symbol_kind::function || !s.operator_spelling.empty())
                continue;
            auto const is_static = s.role == check::function_role::static_;
            if (s.role == check::function_role::constructor || is_static != statics || is_internal(s))
                continue;
            add(s.name, class_of(s), r.symbol_signature(s));
        }
    }

    /// The object of the member being written at `offset`: the `x` of `x.` or of `x.pre`.
    [[nodiscard]] ast::expr_id object_before_dot(u32 dot) const
    {
        auto const& a = r.files[file_index].ast;
        auto const& file = r.files[file_index].file;
        for (auto const& e : a.exprs)
            if (auto const* mb = e.node.try_as<ast::member>(); mb != nullptr && ast::is_valid(mb->object))
                if (mb->name.offset == dot + 1
                    || (mb->name.empty() && is_valid(e.form) && file.at(e.form).where.end() == dot + 1))
                    return mb->object;
        // no member stands there, as when the text after the dot did not parse: the longest expression ending at it
        auto best = ast::expr_id::none;
        auto best_size = u32(0);
        for (auto i = isize(0); i < a.exprs.size(); ++i)
        {
            auto const& e = a.exprs[i];
            if (!is_valid(e.form))
                continue;
            auto const where = file.at(e.form).where;
            if (where.end() != dot || where.length <= best_size
                || !check::is_valid(r.type_at(file_index, ast::expr_id(i))))
                continue;
            best = ast::expr_id(i);
            best_size = where.length;
        }
        return best;
    }

    /// What the symbol a name before a dot stands for has: a binding's members, an enum's cases, a type's statics.
    [[nodiscard]] bool complete_symbol_members(check::symbol_id id)
    {
        auto const& s = r.m.at(id);
        if (s.kind == check::symbol_kind::binding)
            add_members_of(id, true);
        else if (s.kind == check::symbol_kind::enumeration)
        {
            add_cases_of(id);
            add_functions_of(id, true);
        }
        else if (s.kind == check::symbol_kind::structure)
            add_functions_of(id, true);
        else
            return false;
        return true;
    }

    void complete_members_of_type(check::type_id type)
    {
        if (!check::is_valid(type) || check::index_of(type) >= r.m.types.size())
            return;
        auto const owner = r.m.at(type).symbol;
        if (!check::is_valid(owner))
            return;
        if (r.m.at(owner).kind == check::symbol_kind::structure)
            add_members_of(owner, false);
        add_functions_of(owner, false);
    }

    /// `x.` where nothing before the dot was checked, as while its line does not parse yet: `x` read as a name.
    void complete_member_by_name(u32 dot)
    {
        auto const text = cc::string_view(r.files[file_index].file.source);
        auto start = isize(dot);
        while (start > 0 && is_word_char(text[start - 1]))
            --start;
        auto const word = text.subview({.start = start, .end = isize(dot)});
        if (word.empty())
            return;
        for (auto const& n : scoped_names())
            if (n.label == word)
            {
                complete_members_of_type(n.type);
                return;
            }
        for (auto i = isize(0); i < r.m.symbols.size(); ++i)
        {
            auto const& s = r.m.symbols[i];
            auto const is_visible = s.file == file_index || s.file < r.m.prelude_file_count();
            if (is_visible && s.name == word && !check::is_valid(s.owner) && complete_symbol_members(check::symbol_id(i)))
                return;
        }
    }

    void complete_member(u32 dot)
    {
        auto const object = object_before_dot(dot);
        if (!ast::is_valid(object))
        {
            complete_member_by_name(dot);
            return;
        }
        auto const t = r.target_at(file_index, object);
        if (t.kind == check::target_kind::symbol && check::is_valid(t.symbol) && complete_symbol_members(t.symbol))
            return;
        complete_members_of_type(r.type_at(file_index, object));
    }

    [[nodiscard]] source_span body_span(ast::body const& b) const
    {
        return is_valid(b.form) ? r.files[file_index].file.at(b.form).where : source_span();
    }

    [[nodiscard]] static bool contains(source_span outer, u32 at) { return at >= outer.offset && at <= outer.end(); }

    /// A local or a parameter visible at `offset`, with the type the check gave it.
    struct scoped_name
    {
        cc::string label;
        token_class cls = token_class::local;
        cc::string detail;
        check::type_id type = check::type_id::none;
    };

    /// The locals and parameters of the function `offset` stands in, each one visible there, the latest local first.
    /// A `let` still being written at `offset` declares nothing yet.
    [[nodiscard]] cc::vector<scoped_name> scoped_names() const
    {
        auto out = cc::vector<scoped_name>();
        auto const& a = r.files[file_index].ast;
        auto const& file = r.files[file_index].file;
        // the innermost function around the cursor
        auto around = source_span();
        auto const* fun = static_cast<ast::fun_decl const*>(nullptr);
        for (auto const& d : a.decls)
        {
            if (!is_valid(d.form))
                continue;
            auto const where = file.at(d.form).where;
            if (!contains(where, offset) || (!around.empty() && where.length > around.length))
                continue;
            if (auto const* fd = d.node.try_as<ast::fun_decl>())
            {
                around = where;
                fun = fd;
            }
        }
        if (fun == nullptr)
            return out;

        // a block that ends before the cursor takes its locals with it
        auto blocks = cc::vector<source_span>();
        for (auto const& s : a.stmts)
        {
            if (auto const* i = s.node.try_as<ast::if_stmt>())
                for (auto const& b : a.at(i->branches))
                    blocks.push_back(body_span(b.then));
            else if (auto const* f = s.node.try_as<ast::for_stmt>())
                blocks.push_back(body_span(f->body));
            else if (auto const* w = s.node.try_as<ast::while_stmt>())
                blocks.push_back(body_span(w->body));
            else if (auto const* sc = s.node.try_as<ast::scope_stmt>())
                blocks.push_back(body_span(sc->body));
        }
        auto const is_visible = [&](u32 declared)
        {
            for (auto const& b : blocks)
                if (!b.empty() && contains(b, declared) && !contains(b, offset))
                    return false;
            return true;
        };

        auto locals = cc::vector<scoped_name>();
        for (auto i = isize(0); i < a.stmts.size(); ++i)
        {
            auto const& s = a.stmts[i];
            auto pattern = ast::expr_id::none;
            auto in_body = source_span();
            if (auto const* l = s.node.try_as<ast::let_stmt>())
            {
                pattern = l->pattern;
                if (is_valid(s.form) && contains(file.at(s.form).where, offset))
                    continue;
            }
            else if (auto const* f = s.node.try_as<ast::for_stmt>())
            {
                pattern = f->variable;
                in_body = body_span(f->body);
            }
            if (!ast::is_valid(pattern))
                continue;
            auto const* n = a.at(pattern).node.try_as<ast::name>();
            if (n == nullptr || !contains(around, n->where.offset) || n->where.end() > offset)
                continue;
            if (!in_body.empty() ? !contains(in_body, offset) : !is_visible(n->where.offset))
                continue;
            auto ref = reference{.where = n->where};
            r.resolve_local(ref, file_index, ast::stmt_id(i), check::type_id::none);
            locals.push_back({.label = cc::string(file.text_of(n->where)),
                              .cls = token_class::local,
                              .detail = cc::move(ref.signature),
                              .type = r.type_at(file_index, pattern)});
        }
        // the latest declaration of a name shadows the ones before it
        for (auto i = locals.size(); i > 0; --i)
            out.push_back(cc::move(locals[i - 1]));
        for (auto const& p : a.at(fun->parameters))
        {
            auto const name = file.text_of(p.name);
            auto const type = r.type_at(file_index, p.type);
            out.push_back(
                {.label = cc::string(name),
                 .cls = name == "self" ? token_class::self_ : token_class::parameter,
                 .detail = check::is_valid(type) ? cc::format("{} : {}", name, r.type_name(type)) : cc::string(name),
                 .type = type});
        }
        return out;
    }

    void complete_locals()
    {
        for (auto& n : scoped_names())
            add(n.label, n.cls, cc::move(n.detail));
    }

    void complete_names()
    {
        complete_locals();
        auto const& modules = r.m.file_modules;
        auto const own_module = file_index < modules.size() ? cc::string_view(modules[file_index]) : cc::string_view();
        // the document's own declarations first, then its module's, then the prelude's
        for (auto pass = 0; pass < 3; ++pass)
            for (auto const& s : r.m.symbols)
            {
                auto const is_own = s.file == file_index;
                auto const is_module
                    = !is_own && !own_module.empty() && s.file < modules.size() && modules[s.file] == own_module;
                auto const is_prelude = s.file < r.m.prelude_file_count();
                if ((pass == 0 && !is_own) || (pass == 1 && !is_module) || (pass == 2 && !is_prelude))
                    continue;
                if (s.kind == check::symbol_kind::test || s.role == check::function_role::constructor
                    || check::is_valid(s.owner) || !s.operator_spelling.empty() || is_internal(s))
                    continue;
                add(s.name, class_of(s), r.symbol_signature(s));
            }
        for (auto const k : k_keywords)
            add(k, token_class::keyword, {});
    }
};
} // namespace

cc::vector<completion> sgl::completions_at(cc::span<check::module_file const> files,
                                           check::checked_module const& m,
                                           i32 file_index,
                                           u32 offset)
{
    auto c = completer{.r = {.files = files, .m = m}, .file_index = file_index, .offset = offset};
    if (!c.r.has_file(file_index))
        return {};
    auto const text = cc::string_view(files[file_index].file.source);
    auto start = isize(cc::min(isize(offset), text.size()));
    auto const end = start;
    while (start > 0 && is_word_char(text[start - 1]))
        --start;
    c.prefix = text.subview({.start = start, .end = end});
    if (start > 0 && text[start - 1] == '.')
        c.complete_member(u32(start - 1));
    else
        c.complete_names();
    return cc::move(c.out);
}

bool sgl::is_keyword(cc::string_view word)
{
    for (auto const k : k_keywords)
        if (k == word)
            return true;
    return false;
}
