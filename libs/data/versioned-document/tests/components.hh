#pragma once

#include <clean-core/string/string.hh>
#include <versioned-document/component.hh>

/// The component types the vdoc tests use.
///
/// The library ships none, so every test that needs one declares it here — which is also the worked example of what
/// an application writes.

namespace vdoc_test
{
using namespace cc::primitive_defines;

struct transform;
struct mesh;
struct tag;
} // namespace vdoc_test

/// A two-field component at version 2, which migrates a version-1 layout forward.
/// Version 1 stored one `pos` array; version 2 stores `x` and `y` separately.
struct vdoc_test::transform
{
    f64 x = 0;
    f64 y = 0;

    [[nodiscard]] friend bool operator==(transform const&, transform const&) = default;
};

/// A component that owns heap storage, so a document's destruction is actually observable.
struct vdoc_test::mesh
{
    cc::string asset;

    [[nodiscard]] friend bool operator==(mesh const&, mesh const&) = default;
};

/// A component with no properties at all, which must still instantiate.
struct vdoc_test::tag
{
    [[nodiscard]] friend bool operator==(tag const&, tag const&) = default;
};

template <>
struct vdoc::component_traits<vdoc_test::transform>
{
    static constexpr cc::string_view type_name = "Transform";
    static constexpr i32 schema_version = 2;

    static void write(vdoc_test::transform const& c, vdoc::component_writer& w)
    {
        w.set("x", vdoc::value::of(c.x));
        w.set("y", vdoc::value::of(c.y));
    }

    static cc::optional<vdoc_test::transform> parse(vdoc::property_reader const& r)
    {
        auto out = vdoc_test::transform();

        if (r.schema_version() < 2)
        {
            // Version 1's single `pos` array, migrated forward; the stored op is never rewritten.
            auto const pos = r.try_get("pos");
            if (pos.has_value() && pos.value().kind() == vdoc::value_kind::array && pos.value().size() == 2)
            {
                out.x = pos.value().element_at(0).as_f64();
                out.y = pos.value().element_at(1).as_f64();
            }
            return out;
        }

        if (auto const x = r.try_get("x"); x.has_value())
            out.x = x.value().as_f64();
        if (auto const y = r.try_get("y"); y.has_value())
            out.y = y.value().as_f64();

        return out;
    }
};

template <>
struct vdoc::component_traits<vdoc_test::mesh>
{
    static constexpr cc::string_view type_name = "Mesh";
    static constexpr i32 schema_version = 1;

    static void write(vdoc_test::mesh const& c, vdoc::component_writer& w)
    {
        w.set("asset", vdoc::value::of(cc::string_view(c.asset)));
    }

    static cc::optional<vdoc_test::mesh> parse(vdoc::property_reader const& r)
    {
        auto const asset = r.try_get("asset");
        if (!asset.has_value() || asset.value().kind() != vdoc::value_kind::string)
            return {}; // no asset, no mesh

        return vdoc_test::mesh{.asset = cc::string(asset.value().as_string())};
    }
};

template <>
struct vdoc::component_traits<vdoc_test::tag>
{
    static constexpr cc::string_view type_name = "Tag";
    static constexpr i32 schema_version = 1;

    static void write(vdoc_test::tag const&, vdoc::component_writer&) {}
    static cc::optional<vdoc_test::tag> parse(vdoc::property_reader const&) { return vdoc_test::tag{}; }
};
