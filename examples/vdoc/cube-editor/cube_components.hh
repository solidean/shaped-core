#pragma once

#include <clean-core/fwd.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/linalg/pos.hh>
#include <versioned-document/component.hh>

/// The component set this example's documents are made of.
///
/// versioned-document ships zero components on purpose: what a document MEANS is the application's.
/// Two of them rather than one, so the render loop is a real two-component join (`doc.each<placement, style>`)
/// and the two can be edited independently — moving a cube writes no color.

namespace cube_editor
{
using namespace cc::primitive_defines;

/// Where a cube is and how big it is. One entity is one cube.
struct placement
{
    tg::pos3f center;
    f32 half_extent = 0.5f;

    [[nodiscard]] tg::aabb3f box() const
    {
        auto const e = tg::vec3f(half_extent, half_extent, half_extent);
        return tg::aabb3f(center - e, center + e);
    }
};

/// How a cube looks. Split from placement so a color edit and a move are separate properties in the history.
struct style
{
    tg::vec3f color = tg::vec3f(0.8f, 0.8f, 0.85f);
};

/// The registry every parse policy here is built from.
///
/// One per process rather than one per document: the component set is a property of the APPLICATION, and a parse
/// policy holds the registry by reference — so a per-document copy would dangle the moment the document was moved.
[[nodiscard]] vdoc::component_registry const& registry();
} // namespace cube_editor

template <>
struct vdoc::component_traits<cube_editor::placement>
{
    static constexpr cc::string_view type_name = "Placement";
    static constexpr i32 schema_version = 1;

    // write() must NOT stamp $schema_version — op_builder::set does that once, and any '$' name asserts here.
    static void write(cube_editor::placement const& c, vdoc::component_writer& w)
    {
        w.set("x", vdoc::value::of(c.center[0]));
        w.set("y", vdoc::value::of(c.center[1]));
        w.set("z", vdoc::value::of(c.center[2]));
        w.set("half_extent", vdoc::value::of(c.half_extent));
    }

    // try_get is the only supported read: it is where the one-writer / agreed / conflicting rules live.
    // Returning nothing would mean "drop this component"; a missing property just keeps its default.
    static cc::optional<cube_editor::placement> parse(vdoc::property_reader const& r)
    {
        auto out = cube_editor::placement();
        if (auto const v = r.try_get("x"); v.has_value())
            out.center[0] = f32(v.value().as_f64());
        if (auto const v = r.try_get("y"); v.has_value())
            out.center[1] = f32(v.value().as_f64());
        if (auto const v = r.try_get("z"); v.has_value())
            out.center[2] = f32(v.value().as_f64());
        if (auto const v = r.try_get("half_extent"); v.has_value())
            out.half_extent = f32(v.value().as_f64());
        return out;
    }
};

template <>
struct vdoc::component_traits<cube_editor::style>
{
    static constexpr cc::string_view type_name = "Style";
    static constexpr i32 schema_version = 1;

    static void write(cube_editor::style const& c, vdoc::component_writer& w)
    {
        w.set("r", vdoc::value::of(c.color[0]));
        w.set("g", vdoc::value::of(c.color[1]));
        w.set("b", vdoc::value::of(c.color[2]));
    }

    static cc::optional<cube_editor::style> parse(vdoc::property_reader const& r)
    {
        auto out = cube_editor::style();
        if (auto const v = r.try_get("r"); v.has_value())
            out.color[0] = f32(v.value().as_f64());
        if (auto const v = r.try_get("g"); v.has_value())
            out.color[1] = f32(v.value().as_f64());
        if (auto const v = r.try_get("b"); v.has_value())
            out.color[2] = f32(v.value().as_f64());
        return out;
    }
};
