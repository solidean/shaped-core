#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
/// One `use` line as written, before any module is known.
struct use_line
{
    /// The module's whole name, its dots included.
    cc::string module;
    /// The name it binds: the alias, or the last part of the module's name.
    cc::string name;
    source_span where;
};

/// What one file's `module` and `use` lines say.
struct file_lines
{
    /// The module its `module` line names; empty without one.
    cc::string module;
    cc::vector<use_line> uses;
};

/// The module name a path of `module` or `use` writes, `a.b.c` as one name; empty for any other expression, which the
/// AST pass reported.
cc::string path_of(module_file const& f, ast::expr_id e)
{
    auto const& node = f.ast.at(e).node;
    if (auto const* const n = node.try_as<ast::name>())
        return cc::string(f.file.text_of(n->where));
    if (auto const* const m = node.try_as<ast::member>())
    {
        auto head = ast::is_valid(m->object) ? path_of(f, m->object) : cc::string();
        if (head.empty())
            return {};
        return cc::format("{}.{}", head, f.file.text_of(m->name));
    }
    return {};
}

file_lines read_lines(module_file const& f)
{
    auto result = file_lines();
    auto const& ast = f.ast;
    for (auto const id : ast.at(ast.declarations))
    {
        auto const& d = ast.at(id);
        auto const where = f.file.at(d.form).where;
        if (auto const* const m = d.node.try_as<ast::module_decl>())
        {
            // a `module` line that lost its path was reported by the AST pass
            if (ast::is_valid(m->path))
                result.module = path_of(f, m->path);
        }
        else if (auto const* const u = d.node.try_as<ast::use_decl>())
        {
            if (!ast::is_valid(u->path))
                continue;
            auto line = use_line{.module = path_of(f, u->path), .where = where};
            if (line.module.empty())
                continue;
            // CHK-347: `use a.b` binds `b`, the last part of the module's name
            auto const dot = line.module.rfind('.');
            line.name = !u->alias.empty() ? cc::string(f.file.text_of(u->alias))
                      : dot >= 0          ? cc::string(cc::string_view(line.module).subview(dot + 1))
                                          : line.module;
            result.uses.push_back(cc::move(line));
        }
    }
    return result;
}

/// A `use` from a file of one module of another, which the cycle check walks.
struct use_edge
{
    i32 to = -1;
    /// A position in the library, or -1 for the program.
    i32 file = -1;
    source_span where;
};

/// What the plan found wrong, at a position in the library or -1 for the program, until the order of the files is
/// known.
struct pending_diagnostic
{
    diagnostic_kind kind = {};
    i32 file = -1;
    source_span where;
    cc::string detail;
};
} // namespace

cc::string impl::module_name_of(module_file const& f)
{
    for (auto const id : f.ast.at(f.ast.declarations))
        if (auto const* const m = f.ast.at(id).node.try_as<ast::module_decl>(); m != nullptr && ast::is_valid(m->path))
            return path_of(f, m->path);
    return {};
}

module_plan impl::plan_modules(cc::span<module_file const> library, module_file program)
{
    auto lines = cc::vector<file_lines>();
    for (auto const& f : library)
        lines.push_back(read_lines(f));
    auto const program_lines = read_lines(program);

    // CHK-346: the files of a module are the library files whose `module` line names it, in the library's order
    auto files_of = cc::map<cc::string, cc::vector<i32>>();
    for (auto i = i32(0); i < i32(lines.size()); ++i)
        if (!lines[i].module.empty())
            files_of[lines[i].module].push_back(i);

    auto pending = cc::vector<pending_diagnostic>();
    auto modules = cc::vector<cc::string>();
    auto module_files = cc::vector<cc::vector<i32>>();
    auto edges = cc::vector<cc::vector<use_edge>>();
    auto index_of_module = cc::map<cc::string, i32>();
    auto const add_module = [&](cc::string_view name) -> i32
    {
        auto const index = i32(modules.size());
        modules.push_back(cc::string(name));
        module_files.push_back({});
        edges.push_back({});
        if (!name.empty())
        {
            index_of_module[cc::string(name)] = index;
            if (auto const* const files = files_of.get_ptr(name))
                module_files.back() = *files;
        }
        return index;
    };

    // The program's own module is module 0, and a program whose `module` line names one of the library joins it.
    add_module(program_lines.module);

    auto library_uses = cc::map<i32, cc::vector<module_use>>();
    auto program_uses = cc::vector<module_use>();
    auto const read_uses = [&](file_lines const& l, i32 file, i32 module)
    {
        auto& uses = file < 0 ? program_uses : library_uses[file];
        for (auto const& u : l.uses)
        {
            // CHK-347: a module's own names are seen unqualified already
            if (!modules[module].empty() && u.module == modules[module])
            {
                pending.push_back({.kind = diagnostic_kind::use_of_own_module,
                                   .file = file,
                                   .where = u.where,
                                   .detail = cc::format("this file is of module {}", u.module)});
                continue;
            }
            if (!files_of.contains(u.module))
            {
                pending.push_back(
                    {.kind = diagnostic_kind::unknown_module, .file = file, .where = u.where, .detail = u.module});
                continue;
            }
            auto is_taken = false;
            for (auto const& earlier : uses)
                is_taken = is_taken || earlier.name == u.name;
            if (is_taken)
            {
                pending.push_back({.kind = diagnostic_kind::duplicate_declaration,
                                   .file = file,
                                   .where = u.where,
                                   .detail = cc::format("{} names a module of this file already", u.name)});
                continue;
            }
            auto const* const known = index_of_module.get_ptr(u.module);
            auto const target = known != nullptr ? *known : add_module(u.module);
            uses.push_back({.name = u.name, .module = target, .where = u.where});
            edges[module].push_back({.to = target, .file = file, .where = u.where});
        }
    };

    // Each module's files, in the order the modules were reached; a module added on the way is read in its turn.
    for (auto module = i32(0); module < i32(modules.size()); ++module)
    {
        // indexed afresh each time: reading may add modules, which moves `module_files` and every list in it
        for (auto i = isize(0); i < module_files[module].size(); ++i)
            read_uses(lines[module_files[module][i]], module_files[module][i], module);
        if (module == 0)
            read_uses(program_lines, -1, 0);
    }

    // CHK-349: a loop of `use`s among modules, reported once per edge that closes one
    auto state = cc::vector<u8>::create_filled(modules.size(), 0);
    auto path = cc::vector<i32>();
    auto const visit = [&](auto const& self, i32 module) -> void
    {
        state[module] = 1;
        path.push_back(module);
        for (auto const& e : edges[module])
        {
            if (state[e.to] == 1)
            {
                auto loop = cc::string();
                auto is_inside = false;
                for (auto const m : path)
                {
                    is_inside = is_inside || m == e.to;
                    if (is_inside)
                        loop.appendf("{} -> ", modules[m]);
                }
                loop += modules[e.to];
                pending.push_back(
                    {.kind = diagnostic_kind::module_cycle, .file = e.file, .where = e.where, .detail = cc::move(loop)});
            }
            else if (state[e.to] == 0)
                self(self, e.to);
        }
        path.remove_back();
        state[module] = 2;
    };
    visit(visit, 0);

    // The files of the modules the program reaches, then those of its own module, then the program.
    auto plan = module_plan();
    auto position_of = cc::map<i32, i32>();
    auto const place = [&](i32 module)
    {
        for (auto const file : module_files[module])
        {
            position_of[file] = i32(plan.library_order.size());
            plan.library_order.push_back(file);
            plan.file_module.push_back(module);
            auto* const uses = library_uses.get_ptr(file);
            plan.uses.push_back(uses != nullptr ? cc::move(*uses) : cc::vector<module_use>());
        }
    };
    for (auto module = i32(1); module < i32(modules.size()); ++module)
        place(module);
    place(0);
    plan.file_module.push_back(0);
    plan.uses.push_back(cc::move(program_uses));
    plan.modules = cc::move(modules);

    auto const program_position = i32(plan.library_order.size());
    for (auto& d : pending)
        plan.diagnostics.push_back({
            .what = {.kind = d.kind, .level = default_severity_of(d.kind), .where = d.where},
            .file = d.file < 0 ? program_position : position_of[d.file],
            .detail = cc::move(d.detail),
        });
    return plan;
}
