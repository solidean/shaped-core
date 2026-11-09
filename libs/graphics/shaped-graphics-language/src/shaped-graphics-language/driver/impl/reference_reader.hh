#pragma once

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/driver/references.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

/// What `reference_at` and `completions_at` both read: declarations, signatures and doc comments.
namespace sgl::impl
{
[[nodiscard]] inline bool is_word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

[[nodiscard]] inline cc::string_view trimmed(cc::string_view s)
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
[[nodiscard]] inline source_span name_of_decl(ast::decl const& d)
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

    /// `fun dot(a: vec3, b: vec3) -> float`.
    /// `as_call` writes it as a call reads it instead: no `fun`, no `self`, and a constructor as `vec3(x: float, …)`.
    /// `params`, where given, receives where each parameter stands in the result.
    [[nodiscard]] cc::string function_signature(check::symbol const& s,
                                                bool as_call = false,
                                                cc::vector<parameter_label>* params = nullptr) const
    {
        auto const is_constructor = s.role == check::function_role::constructor;
        auto out = as_call ? cc::string() : cc::string("fun ");
        if (!is_constructor)
            out += owner_prefix(s);
        out += s.name;
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
                    if (as_call)
                    {
                        first = true;
                        continue;
                    }
                    out += p.is_mut ? "mut self" : "self";
                    continue;
                }
                auto const start = u32(out.size());
                out.appendf("{}{}: {}{}", p.is_named_only ? "." : "", p.name, p.is_mut ? "mut " : "", type_name(p.type));
                if (p.has_default && has_file(s.file) && ast::is_valid(p.field))
                {
                    auto const& own = files[s.file];
                    auto const def = own.ast.at(p.field).default_value;
                    if (ast::is_valid(def) && is_valid(own.ast.at(def).form))
                        out.appendf(" = {}", own.file.text_of(own.file.at(own.ast.at(def).form).where));
                }
                if (params != nullptr)
                    params->push_back({.offset = start, .length = u32(out.size()) - start});
            }
            out += ")";
        }
        if (!is_constructor && f.result != check::checked_module::void_type && check::is_valid(f.result))
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
        case check::symbol_kind::alias:
            return s.type == check::type_id::none ? cc::string("type ") + s.name
                                                  : cc::format("type {} = {}", s.name, type_name(s.type));
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

    /// The other functions of `id`'s overload set: its name, scope and role, the symbol itself left out.
    /// A program's function of a prelude function's name hides it rather than joining its set.
    [[nodiscard]] cc::vector<check::symbol_id> overloads_of(check::symbol_id id) const
    {
        auto out = cc::vector<check::symbol_id>();
        auto const& s = m.at(id);
        if (s.kind != check::symbol_kind::function)
            return out;
        auto const is_prelude = s.file < m.prelude_file_count();
        for (auto i = isize(0); i < m.symbols.size(); ++i)
        {
            auto const& o = m.symbols[i];
            if (check::symbol_id(i) != id && o.kind == check::symbol_kind::function && o.name == s.name
                && o.owner == s.owner && o.role == s.role && o.operator_spelling == s.operator_spelling
                && (o.file < m.prelude_file_count()) == is_prelude && m.is_reached_file(o.file))
                out.push_back(check::symbol_id(i));
        }
        return out;
    }

    void resolve_symbol(reference& r, check::symbol_id id) const
    {
        auto const& s = m.at(id);
        r.declaring_file = s.file;
        r.declared = declared_name_of(s);
        r.signature = symbol_signature(s);
        r.overloads.clear();
        if (s.role != check::function_role::constructor)
            for (auto const o : overloads_of(id))
                r.overloads.push_back(symbol_signature(m.at(o)));
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
        case check::target_kind::buffer_length:
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
} // namespace sgl::impl
