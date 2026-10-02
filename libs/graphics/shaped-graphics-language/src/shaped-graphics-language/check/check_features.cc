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
    // CHK-272: a stage input some device lacks needs its feature, as a binding member does; a stage that has it
    // natively needs none
    for (auto const& parameter : out.at(info.parameters))
        if (parameter.input != stage_input::none && info_of(parameter.input).feature >= 0
            && (info_of(parameter.input).in_stage == info.entry_stage
                || info_of(parameter.input).needs_feature_everywhere))
            needed.set(feature(info_of(parameter.input).feature));
    // CHK-301, CHK-304, CHK-306: the geometry and the tessellation stages are features a device grants
    if (info.entry_stage == stage::geometry)
        needed.set(feature::geometry_shader);
    if (info.entry_stage == stage::tessellation_control || info.entry_stage == stage::tessellation_evaluation)
        needed.set(feature::tessellation_shader);
    // CHK-326: the ray-tracing stages are the pipeline's, which a device grants
    if (info.entry_stage >= stage::raygen)
        needed.set(feature::raytracing_pipeline);
    // CHK-274: a pixel stage that takes a member per sample runs per sample, which vulkan gives only with a feature
    if (info.entry_stage == stage::pixel)
        for (auto const& parameter : out.at(info.parameters))
            if (parameter.input == stage_input::none && out.at(parameter.type).kind == type_kind::structure)
                for (auto const& m : out.at(out.at(parameter.type).members))
                    if (m.interpolate.sampling == interpolation::sampling_t::sample)
                        needed.set(feature::sample_rate_shading);

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
    notes[s.info].declared_features = declared;

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

feature_set checker::features_of_type(type_id type) const
{
    if (!is_valid(type) || type == checked_module::error_type || type == checked_module::void_type)
        return {};
    if (auto const* const record = out.builtin_type_of(type))
        return record->features;
    auto const& t = out.at(type);
    switch (t.kind)
    {
    case type_kind::array:
    case type_kind::atomic:
    case type_kind::buffer:
        return features_of_type(t.element);
    case type_kind::structure:
    {
        auto result = feature_set();
        for (auto const& m : out.at(t.members))
            result |= features_of_type(m.type);
        return result;
    }
    default:
        return {};
    }
}

cc::string checker::sixteen_bit_path(type_id type) const
{
    if (type == checked_module::error_type)
        return {};
    if (auto const* const record = out.builtin_type_of(type))
        return is_16_bit(record->leaf_kind) ? cc::format(": {}", out.name_of(type)) : cc::string();
    auto const& t = out.at(type);
    if (t.kind == type_kind::array)
        return sixteen_bit_path(t.element);
    if (t.kind != type_kind::structure)
        return {};
    for (auto const& m : out.at(t.members))
        if (auto inner = sixteen_bit_path(m.type); !inner.empty())
            return cc::format(".{}{}", m.name, inner);
    return {};
}

void checker::judge_edge_16_bit(i32 file, source_span where, type_id type)
{
    // a patch and a geometry stage's vertices are arrays of the struct that crosses, and a stream holds it
    while (type != checked_module::error_type && out.builtin_type_of(type) == nullptr
           && (out.at(type).kind == type_kind::array || out.at(type).kind == type_kind::stream))
        type = out.at(type).element;
    if (type == checked_module::error_type || out.builtin_type_of(type) != nullptr
        || out.at(type).kind != type_kind::structure)
        return;
    if (auto const found = sixteen_bit_path(type); !found.empty())
        unsupported(file, where, cc::format("a 16-bit value crossing a stage edge, in {}{}", out.name_of(type), found));
}

void checker::mark_requires_used(cc::span<symbol_id const> functions, feature_set features)
{
    for (auto const function : functions)
        for (auto i = isize(0); i < k_feature_count; ++i)
        {
            if (!features.has(feature(i)))
                continue;
            for (auto& line : require_lines)
                if (line.owner == function && line.scope == require_scope::body && line.what == feature(i))
                {
                    line.is_used = true;
                    break;
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
