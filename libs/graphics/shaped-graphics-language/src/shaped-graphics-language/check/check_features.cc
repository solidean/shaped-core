#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/checker.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

// `require` and what an entry point needs of a device (the spec's checking file, "Features").

namespace
{
cc::string feature_list()
{
    auto result = cc::string();
    for (auto i = isize(0); i < k_feature_count; ++i)
        result.appendf("{}{}", i == 0 ? "" : ", ", k_feature_names[i]);
    return result;
}
} // namespace

feature_set checker::read_require(i32 file, ast::require_decl const& r, require_scope scope, symbol_id owner)
{
    auto const& ast = ast_of(file);
    auto result = feature_set();
    for (auto const e : ast.at(r.features))
    {
        // an argument that is no name was reported by the AST pass
        auto const* const n = ast.at(e).node.try_as<ast::name>();
        if (n == nullptr)
            continue;
        auto const text = text_of(file, n->where);
        auto const index = find_feature(text);
        if (index < 0)
        {
            report(diagnostic_kind::unknown_feature, file, n->where,
                   cc::format("{}; a shader may require {}", text, feature_list()));
            continue;
        }
        auto const f = feature(index);
        result.set(f);
        // CHK-265: a file's `require` states what the whole file may do, and a binding's what its listers need,
        // so neither is ever unused.
        if (scope == require_scope::body)
            require_lines.push_back({.file = file, .where = n->where, .what = f, .scope = scope, .owner = owner});
    }
    return result;
}

void checker::judge_entry_features(symbol_id id)
{
    auto const& s = out.at(id);
    auto& info = out.functions[s.info];
    if (info.entry_stage == stage::none)
        return;

    // CHK-261 and CHK-263: what the listed bindings require is what a device needs, and nothing it merely may use.
    auto needed = feature_set();
    auto declared = file_features[s.file];
    for (auto const b : out.at(info.bindings))
    {
        needed |= out.bindings[out.at(b).info].required;
        declared |= out.bindings[out.at(b).info].declared;
    }

    // CHK-265: a body's `require` is used where it declares what nothing else declares; the first of a feature counts.
    auto in_body = feature_set();
    for (auto& line : require_lines)
    {
        if (line.owner != id || line.scope != require_scope::body || in_body.has(line.what))
            continue;
        in_body.set(line.what);
        line.is_used = needed.has(line.what) && !declared.has(line.what);
    }
    declared |= in_body;
    info.features = needed;

    // CHK-264
    auto const where = ast_of(s.file).at(s.declaration).node.as<ast::fun_decl>().name;
    for (auto i = isize(0); i < k_feature_count; ++i)
    {
        auto const f = feature(i);
        if (!needed.has(f) || declared.has(f))
            continue;
        auto& d = report(diagnostic_kind::feature_not_declared, s.file, where,
                         cc::format("{} needs {}, which neither its file, a binding it lists nor its body requires",
                                    s.name, name_of(f)));
        for (auto const b : out.at(info.bindings))
        {
            if (!out.bindings[out.at(b).info].required.has(f))
                continue;
            auto const& binding = out.at(b);
            d.notes.push_back({.file = binding.file,
                               .where = ast_of(binding.file).at(binding.declaration).node.as<ast::binding_decl>().name,
                               .message = cc::format("{} needs {}", binding.name, name_of(f))});
        }
    }
}

void checker::report_unused_requires()
{
    for (auto const& line : require_lines)
        if (!line.is_used)
            report(diagnostic_kind::unused_require, line.file, line.where,
                   cc::format("nothing in {} needs {} of it",
                              out.at(line.owner).name.empty() ? cc::string_view("the test")
                                                              : cc::string_view(out.at(line.owner).name),
                              name_of(line.what)));
}
