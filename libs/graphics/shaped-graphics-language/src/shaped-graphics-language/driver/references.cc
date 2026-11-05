#include "references.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/set.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

using namespace sgl;

namespace
{
[[nodiscard]] bool is_word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

[[nodiscard]] cc::string_view trimmed(cc::string_view s)
{
    auto first = isize(0);
    auto last = s.size();
    while (first < last && (s[first] == ' ' || s[first] == '\t'))
        ++first;
    while (last > first && (s[last - 1] == ' ' || s[last - 1] == '\t' || s[last - 1] == '\r'))
        --last;
    return s.subview({.start = first, .end = last});
}

/// The name a declaration declares; empty for one that declares none, such as a `use`.
[[nodiscard]] source_span name_of_decl(ast::decl const& d)
{
    if (auto const* x = d.node.try_as<ast::fun_decl>())
        return x->name;
    if (auto const* x = d.node.try_as<ast::struct_decl>())
        return x->name;
    if (auto const* x = d.node.try_as<ast::enum_decl>())
        return x->name;
    if (auto const* x = d.node.try_as<ast::enum_case_decl>())
        return x->name;
    if (auto const* x = d.node.try_as<ast::type_decl>())
        return x->name;
    if (auto const* x = d.node.try_as<ast::const_decl>())
        return x->name;
    if (auto const* x = d.node.try_as<ast::binding_decl>())
        return x->name;
    if (auto const* x = d.node.try_as<ast::sampler_decl>())
        return x->name;
    if (auto const* x = d.node.try_as<ast::pipeline_decl>())
        return x->name;
    if (auto const* x = d.node.try_as<ast::property_decl>())
        return x->name;
    return {};
}

/// What reads the module and its files, which every lookup here shares.
struct reader
{
    cc::span<check::module_file const> files;
    check::checked_module const& m;

    [[nodiscard]] bool has_file(i32 f) const { return f >= 0 && f < files.size() && f < m.files.size(); }

    [[nodiscard]] cc::string type_name(check::type_id t) const
    {
        if (!check::is_valid(t) || check::index_of(t) >= m.types.size())
            return {};
        return cc::string(m.name_of(t));
    }

    [[nodiscard]] check::type_id type_at(i32 file, ast::expr_id e) const
    {
        if (!has_file(file) || !ast::is_valid(e) || ast::index_of(e) >= m.files[file].type_of.size())
            return check::type_id::none;
        return m.files[file].type_at(e);
    }

    [[nodiscard]] check::target target_at(i32 file, ast::expr_id e) const
    {
        if (!has_file(file) || !ast::is_valid(e) || ast::index_of(e) >= m.files[file].target_of.size())
            return {};
        return m.files[file].target_at(e);
    }

    [[nodiscard]] ast::decl const* decl_of(check::symbol const& s) const
    {
        if (!has_file(s.file) || !ast::is_valid(s.declaration)
            || ast::index_of(s.declaration) >= files[s.file].ast.decls.size())
            return nullptr;
        return &files[s.file].ast.at(s.declaration);
    }

    [[nodiscard]] source_span declared_name_of(check::symbol const& s) const
    {
        auto const* d = decl_of(s);
        return d != nullptr ? name_of_decl(*d) : source_span();
    }

    [[nodiscard]] source_span field_name(i32 file, ast::field_id f) const
    {
        if (!has_file(file) || !ast::is_valid(f) || ast::index_of(f) >= files[file].ast.fields.size())
            return {};
        return files[file].ast.at(f).name;
    }

    /// The `///` lines right above the line that declares `declared`, attribute lines between them skipped.
    [[nodiscard]] cc::string doc_above(i32 file, source_span declared) const
    {
        if (!has_file(file) || declared.empty())
            return {};
        auto const text = cc::string_view(files[file].file.source);
        auto start = isize(declared.offset);
        while (start > 0 && text[start - 1] != '\n')
            --start;
        auto lines = cc::vector<cc::string_view>();
        while (start > 0)
        {
            auto const line_end = start - 1;
            auto line_start = line_end;
            while (line_start > 0 && text[line_start - 1] != '\n')
                --line_start;
            auto const line = trimmed(text.subview({.start = line_start, .end = line_end}));
            if (line.starts_with("///"))
            {
                auto body = line.subview(3);
                if (body.starts_with(" "))
                    body = body.subview(1);
                lines.push_back(body);
            }
            else if (!line.starts_with("@"))
                break;
            start = line_start;
        }
        auto out = cc::string();
        for (auto i = lines.size(); i > 0; --i)
        {
            if (!out.empty())
                out += "\n";
            out += lines[i - 1];
        }
        return out;
    }

    [[nodiscard]] cc::string owner_prefix(check::symbol const& s) const
    {
        return check::is_valid(s.owner) ? m.at(s.owner).name + "." : cc::string();
    }

    [[nodiscard]] cc::string function_signature(check::symbol const& s) const
    {
        auto out = cc::string("fun ") + owner_prefix(s) + s.name;
        auto const* d = decl_of(s);
        auto const* fd = d != nullptr ? d->node.try_as<ast::fun_decl>() : nullptr;
        if (fd != nullptr && !fd->type_parameters.empty())
        {
            out += "[";
            auto first = true;
            for (auto const& p : files[s.file].ast.at(fd->type_parameters))
            {
                out += first ? "" : ", ";
                out += files[s.file].file.text_of(p.name);
                first = false;
            }
            out += "]";
        }
        if (s.info < 0 || s.info >= m.functions.size())
            return out;
        auto const& f = m.functions[s.info];
        if (s.role != check::function_role::property)
        {
            out += "(";
            auto first = true;
            for (auto const& p : m.at(f.parameters))
            {
                out += first ? "" : ", ";
                first = false;
                if (p.name == "self")
                {
                    out += p.is_mut ? "mut self" : "self";
                    continue;
                }
                out.appendf("{}{}: {}{}", p.is_named_only ? "." : "", p.name, p.is_mut ? "mut " : "", type_name(p.type));
                if (p.has_default && has_file(s.file) && ast::is_valid(p.field))
                {
                    auto const& own = files[s.file];
                    auto const def = own.ast.at(p.field).default_value;
                    if (ast::is_valid(def) && is_valid(own.ast.at(def).form))
                        out.appendf(" = {}", own.file.text_of(own.file.at(own.ast.at(def).form).where));
                }
            }
            out += ")";
        }
        if (f.result != check::checked_module::void_type && check::is_valid(f.result))
            out.appendf(" -> {}", type_name(f.result));
        return out;
    }

    [[nodiscard]] cc::string symbol_signature(check::symbol const& s) const
    {
        switch (s.kind)
        {
        case check::symbol_kind::structure:
            return cc::string("struct ") + s.name;
        case check::symbol_kind::enumeration:
            return cc::string("enum ") + s.name;
        case check::symbol_kind::function:
            return s.role == check::function_role::constructor ? cc::string("struct ") + s.name : function_signature(s);
        case check::symbol_kind::binding:
            return cc::string("binding ") + s.name;
        case check::symbol_kind::pipeline:
            return cc::string("pipeline ") + s.name;
        case check::symbol_kind::constant:
        {
            auto const type = type_name(s.type);
            return type.empty() ? cc::string("const ") + s.name : cc::format("const {} : {}", s.name, type);
        }
        case check::symbol_kind::sampler:
            return cc::string("sampler ") + s.name;
        case check::symbol_kind::test:
            return "test";
        case check::symbol_kind::unsupported:
            return cc::string("type ") + s.name;
        }
        return s.name;
    }

    /// The members of struct or enum `s`'s type, empty where it has none.
    [[nodiscard]] check::type_info const* type_of_symbol(check::symbol_id id) const
    {
        auto const& s = m.at(id);
        if (check::is_valid(s.type) && check::index_of(s.type) < m.types.size() && m.at(s.type).symbol == id)
            return &m.at(s.type);
        for (auto const& t : m.types)
            if (t.symbol == id && (t.kind == check::type_kind::structure || t.kind == check::type_kind::enumeration))
                return &t;
        return nullptr;
    }

    void resolve_symbol(reference& r, check::symbol_id id) const
    {
        auto const& s = m.at(id);
        r.declaring_file = s.file;
        r.declared = declared_name_of(s);
        r.signature = symbol_signature(s);
    }

    void resolve_member(reference& r, check::symbol_id owner, i32 index, bool is_binding) const
    {
        auto const& s = m.at(owner);
        auto members = cc::span<check::member_info const>();
        if (is_binding && s.info >= 0 && s.info < m.bindings.size())
            members = m.at(m.bindings[s.info].members);
        else if (auto const* t = type_of_symbol(owner); !is_binding && t != nullptr)
            members = m.at(t->members);
        if (index < 0 || index >= members.size())
            return;
        auto const& member = members[index];
        r.declaring_file = s.file;
        r.declared = field_name(s.file, member.field);
        r.signature = cc::format("{}.{} : {}", s.name, member.name, type_name(member.type));
    }

    void resolve_case(reference& r, check::symbol_id owner, i32 index) const
    {
        auto const& s = m.at(owner);
        auto const* t = type_of_symbol(owner);
        if (t == nullptr || index < 0 || index >= isize(t->cases.count))
            return;
        auto const& c = m.at(t->cases)[index];
        r.declaring_file = s.file;
        if (has_file(s.file) && ast::is_valid(c.declaration)
            && ast::index_of(c.declaration) < files[s.file].ast.decls.size())
            r.declared = name_of_decl(files[s.file].ast.at(c.declaration));
        r.signature = cc::format("{}.{} = {}", s.name, c.name, c.value);
    }

    /// The `let` or `for` statement `stmt` of `file`, resolved as the local it declares.
    void resolve_local(reference& r, i32 file, ast::stmt_id stmt, check::type_id use_type) const
    {
        auto const& a = files[file].ast;
        if (!ast::is_valid(stmt) || ast::index_of(stmt) >= a.stmts.size())
            return;
        auto pattern = ast::expr_id::none;
        auto keyword = cc::string_view("let");
        if (auto const* l = a.at(stmt).node.try_as<ast::let_stmt>())
        {
            pattern = l->pattern;
            keyword = l->is_mut ? "let mut" : "let";
        }
        else if (auto const* f = a.at(stmt).node.try_as<ast::for_stmt>())
        {
            pattern = f->variable;
            keyword = "for";
        }
        if (!ast::is_valid(pattern))
            return;
        auto const* n = a.at(pattern).node.try_as<ast::name>();
        if (n == nullptr)
            return;
        auto type = type_at(file, pattern);
        if (!check::is_valid(type))
            type = use_type;
        r.declaring_file = file;
        r.declared = n->where;
        r.signature = cc::format("{} {} : {}", keyword, files[file].file.text_of(n->where), type_name(type));
    }

    void resolve_target(reference& r, i32 file, check::target const& t, check::type_id use_type) const
    {
        auto const name = cc::string(files[file].file.text_of(r.where));
        switch (t.kind)
        {
        case check::target_kind::none:
        case check::target_kind::array_filled:
        case check::target_kind::undefined_value:
        case check::target_kind::texel_conversion:
            return;
        case check::target_kind::local:
            resolve_local(r, file, ast::stmt_id(t.index), use_type);
            return;
        case check::target_kind::parameter:
            r.declaring_file = file;
            r.declared = field_name(file, ast::field_id(t.index));
            r.signature = cc::format("{} : {}", name, type_name(use_type));
            return;
        case check::target_kind::symbol:
        case check::target_kind::overload:
        case check::target_kind::constructor:
            if (check::is_valid(t.symbol))
                resolve_symbol(r, t.symbol);
            return;
        case check::target_kind::field:
            if (check::is_valid(t.symbol))
                resolve_member(r, t.symbol, t.index, false);
            return;
        case check::target_kind::binding_member:
            if (check::is_valid(t.symbol))
                resolve_member(r, t.symbol, t.index, true);
            return;
        case check::target_kind::enum_case:
            if (check::is_valid(t.symbol))
                resolve_case(r, t.symbol, t.index);
            return;
        case check::target_kind::receiver:
            r.signature = cc::format("self : {}", type_name(use_type));
            return;
        case check::target_kind::array_length:
            r.signature = cc::format("length : {}", type_name(use_type));
            return;
        case check::target_kind::swizzle:
        case check::target_kind::generic_value:
            r.signature = cc::format("{} : {}", name, type_name(use_type));
            return;
        }
    }

    /// `where` as a declaration of file `file`, which no expression names: a symbol, a member, a parameter or a case.
    void resolve_declaration(reference& r, i32 file) const
    {
        auto const& a = files[file].ast;
        for (auto i = isize(0); i < m.symbols.size(); ++i)
        {
            auto const& s = m.symbols[i];
            if (s.file != file || s.role == check::function_role::constructor || declared_name_of(s) != r.where)
                continue;
            resolve_symbol(r, check::symbol_id(i));
            return;
        }
        for (auto i = isize(0); i < m.symbols.size(); ++i)
        {
            auto const& s = m.symbols[i];
            if (s.file != file)
                continue;
            auto const id = check::symbol_id(i);
            auto const is_binding = s.kind == check::symbol_kind::binding;
            if (s.kind == check::symbol_kind::structure || is_binding)
            {
                auto members = cc::span<check::member_info const>();
                if (is_binding && s.info >= 0 && s.info < m.bindings.size())
                    members = m.at(m.bindings[s.info].members);
                else if (auto const* t = type_of_symbol(id); !is_binding && t != nullptr)
                    members = m.at(t->members);
                for (auto k = isize(0); k < members.size(); ++k)
                    if (field_name(file, members[k].field) == r.where)
                    {
                        resolve_member(r, id, i32(k), is_binding);
                        return;
                    }
            }
            if (s.kind == check::symbol_kind::enumeration)
                if (auto const* t = type_of_symbol(id))
                    for (auto k = isize(0); k < isize(t->cases.count); ++k)
                    {
                        auto const decl = m.at(t->cases)[k].declaration;
                        if (ast::is_valid(decl) && ast::index_of(decl) < a.decls.size()
                            && name_of_decl(a.at(decl)) == r.where)
                        {
                            resolve_case(r, id, i32(k));
                            return;
                        }
                    }
        }
        // a parameter, which is a field no symbol has as a member
        for (auto const& f : a.fields)
            if (f.name == r.where)
            {
                auto const type = type_at(file, f.type);
                r.declaring_file = file;
                r.declared = f.name;
                auto const name = files[file].file.text_of(f.name);
                r.signature = check::is_valid(type) ? cc::format("{} : {}", name, type_name(type)) : cc::string(name);
                return;
            }
    }
};

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
        case check::symbol_kind::unsupported:
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
