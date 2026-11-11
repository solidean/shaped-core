#include "check.hh"

#include <clean-core/common/hash.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>
#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/check/impl/checker.hh>
#include <shaped-graphics-language/check/impl/relocate.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// ---- the driver -----------------------------------------------------------------------------------------------------

void checker::run(resume_point from)
{
    run_from_files = from.files;
    bodies_read_from = bodies_read.size();
    run_start = sizes_of(*this);
    if (from.files == 0)
    {
        out.types.push_back({.kind = type_kind::error});
        out.types.push_back({.kind = type_kind::void_});
    }
    // A checked layer leaves the scope of its empty program behind, which this check's own module replaces.
    // A layer of modules keeps theirs, since their files are declared already and are not declared again.
    if (from.files > prelude_count)
    {
        module_scopes[0] = {};
        // a layer of the chain adds modules behind the ones it continues
        while (module_scopes.size() < modules.size())
            module_scopes.push_back({});
    }
    else
    {
        module_scopes = {};
        for (auto i = isize(0); i < modules.size(); ++i)
            module_scopes.push_back({});
    }
    for (auto file = from.files; file < i32(files.size()); ++file)
    {
        auto const count = ast_of(file).exprs.size();
        out.files.push_back({
            .type_of = cc::vector<type_id>::create_filled(count, type_id::none),
            .target_of = cc::vector<target>::create_filled(count, target{}),
            .call_of = cc::vector<i32>::create_filled(count, -1),
        });
        file_features.push_back({});
    }

    for (auto file = from.files; file < i32(files.size()); ++file)
        declare_file(file);
    declare_constructors(from.symbols);
    merge_scopes();
    attach_extensions(from.extensions);

    // Source order is only the order of the first demand: whatever a symbol needs is compiled from inside it.
    for (auto i = from.symbols; i < out.symbols.size(); ++i)
        if (out.symbols[i].state == symbol_state::untouched)
            compile(symbol_id(i));

    judge_redeclarations(from.symbols);
    judge_option_values();

    // A default is checked where it is declared, once, and a call binds against the signature alone (CHK-243).
    for (auto i = from.symbols; i < out.symbols.size(); ++i)
        if (out.symbols[i].kind == symbol_kind::function && out.symbols[i].state == symbol_state::checked)
            check_defaults(symbol_id(i));
    // what a later link of a chain may name of this one, before a body adds anything
    if (records_interface)
    {
        interface_end = sizes_of(*this);
        interface_digest = interface_hash(*this, run_start, interface_end);
    }

    // A call needs a signature only, so each body is checked once, after every signature is known.
    // That is what lets a function call one declared below it.
    // The exception checked its body already: an arrow body without `-> T`, whose signature is not known before.
    for (auto i = from.symbols; i < out.symbols.size(); ++i)
        if (out.symbols[i].kind == symbol_kind::function && out.symbols[i].state == symbol_state::checked)
            check_body(symbol_id(i));

    // A test in a body that was never checked, a function whose signature failed or a test inside a test, is found
    // nowhere else, and a test is run or fails: it is never left out (CHK-224).
    add_unregistered_tests(from.files, from.tests);

    // Last, since a test in a function body is found while that body is checked (CHK-224).
    for (auto i = from.tests; i < out.tests.size(); ++i)
        check_test(i32(i));
    warn_uninstantiated_generics(from.symbols);

    judge_wide_literals(from.wide_literals);
    find_recursion(from.symbols, from.notes, from.calls);

    // Every binding and body is checked by now, so what each entry point needs and declares is known.
    for (auto i = from.symbols; i < out.symbols.size(); ++i)
        if (out.symbols[i].kind == symbol_kind::function && out.symbols[i].state == symbol_state::checked)
            judge_entry_features(symbol_id(i));
    // CHK-487: what a task stage hands on is known once its body is, and its pipeline's mesh stage takes exactly it
    judge_mesh_payloads();

    index_builtin_symbols(from.symbols);
    instantiate_generics(from.call_records, from.types);
    prepare_target_layouts();
    // CHK-350: the entry points are the program's; a module the program uses is a library, whose own are not built
    for (auto i = from.symbols; i < out.symbols.size(); ++i)
        if (out.symbols[i].kind == symbol_kind::function && out.symbols[i].state == symbol_state::checked
            && (is_prelude_file(out.symbols[i].file) || out.symbols[i].file == program_file()))
            flatten_entry_point(symbol_id(i));
    flatten_metal_traversals();
    for (auto i = from.tests; i < out.tests.size(); ++i)
        flatten_test(i32(i));
    place_layout_bindings(from.symbols);
    // after flattening, which finds what a body's calls need (CHK-322)
    report_unused_requires();
    judge_footprint_pins();
    judge_trace_graphs();
    judge_callables();
    judge_indices();
    merge_entry_diagnostics();
    judge_expecting_in_pipelines();
    drop_expecting_entry_points();
}

void checker::declare_file(i32 file)
{
    auto const& ast = ast_of(file);
    for (auto const decl : ast.at(ast.declarations))
        declare(file, decl);
}

void checker::add_symbol(symbol s, source_span name_where)
{
    auto const id = symbol_id(out.symbols.size());
    auto const file = s.file;
    auto const is_function = s.kind == symbol_kind::function;
    s.is_shadowable = !ast::is_valid(s.declaration) || is_shadowable_by(file, ast_of(file).at(s.declaration).attributes);
    auto const name = s.name;
    auto const spelling = s.operator_spelling;
    out.symbols.push_back(cc::move(s));

    if (!spelling.empty())
    {
        operators[spelling].push_back(id);
        if (is_prelude_file(file))
            prelude_operators[spelling].push_back(id);
        return;
    }

    // CHK-12 holds within one scope; a module's may shadow the prelude's.
    // A struct shares its name with functions, its constructors among them, and stands in front of them (CHK-240).
    auto& declared = is_prelude_file(file) ? prelude_names[name] : module_scopes[file_module[file]][name];
    auto const is_struct = out.at(id).kind == symbol_kind::structure;
    auto const is_allowed
        = declared.empty() || (is_function && is_overload_set(declared)) || (is_struct && is_all_functions(declared));
    if (!is_allowed)
    {
        // The later declaration is compiled like any other and no lookup finds it.
        report(diagnostic_kind::duplicate_declaration, file, name_where, name);
        return;
    }
    if (is_struct)
    {
        declared.insert_at(0, id);
        return;
    }
    declared.push_back(id);
}

void checker::declare_members(symbol_id owner, i32 file, ast::range_of<ast::decl_id> members)
{
    for (auto const member : ast_of(file).at(members))
    {
        auto const& d = ast_of(file).at(member);
        // An extension inside a type's block is meant to extend the type it names, seen only inside that block.
        auto const* const ef = d.node.try_as<ast::fun_decl>();
        auto const* const ep = d.node.try_as<ast::property_decl>();
        if ((ef != nullptr && !ef->extended_type.empty()) || (ep != nullptr && !ep->extended_type.empty()))
        {
            unsupported(file, ef != nullptr ? ef->extended_type : ep->extended_type,
                        "an extension inside a type's block");
            continue;
        }
        if (auto const* const f = d.node.try_as<ast::fun_decl>(); f != nullptr && !f->name.empty())
            add_member({.file = file,
                        .declaration = member,
                        .kind = symbol_kind::function,
                        .name = cc::string(text_of(file, f->name)),
                        .role = f->receiver == ast::receiver_kind::none ? function_role::static_ : function_role::method,
                        .owner = owner},
                       f->name);
        else if (auto const* const p = d.node.try_as<ast::property_decl>(); p != nullptr && !p->name.empty())
            add_member({.file = file,
                        .declaration = member,
                        .kind = symbol_kind::function,
                        .name = cc::string(text_of(file, p->name)),
                        .role = function_role::property,
                        .owner = owner},
                       p->name);
    }
}

cc::string_view checker::member_kind_of(symbol_id owner, cc::string_view name) const
{
    auto const& o = out.at(owner);
    auto const& ast = ast_of(o.file);
    auto const& node = ast.at(o.declaration).node;
    auto const* const s = node.try_as<ast::struct_decl>();
    auto const* const e = node.try_as<ast::enum_decl>();
    auto const members = s != nullptr ? s->members : e != nullptr ? e->members : ast::range_of<ast::decl_id>();
    for (auto const member : ast.at(members))
    {
        auto const& d = ast.at(member).node;
        if (auto const* const f = d.try_as<ast::field_decl>();
            f != nullptr && ast::is_valid(f->field) && text_of(o.file, ast.at(f->field).name) == name)
            return "field";
        if (auto const* const c = d.try_as<ast::enum_case_decl>(); c != nullptr && text_of(o.file, c->name) == name)
            return "case";
    }
    // CHK-386: every swizzle is a member as a field is, and a function of its name would never be reached
    auto const* const swizzled
        = s != nullptr ? find_attribute(o.file, ast.at(o.declaration).attributes, "swizzle") : nullptr;
    if (swizzled != nullptr && name.size() >= 1 && name.size() <= 4)
    {
        auto letters = cc::string();
        for (auto const member : ast.at(members))
            if (auto const* const f = ast.at(member).node.try_as<ast::field_decl>();
                f != nullptr && ast::is_valid(f->field) && text_of(o.file, ast.at(f->field).name).size() == 1)
                letters += text_of(o.file, ast.at(f->field).name);
        // CHK-409: the second alphabet's letters are members too, one alone included
        auto alias = cc::string_view();
        if (auto const arguments = ast.at(swizzled->arguments);
            arguments.size() == 1 && ast::is_valid(arguments[0].value))
            alias = text_of(o.file, span_of(o.file, arguments[0].value));
        for (auto const set : {cc::string_view(letters), alias})
        {
            if (set.empty() || (set.data() == letters.data() && name.size() < 2))
                continue;
            auto is_swizzle = true;
            for (auto const letter : name)
            {
                auto is_letter = false;
                for (auto const c : set)
                    is_letter = is_letter || c == letter;
                is_swizzle = is_swizzle && is_letter;
            }
            if (is_swizzle)
                return "swizzle";
        }
    }
    auto const* const scope = type_scopes.get_ptr(i32(index_of(owner)));
    auto const* const found = scope != nullptr ? scope->get_ptr(name) : nullptr;
    if (found == nullptr || found->empty())
        return {};
    return out.at(found->front()).role == function_role::property ? "property" : "function";
}

void checker::add_member(symbol s, source_span name_where)
{
    auto const id = symbol_id(out.symbols.size());
    auto const owner = s.owner;
    auto const file = s.file;
    auto const name = s.name;
    auto const kind = s.role == function_role::property ? cc::string_view("property") : cc::string_view("function");
    // CHK-238: a type scope holds one kind of thing per name, and functions of one name are an overload set
    auto const existing = member_kind_of(owner, name);
    out.symbols.push_back(cc::move(s));
    if (!existing.empty() && (existing != "function" || kind != "function"))
    {
        if (existing == kind)
            report(diagnostic_kind::duplicate_declaration, file, name_where, name);
        else
            report(diagnostic_kind::member_name_clash, file, name_where,
                   cc::format("{} is a {} of {} already", name, existing, out.at(owner).name));
        out.symbols.own(index_of(id)).state = symbol_state::failed;
        return;
    }
    type_scopes[i32(index_of(owner))][name].push_back(id);
}

void checker::attach_extensions(isize first)
{
    for (auto const& pending : cc::span<pending_extension const>(pending_extensions).subspan(first))
    {
        auto const file = pending.file;
        auto const& d = ast_of(file).at(pending.declaration).node;
        auto const* const f = d.try_as<ast::fun_decl>();
        auto const* const p = d.try_as<ast::property_decl>();
        auto const extended = f != nullptr ? f->extended_type : p->extended_type;
        auto const name_where = f != nullptr ? f->name : p->name;
        auto const type_name = text_of(file, extended);

        // CHK-237: `T` names a struct or an enum
        auto const* const found = seen_from(file, type_name);
        if (found == nullptr || found->empty())
        {
            report(diagnostic_kind::unknown_name, file, extended, type_name);
            continue;
        }
        auto const owner = found->front();
        auto const owner_kind = out.at(owner).kind;
        // an alias names its type only once it is compiled, and type scopes are made before anything is
        if (owner_kind == symbol_kind::alias)
        {
            unsupported(file, extended, "an extension through a type alias; extend the type it names");
            continue;
        }
        if (owner_kind != symbol_kind::structure && owner_kind != symbol_kind::enumeration)
        {
            report(diagnostic_kind::wrong_kind_of_name, file, extended,
                   cc::format("{} is no struct and no enum, and only a type has functions of its own", type_name));
            continue;
        }
        auto const role = p != nullptr                            ? function_role::property
                        : f->receiver == ast::receiver_kind::none ? function_role::static_
                                                                  : function_role::method;
        add_member({.file = file,
                    .declaration = pending.declaration,
                    .kind = symbol_kind::function,
                    .name = cc::string(text_of(file, name_where)),
                    .role = role,
                    .owner = owner},
                   name_where);
    }
}

cc::vector<symbol_id> checker::candidates_of(i32 file, cc::string_view name, type_id first) const
{
    auto result = cc::vector<symbol_id>();
    if (auto const* const named = seen_from(file, name))
        for (auto const id : *named)
            if (out.at(id).kind == symbol_kind::function)
                result.push_back(id);

    // CHK-247: the type scope of the first argument's type, extensions visible from `file` among it, and the functions
    // of the name visible where that type is declared.
    if (!is_valid(first) || index_of(first) >= out.types.size())
        return result;
    auto const& type = out.at(first);
    if ((type.kind != type_kind::structure && type.kind != type_kind::enumeration) || !is_valid(type.symbol))
        return result;
    auto const add = [&](symbol_id id)
    {
        auto is_known = false;
        for (auto const r : result)
            is_known = is_known || r == id;
        if (!is_known)
            result.push_back(id);
    };
    auto const* const scope = type_scopes.get_ptr(i32(index_of(type.symbol)));
    auto const* const members = scope != nullptr ? scope->get_ptr(name) : nullptr;
    if (members != nullptr)
        for (auto const id : *members)
            if (is_visible_from(file, id))
                add(id);
    // A file sees its own module's declarations already, so only a prelude type or another module's has a declaring
    // scope to add; for a prelude type it matters where the module shadows the name with something that is no function
    // (CHK-188).
    auto const declaring_file = out.at(type.symbol).file;
    auto const* const declared = is_prelude_file(declaring_file) ? prelude_names.get_ptr(name)
                               : is_prelude_file(file) || file_module[declaring_file] != file_module[file]
                                   ? module_scopes[file_module[declaring_file]].get_ptr(name)
                                   : nullptr;
    if (declared != nullptr)
        for (auto const id : *declared)
            if (out.at(id).kind == symbol_kind::function && is_visible_from(file, id))
                add(id);
    return result;
}

cc::string checker::name_seen_from(i32 file, symbol_id id) const
{
    auto const& s = out.at(id);
    if (s.file < 0 || is_prelude_file(s.file) || is_prelude_file(file) || file_module[s.file] == file_module[file])
        return cc::string(s.name);
    return cc::format("{}.{}", modules[file_module[s.file]], s.name);
}

i32 checker::module_named(i32 file, ast::expr_id expr, function_scope const* scope) const
{
    if (!ast::is_valid(expr) || is_prelude_file(file))
        return -1;
    auto const* const n = ast_of(file).at(expr).node.try_as<ast::name>();
    if (n == nullptr)
        return -1;
    auto const text = text_of(file, n->where);
    if ((scope != nullptr && scope->find_local(text) != nullptr) || is_type_parameter_name(text))
        return -1;
    return used_module(file, text);
}

cc::vector<symbol_id> const* checker::symbols_named(i32 file,
                                                    ast::expr_id expr,
                                                    function_scope const* scope,
                                                    bool* is_qualified) const
{
    if (is_qualified != nullptr)
        *is_qualified = false;
    if (!ast::is_valid(expr))
        return nullptr;
    auto const& node = ast_of(file).at(expr).node;
    cc::vector<symbol_id> const* found = nullptr;
    if (auto const* const n = node.try_as<ast::name>())
    {
        auto const text = text_of(file, n->where);
        if ((scope != nullptr && scope->find_local(text) != nullptr) || is_type_parameter_name(text))
            return nullptr;
        found = seen_from(file, text);
    }
    else if (auto const* const m = node.try_as<ast::member>())
    {
        // CHK-348: what a module declares, and none of the prelude it sees
        auto const module = module_named(file, m->object, scope);
        if (module < 0 || m->name.empty())
            return nullptr;
        if (is_qualified != nullptr)
            *is_qualified = true;
        found = module_scopes[module].get_ptr(text_of(file, m->name));
    }
    return found != nullptr && !found->empty() ? found : nullptr;
}

void checker::judge_redeclarations(isize first)
{
    // Two overloads can clash only when their parameters hash alike, so each meets only the earlier ones that do.
    auto const signature_hash = [&](symbol const& s) -> u64
    {
        auto h = u64(0);
        for (auto const& p : out.at(out.functions[s.info].parameters))
            h = cc::make_hash(h, u32(p.type), p.name, p.is_named_only);
        return h;
    };
    auto const judge = [&](cc::span<symbol_id const> set)
    {
        auto is_new = false;
        for (auto const id : set)
            is_new = is_new || index_of(id) >= first;
        if (set.size() < 2 || !is_new)
            return;
        auto earlier = cc::map<u64, cc::vector<isize>>();
        for (auto i = isize(0); i < set.size(); ++i)
        {
            auto const& candidate = out.at(set[i]);
            if (candidate.kind != symbol_kind::function || candidate.info < 0)
                continue;
            auto& alike = earlier[signature_hash(candidate)];
            for (auto const j : alike)
            {
                auto const& a = out.at(set[j]);
                auto const& b = out.at(set[i]);
                if (a.kind != symbol_kind::function || b.kind != symbol_kind::function || a.info < 0 || b.info < 0
                    || a.state != symbol_state::checked || b.state != symbol_state::checked)
                    continue;
                auto const pa = out.at(out.functions[a.info].parameters);
                auto const pb = out.at(out.functions[b.info].parameters);
                auto is_same = pa.size() == pb.size();
                for (auto k = isize(0); is_same && k < pa.size(); ++k)
                    is_same = pa[k].type == pb[k].type && pa[k].name == pb[k].name
                           && pa[k].is_named_only == pb[k].is_named_only;
                if (!is_same)
                    continue;
                // the later one: a synthesized constructor is the struct's, which stands before any function of it
                auto const later = b.role == function_role::constructor ? set[j] : set[i];
                auto const& l = out.at(later);
                auto const& decl = ast_of(l.file).at(l.declaration).node;
                auto where = span_of(l.file, l.declaration);
                if (auto const* const f = decl.try_as<ast::fun_decl>())
                    where = f->name;
                report(diagnostic_kind::duplicate_declaration, l.file, where,
                       cc::format("{} has these parameters already", l.name));
                // No call could choose between the two, so the later one is out of every lookup.
                out.symbols.own(index_of(later)).state = symbol_state::failed;
            }
            alike.push_back(i);
        }
    };
    // a set a lower layer holds unchanged has no symbol of this layer, so only this layer's own entries can hold one
    for (auto const& [name, ids] : prelude_names.own_entries())
        judge(ids);
    for (auto const& scope : module_scopes)
        for (auto const& [name, ids] : scope)
            judge(ids);
    for (auto const& [owner, scope] : type_scopes.own_entries())
        for (auto const& [name, ids] : scope)
            judge(ids);
}

bool checker::is_overload_set(cc::span<symbol_id const> ids) const
{
    for (auto i = isize(0); i < ids.size(); ++i)
    {
        auto const kind = out.at(ids[i]).kind;
        if (kind != symbol_kind::function && !(i == 0 && kind == symbol_kind::structure))
            return false;
    }
    return true;
}

void checker::declare_constructors(isize first)
{
    auto const count = out.symbols.size();
    for (auto i = first; i < count; ++i)
    {
        auto const s = out.symbols[i];
        if (s.kind != symbol_kind::structure)
            continue;
        auto const& decl = ast_of(s.file).at(s.declaration).node.as<ast::struct_decl>();
        // A struct nobody can name has no constructor anybody could call, and an opaque one has none (CHK-33).
        if (decl.is_opaque || decl.name.empty())
            continue;
        // Only a struct a lookup finds has a constructor there: a duplicate is found by nothing.
        auto const* const found = is_prelude_file(s.file) ? prelude_names.get_ptr(s.name)
                                                          : module_scopes[file_module[s.file]].get_ptr(s.name);
        if (found == nullptr || found->empty() || found->front() != symbol_id(i))
            continue;
        add_symbol({.file = s.file,
                    .declaration = s.declaration,
                    .kind = symbol_kind::function,
                    .name = s.name,
                    .role = function_role::constructor,
                    .owner = symbol_id(i)},
                   decl.name);
    }
}

bool checker::is_all_functions(cc::span<symbol_id const> ids) const
{
    auto result = true;
    for (auto const id : ids)
        result = result && out.at(id).kind == symbol_kind::function;
    return result;
}

bool checker::is_internal(symbol_id id) const
{
    auto const& s = out.at(id);
    return is_prelude_file(s.file) && ast::is_valid(s.declaration)
        && find_attribute(s.file, ast_of(s.file).at(s.declaration).attributes, "internal") != nullptr;
}

void checker::merge_scopes()
{
    // CHK-323: an `@internal` symbol of the prelude is the prelude's alone, and no lookup from a module finds it
    if (!has_prelude_visible)
    {
        for (auto const& [name, ids] : prelude_names)
            for (auto const id : ids)
                if (!is_internal(id))
                    prelude_visible[name].push_back(id);
        has_prelude_visible = true;
    }
    merged_scopes = {};
    for (auto const& scope : module_scopes)
    {
        auto& names = merged_scopes.emplace_back();
        // only the names the module declares: every other one a module's file finds in `prelude_visible`
        for (auto const& [name, ids] : scope)
            if (auto const* const prelude = prelude_visible.get_ptr(name))
                names[name] = *prelude;
        merge_scope(names, scope);
    }

    // CHK-347: a `use` line's name and a declaration of the file's module, or of the prelude, would be one name for two
    // things
    // A refused `use` binds nothing, so the name keeps meaning the declaration.
    for (auto file = prelude_count; file < i32(files.size()); ++file)
    {
        auto kept = cc::vector<module_use>();
        for (auto& u : uses[file])
        {
            auto const* const found = module_scopes[file_module[file]].get_ptr(u.name);
            auto is_prelude_name = false;
            if (auto const* const prelude = prelude_names.get_ptr(u.name))
                for (auto const id : *prelude)
                    is_prelude_name = is_prelude_name || !is_internal(id);
            if (found != nullptr && !found->empty())
                report(diagnostic_kind::duplicate_declaration, file, u.where,
                       cc::format("{} names a module here, and a declaration of this module", u.name));
            else if (is_prelude_name)
                report(diagnostic_kind::duplicate_declaration, file, u.where,
                       cc::format("{} names a module here, and a declaration of the prelude", u.name));
            else
                kept.push_back(cc::move(u));
        }
        uses[file] = cc::move(kept);
    }
}

void checker::merge_scope(cc::map<cc::string, cc::vector<symbol_id>>& names,
                          cc::map<cc::string, cc::vector<symbol_id>> const& scope)
{
    for (auto const& [name, ids] : scope)
    {
        auto& seen = names[name];
        // Two overload sets are one, a struct's constructors among them (CHK-189).
        // Anything else of the user file hides what the prelude has of that name.
        if (!is_overload_set(seen) || !is_all_functions(ids))
        {
            // CHK-220: unless the prelude's may not be hidden, which keeps the prelude's and reports the user's.
            auto is_sealed = false;
            for (auto const s : seen)
                is_sealed = is_sealed || !out.at(s).is_shadowable;
            if (is_sealed)
            {
                for (auto i = isize(0); i < ids.size(); ++i)
                {
                    // a struct's constructor has the struct's declaration, which is reported once
                    auto const s = ids[i];
                    auto is_reported = false;
                    for (auto j = isize(0); j < i; ++j)
                        is_reported = is_reported || out.at(ids[j]).declaration == out.at(s).declaration;
                    if (is_reported)
                        continue;
                    report(diagnostic_kind::shadows_unshadowable, out.at(s).file,
                           span_of(out.at(s).file, out.at(s).declaration),
                           cc::format("{} is @shadowable(false) in the prelude", name));
                }
                continue;
            }
            seen.clear();
        }
        seen.push_back_range(ids);
    }
}

void checker::declare(i32 file, ast::decl_id decl)
{
    auto const& d = ast_of(file).at(decl);
    auto const named = [&](symbol_kind kind, source_span name)
    { return symbol{.file = file, .declaration = decl, .kind = kind, .name = text_of(file, name)}; };
    d.node.visit(
        [&](ast::fun_decl const& f)
        {
            // a function that lost its name was reported by the AST pass, and nothing can call it
            if (f.name.empty())
                return;
            if (!f.extended_type.empty())
            {
                pending_extensions.push_back({.file = file, .declaration = decl});
                return;
            }
            auto s = named(symbol_kind::function, f.name);
            if (auto const* const a = find_attribute(file, d.attributes, "operator"))
            {
                auto const arguments = ast_of(file).at(a->arguments);
                auto spelling = cc::string_view();
                if (arguments.size() == 1 && arguments[0].name.empty() && !arguments[0].is_splat
                    && ast::is_valid(arguments[0].value))
                {
                    auto const& value = ast_of(file).at(arguments[0].value);
                    auto const* const literal = value.node.try_as<ast::literal>();
                    if (literal != nullptr && literal->kind == ast::literal_kind::quoted)
                        spelling = text_of(file, span_of(file, value.form));
                }
                if (spelling.size() >= 2 && spelling.starts_with('"') && spelling.ends_with('"'))
                    spelling = spelling.subview({.offset = 1, .size = spelling.size() - 2});
                else
                    spelling = {};

                if (spelling.empty())
                {
                    report(diagnostic_kind::invalid_attribute_arguments, file, a->name,
                           "@operator takes one quoted operator, as in @operator(\"*\")");
                    // Neither its name nor an operator finds it: the attribute says its name is hidden.
                    s.state = symbol_state::failed;
                    out.symbols.push_back(cc::move(s));
                    return;
                }
                s.operator_spelling = spelling;
            }
            add_symbol(cc::move(s), f.name);
        },
        [&](ast::struct_decl const& s)
        {
            auto const owner = symbol_id(out.symbols.size());
            if (!s.name.empty())
            {
                add_symbol(named(symbol_kind::structure, s.name), s.name);
                declare_members(owner, file, s.members);
            }
            add_member_tests(file, s.members, cc::format("struct {}", text_of(file, s.name)));
        },
        [&](ast::binding_decl const& b)
        {
            if (!b.name.empty())
                add_symbol(named(symbol_kind::binding, b.name), b.name);
        },
        // what the two lines name was read before any file was declared (`plan_modules`)
        [&](ast::module_decl const&) {}, //
        [&](ast::use_decl const&) {},
        [&](ast::require_decl const& r)
        {
            judge_attributes(file, d.attributes, {}, "a require");
            file_features[file] |= read_require(file, r, require_scope::file, symbol_id::none);
        },
        [&](ast::preamble_decl const& p)
        {
            judge_attributes(file, d.attributes, {}, "a preamble");
            declare_preamble(file, p);
        },
        [&](ast::enum_decl const& e)
        {
            auto const owner = symbol_id(out.symbols.size());
            if (!e.name.empty())
            {
                add_symbol(named(symbol_kind::enumeration, e.name), e.name);
                declare_members(owner, file, e.members);
            }
            add_member_tests(file, e.members, cc::format("enum {}", text_of(file, e.name)));
        },
        [&](ast::type_decl const& t)
        {
            if (!t.name.empty())
                add_symbol(named(symbol_kind::alias, t.name), t.name);
        },
        [&](ast::const_decl const& c)
        {
            if (!c.name.empty())
                add_symbol(named(symbol_kind::constant, c.name), c.name);
        },
        [&](ast::sampler_decl const& s)
        {
            // EMIT-133: a module's sampler takes an index after the samplers declared before it, whatever their module
            if (!s.name.empty())
                add_symbol(named(symbol_kind::sampler, s.name), s.name);
        },
        [&](ast::pipeline_decl const& p)
        {
            // Without a name it is the file's pipeline, named `pipeline`; a second one is a duplicate like any other.
            auto s = symbol{.file = file, .declaration = decl, .kind = symbol_kind::pipeline, .name = "pipeline"};
            if (!p.name.empty())
                s.name = text_of(file, p.name);
            add_symbol(cc::move(s), p.name.empty() ? span_of(file, decl) : p.name);
        },
        [&](ast::notation_decl const&) { unsupported(file, span_of(file, decl), "notation"); },
        [&](ast::test_decl const&) { add_test(file, decl, ""); },
        // A member line at module level and an `invalid` declaration were reported by the AST pass.
        [&](ast::field_decl const&) {}, //
        [&](ast::property_decl const& p)
        {
            if (!p.extended_type.empty())
                pending_extensions.push_back({.file = file, .declaration = decl});
        },
        [&](ast::enum_case_decl const&) {}, [&](ast::invalid_decl const&) {});
}

symbol_state checker::demand(symbol_id id, i32 file, source_span where)
{
    auto const state = out.at(id).state;
    if (state == symbol_state::untouched)
    {
        compile(id);
        return out.at(id).state;
    }
    if (state != symbol_state::in_compilation)
        return state;

    // The loop starts where the demanded symbol entered compilation and closes on it again.
    auto loop = cc::string();
    auto is_inside = false;
    for (auto const s : compiling)
    {
        is_inside = is_inside || s == id;
        if (is_inside)
            loop.appendf("{} -> ", out.at(s).name);
    }
    loop += out.at(id).name;
    report(diagnostic_kind::dependency_cycle, file, where, cc::move(loop));
    return symbol_state::in_compilation;
}

void checker::compile(symbol_id id)
{
    out.symbols.own(index_of(id)).state = symbol_state::in_compilation;
    compiling.push_back(id);

    // A symbol demanded from inside a binding's members is no member of it: it sees its own file's features alone.
    auto const outer_granted = granted;
    auto* const outer_used = used_features;
    granted = {};
    used_features = nullptr;
    // a symbol compiled from inside a generic one sees none of its type parameters
    auto outer_parameters = cc::move(type_parameter_names);
    type_parameter_names = {};

    switch (out.at(id).kind)
    {
    case symbol_kind::structure:
        compile_struct(id);
        break;
    case symbol_kind::enumeration:
        compile_enum(id);
        break;
    case symbol_kind::binding:
        compile_binding(id);
        break;
    case symbol_kind::function:
        if (out.at(id).role == function_role::constructor)
            compile_constructor(id);
        else if (out.at(id).role == function_role::property)
            compile_property(id);
        else
            compile_function(id);
        break;
    case symbol_kind::pipeline:
        compile_pipeline(id);
        break;
    case symbol_kind::constant:
        compile_const(id);
        break;
    case symbol_kind::sampler:
        compile_file_sampler(id);
        break;
    case symbol_kind::test:
        // A test's signature is made where it is found, and its body is checked with the others.
        break;
    case symbol_kind::alias:
        compile_alias(id);
        break;
    }

    granted = outer_granted;
    used_features = outer_used;
    type_parameter_names = cc::move(outer_parameters);
    compiling.remove_back();
    if (out.at(id).state == symbol_state::in_compilation)
        out.symbols.own(index_of(id)).state = symbol_state::checked;
}
