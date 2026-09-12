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

    // Every table is an array binding in ONE space — the pass's, from the group these are declared in — laid out
    // end to end, because an array consumes one index per element.
    //
    // That the ranges do not overlap is the property worth checking, and the reason the whole set is declared in
    // every permutation: a shader declaring a subset would start its first table at t0 and disagree with all of
    // this.
    auto next_free = u32(0);
    for (auto const& b : all)
    {
        CHECK(b.count >= 2);
        CHECK(b.is_array());

        REQUIRE(b.space.has_value());
        CHECK(b.space.value() == u32(sv::bindless_group));

        REQUIRE(b.group_index.has_value());
        CHECK(b.group_index.value() == u32(sv::bindless_group));

        // Declaration order, so each table begins exactly where the one before it ended.
        CHECK(b.index == next_free);
        next_free = b.index + b.count;
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
