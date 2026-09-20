#include "emit.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/emit/impl/dialect.hh>
#include <shaped-graphics-language/emit/impl/plan.hh>

namespace
{
constexpr sgl::emit::target k_all_targets[] = {
    sgl::emit::target::hlsl_dx12,
    sgl::emit::target::hlsl_vulkan,
    sgl::emit::target::wgsl,
    sgl::emit::target::msl,
};
} // namespace

cc::span<sgl::emit::target const> sgl::emit::all_targets()
{
    return k_all_targets;
}

cc::string_view sgl::emit::to_string(target t)
{
    switch (t)
    {
    case target::hlsl_dx12:
        return "hlsl-dx12";
    case target::hlsl_vulkan:
        return "hlsl-vulkan";
    case target::wgsl:
        return "wgsl";
    case target::msl:
        return "msl";
    }
    return "";
}

cc::string_view sgl::emit::to_string(error_kind kind)
{
    switch (kind)
    {
    case error_kind::module_has_errors:
        return "module-has-errors";
    case error_kind::unknown_entry_point:
        return "unknown-entry-point";
    case error_kind::reserved_entry_point_name:
        return "reserved-entry-point-name";
    case error_kind::unsupported:
        return "unsupported";
    case error_kind::system_value_semantic:
        return "system-value-semantic";
    case error_kind::layout_mismatch:
        return "layout-mismatch";
    case error_kind::non_finite_literal:
        return "non-finite-literal";
    case error_kind::malformed_tree:
        return "malformed-tree";
    case error_kind::not_core:
        return "not-core";
    }
    return "";
}

// The one place that knows every target; a new one is a case here and a file under impl/.
sgl::emit::impl::dialect const& sgl::emit::impl::dialect_of(target t)
{
    switch (t)
    {
    case target::hlsl_dx12:
        return hlsl_dx12_dialect();
    case target::hlsl_vulkan:
        return hlsl_vulkan_dialect();
    case target::wgsl:
        return wgsl_dialect();
    case target::msl:
        return msl_dialect();
    }
    return hlsl_dx12_dialect();
}

sgl::emit::emitted_text sgl::emit::emit(check::checked_module const& m, sgl::isize entry_point, target t)
{
    auto result = emitted_text();

    auto error_count = 0;
    for (auto const& d : m.diagnostics)
        if (d.what.level != severity::warning)
            ++error_count;
    if (error_count != 0)
    {
        result.errors.push_back({.kind = error_kind::module_has_errors, .detail = cc::format("{} errors", error_count)});
        return result;
    }
    if (entry_point < 0 || entry_point >= m.entry_points.size())
    {
        result.errors.push_back({.kind = error_kind::unknown_entry_point,
                                 .detail = cc::format("{} of {}", entry_point, m.entry_points.size())});
        return result;
    }

    return emit_entry_point(m, m.entry_points[entry_point], t);
}

sgl::emit::emitted_text sgl::emit::emit_entry_point(check::checked_module const& m,
                                                    check::flat_entry_point const& e,
                                                    target t)
{
    auto result = emitted_text();
    impl::validate(m, e, result.errors);
    if (!result.errors.empty())
        return result;

    auto plan = impl::make_plan(m, e, t);
    result.text = impl::write_text(plan, impl::dialect_of(t));
    return result;
}

cc::string sgl::emit::dump_errors(emitted_text const& e)
{
    auto out = cc::string();
    for (auto const& error : e.errors)
        out.appendf("{} {}\n", to_string(error.kind), error.detail);
    return out;
}
