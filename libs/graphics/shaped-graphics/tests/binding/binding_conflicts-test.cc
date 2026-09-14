#include <nexus/test.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/binding/impl/binding_conflicts.hh>

using namespace cc::primitive_defines;

// The stages of one pipeline share a binding interface, and each is reflected from its own module — so nothing but
// this makes them agree.
// What it must catch is two shaders that number a group differently; what it must not catch is the ordinary case of
// two stages that legitimately reuse an address in different register classes.

namespace
{
[[nodiscard]] sg::compiled_shader make_shader(cc::string_view entry, cc::vector<sg::binding> bindings)
{
    sg::compiled_shader s;
    s.entry_point = cc::string(entry);
    s.bindings = cc::move(bindings);
    return s;
}

/// A DXIL-shaped binding: a register space and a per-class index, and no group.
[[nodiscard]] sg::binding dxil_binding(cc::string_view name, sg::binding_type type, u32 index)
{
    sg::binding b;
    b.name = cc::string(name);
    b.type = type;
    b.space = 0u;
    b.index = index;
    return b;
}

/// A SPIR-V-shaped binding: a set, and one index space for every kind.
[[nodiscard]] sg::binding spirv_binding(cc::string_view name, sg::binding_type type, u32 group, u32 index)
{
    sg::binding b;
    b.name = cc::string(name);
    b.type = type;
    b.group_index = group;
    b.index = index;
    return b;
}
} // namespace

TEST("sg::binding - stages that agree carry no conflict")
{
    auto const vs = make_shader("main_vs", {spirv_binding("frame", sg::binding_type::uniform_buffer, 0, 0)});
    auto const ps = make_shader("main_ps", {spirv_binding("frame", sg::binding_type::uniform_buffer, 0, 0),
                                            spirv_binding("albedo", sg::binding_type::readonly_texture, 0, 1)});

    sg::compiled_shader const* const stages[] = {&vs, &ps};
    CHECK(!sg::impl::find_binding_conflict(stages).has_value());
}

TEST("sg::binding - two stages numbering one group differently is a conflict")
{
    // The failure the shared-header rule exists to prevent: each file counted from zero on its own.
    auto const vs = make_shader("main_vs", {spirv_binding("frame", sg::binding_type::uniform_buffer, 0, 0)});
    auto const ps = make_shader("main_ps", {spirv_binding("albedo", sg::binding_type::readonly_texture, 0, 0)});

    sg::compiled_shader const* const stages[] = {&vs, &ps};
    auto const conflict = sg::impl::find_binding_conflict(stages);
    REQUIRE(conflict.has_value());
    CHECK(conflict.value().contains("frame"));
    CHECK(conflict.value().contains("albedo"));
    CHECK(conflict.value().contains("main_ps"));
}

TEST("sg::binding - one name at two addresses is a conflict")
{
    // A binding group resolves a name, so this one cannot be satisfied by any single group.
    auto const vs = make_shader("main_vs", {spirv_binding("albedo", sg::binding_type::readonly_texture, 0, 2)});
    auto const ps = make_shader("main_ps", {spirv_binding("albedo", sg::binding_type::readonly_texture, 0, 5)});

    sg::compiled_shader const* const stages[] = {&vs, &ps};
    auto const conflict = sg::impl::find_binding_conflict(stages);
    REQUIRE(conflict.has_value());
    CHECK(conflict.value().contains("albedo"));
}

TEST("sg::binding - HLSL register classes share an index without colliding")
{
    // b0 and t0 are different addresses that both reflect as index 0, which is why the class is part of a DXIL address.
    // Without that, every raster pipeline with inline constants would report a conflict.
    auto const vs = make_shader("main_vs", {dxil_binding("frame", sg::binding_type::uniform_buffer, 0)});
    auto const ps = make_shader("main_ps", {dxil_binding("albedo", sg::binding_type::readonly_texture, 0),
                                            dxil_binding("linear_sampler", sg::binding_type::sampler, 0)});

    sg::compiled_shader const* const stages[] = {&vs, &ps};
    CHECK(!sg::impl::find_binding_conflict(stages).has_value());
}

TEST("sg::binding - a null stage is skipped rather than dereferenced")
{
    // The raster description carries optional stages, and the caller passes them straight through.
    auto const vs = make_shader("main_vs", {spirv_binding("frame", sg::binding_type::uniform_buffer, 0, 0)});

    sg::compiled_shader const* const stages[] = {&vs, nullptr};
    CHECK(!sg::impl::find_binding_conflict(stages).has_value());
}
