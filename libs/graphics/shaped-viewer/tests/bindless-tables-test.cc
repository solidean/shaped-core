#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-viewer/resources/bindless_tables.hh>

using namespace cc::primitive_defines;

// sv's hand-declared bindless layout: the names, spaces and dimensions a shader must match, and what a budget
// of 0 or 1 means.
// Pure — no context, so it runs on every platform, unlike the manager half in gpu-resource-manager-test.cc.

namespace
{
[[nodiscard]] sv::bindless_config only(sv::bindless_table table, u32 count)
{
    return {.tables = cc::vector<sv::bindless_table_budget>{{.table = table, .count = count}}};
}

[[nodiscard]] sg::binding const* find(cc::span<sg::binding const> bindings, cc::string_view name)
{
    for (auto const& b : bindings)
        if (b.name == name)
            return &b;
    return nullptr;
}
} // namespace

TEST("sv - the bindless layout follows the config")
{
    auto const all = sv::make_bindless_bindings({});
    CHECK(all.size() == isize(sv::bindless_table::count_)); // the defaults declare every table

    // Each table is an array binding at index 0 of its own space, so a category is addressed with no
    // register-offset math and adding one never renumbers another.
    auto seen_spaces = cc::vector<u32>();
    for (auto const& b : all)
    {
        CHECK(b.index == 0);
        CHECK(b.count >= 2);
        CHECK(b.is_array());
        CHECK(b.space.has_value());
        for (auto const s : seen_spaces)
            CHECK(s != b.space.value());
        seen_spaces.push_back(b.space.value());
    }

    // A texture table carries the dimension a backend needs for a dimension-correct null descriptor; the buffer
    // table is byte-address and carries none.
    auto const* const tex2d = find(all, "gBindlessTextures2D");
    REQUIRE(tex2d != nullptr);
    CHECK(tex2d->type == sg::binding_type::readonly_texture);
    CHECK(tex2d->texture_dimension.has_value());
    CHECK(tex2d->texture_dimension.value() == sg::texture_view_dimension::tex_2d);

    auto const* const cube = find(all, "gBindlessTexturesCube");
    REQUIRE(cube != nullptr);
    CHECK(cube->texture_dimension.value() == sg::texture_view_dimension::cube);

    auto const* const buffers = find(all, "gBindlessBuffers");
    REQUIRE(buffers != nullptr);
    CHECK(buffers->type == sg::binding_type::readonly_raw_buffer);
    CHECK(!buffers->texture_dimension.has_value());
}

TEST("sv - a bindless table budgeted at zero is not declared")
{
    auto cfg = sv::bindless_config{};
    for (auto& t : cfg.tables)
        if (t.table == sv::bindless_table::textures_3d)
            t.count = 0;

    auto const bindings = sv::make_bindless_bindings(cfg);
    CHECK(bindings.size() == isize(sv::bindless_table::count_) - 1);
    CHECK(find(bindings, "gBindlessTextures3D") == nullptr);
    CHECK(find(bindings, "gBindlessTextures2D") != nullptr);

    // One element is a scalar binding to sg, which has no vacant elements — so it cannot back a table at all.
    CHECK_ASSERTS(sv::make_bindless_bindings(only(sv::bindless_table::textures_2d, 1)));
}
