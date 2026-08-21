#include <nexus/test.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/binding/binding_group_layout.hh>
#include <shaped-graphics/binding/impl/layout_hash.hh>
#include <shaped-graphics/binding/pipeline_layout.hh>

using namespace cc::primitive_defines;

// The layout hashes are pure CPU value functions over sg-level inputs — no device, no backend.
// That is the point of them: they have to mean the same thing in a process that has no GPU at all.

namespace
{
/// A group layout carrying nothing but its identity and its bindings, which is all these tests read.
struct fake_group_layout final : sg::binding_group_layout
{
    fake_group_layout(cc::hash128 h, cc::vector<sg::binding> bindings) : sg::binding_group_layout(h, cc::move(bindings))
    {
    }
};

sg::binding_group_layout_handle group_of(cc::span<sg::binding const> bindings,
                                         cc::span<sg::named_sampler const> static_samplers = {})
{
    auto declared = cc::vector<sg::binding>();
    declared.push_back_range(bindings);
    return std::make_shared<fake_group_layout>(sg::impl::binding_group_layout_hash(bindings, static_samplers),
                                               cc::move(declared));
}

sg::binding uniform(cc::string_view name, u32 index)
{
    return {.name = cc::string::create_copy_of(name), .index = index, .type = sg::binding_type::uniform_buffer};
}
} // namespace

TEST("sg binding-group-layout hash is over content, not over identity")
{
    sg::binding const a[] = {uniform("Params", 0), uniform("Extra", 1)};
    sg::binding const same[] = {uniform("Params", 0), uniform("Extra", 1)};

    // Two independently built descriptions, no shared storage: equal content must give one hash.
    CHECK(sg::impl::binding_group_layout_hash(a, {}) == sg::impl::binding_group_layout_hash(same, {}));
}

TEST("sg binding-group-layout hash separates every field it covers")
{
    sg::binding const base[] = {uniform("Params", 0)};
    auto const key = sg::impl::binding_group_layout_hash(base, {});

    sg::binding const renamed[] = {uniform("Other", 0)};
    CHECK(sg::impl::binding_group_layout_hash(renamed, {}) != key);

    sg::binding const moved[] = {uniform("Params", 1)};
    CHECK(sg::impl::binding_group_layout_hash(moved, {}) != key);

    auto retyped = base[0];
    retyped.type = sg::binding_type::readonly_structured_buffer;
    CHECK(sg::impl::binding_group_layout_hash(cc::span<sg::binding const>(&retyped, 1), {}) != key);

    auto counted = base[0];
    counted.count = 4;
    CHECK(sg::impl::binding_group_layout_hash(cc::span<sg::binding const>(&counted, 1), {}) != key);

    auto dimensioned = base[0];
    dimensioned.texture_dimension = sg::texture_view_dimension::cube;
    CHECK(sg::impl::binding_group_layout_hash(cc::span<sg::binding const>(&dimensioned, 1), {}) != key);

    // Order is part of the layout: slot i is a different slot from slot j.
    sg::binding const forward[] = {uniform("A", 0), uniform("B", 1)};
    sg::binding const backward[] = {uniform("B", 1), uniform("A", 0)};
    CHECK(sg::impl::binding_group_layout_hash(forward, {}) != sg::impl::binding_group_layout_hash(backward, {}));
}

TEST("sg binding-group-layout hash covers the static samplers baked into it")
{
    sg::binding const bindings[] = {{.name = cc::string("Samp"), .type = sg::binding_type::sampler}};

    sg::named_sampler const point[]
        = {{.name = cc::string("Samp"), .sampler = {.min_filter = sg::sampler_filter::nearest}}};
    sg::named_sampler const linear[]
        = {{.name = cc::string("Samp"), .sampler = {.min_filter = sg::sampler_filter::linear}}};

    auto const bare = sg::impl::binding_group_layout_hash(bindings, {});
    CHECK(sg::impl::binding_group_layout_hash(bindings, point) != bare);
    CHECK(sg::impl::binding_group_layout_hash(bindings, point) != sg::impl::binding_group_layout_hash(bindings, linear));
}

TEST("sg pipeline-layout hash reaches through its groups' content")
{
    sg::binding const bindings[] = {uniform("Params", 0)};
    sg::binding const other[] = {uniform("Params", 3)};

    // Two DIFFERENT handles over the same content — the case the old pointer-identity key got wrong.
    auto const desc = sg::pipeline_layout_description{.groups = {group_of(bindings)}};
    auto const twin = sg::pipeline_layout_description{.groups = {group_of(bindings)}};
    CHECK(desc.groups[0] != twin.groups[0]);
    CHECK(sg::impl::pipeline_layout_hash(desc) == sg::impl::pipeline_layout_hash(twin));

    auto const different = sg::pipeline_layout_description{.groups = {group_of(other)}};
    CHECK(sg::impl::pipeline_layout_hash(different) != sg::impl::pipeline_layout_hash(desc));

    // Slot order matters, and so does how many slots there are.
    auto const two = sg::pipeline_layout_description{.groups = {group_of(bindings), group_of(other)}};
    auto const swapped = sg::pipeline_layout_description{.groups = {group_of(other), group_of(bindings)}};
    CHECK(sg::impl::pipeline_layout_hash(two) != sg::impl::pipeline_layout_hash(swapped));
    CHECK(sg::impl::pipeline_layout_hash(two) != sg::impl::pipeline_layout_hash(desc));
}

TEST("sg pipeline-layout hash covers static samplers and inline constants")
{
    sg::binding const bindings[] = {uniform("Params", 0)};
    auto const bare = sg::pipeline_layout_description{.groups = {group_of(bindings)}};

    auto sampled = sg::pipeline_layout_description{.groups = {group_of(bindings)}};
    sampled.static_samplers.push_back({.binding = {.space = 1, .index = 0, .type = sg::binding_type::sampler}});
    CHECK(sg::impl::pipeline_layout_hash(sampled) != sg::impl::pipeline_layout_hash(bare));

    auto inlined = sg::pipeline_layout_description{.groups = {group_of(bindings)}};
    inlined.inline_constants
        = sg::binding{.name = cc::string("Push"), .space = 2, .type = sg::binding_type::uniform_buffer, .block_size = 64};
    CHECK(sg::impl::pipeline_layout_hash(inlined) != sg::impl::pipeline_layout_hash(bare));

    // block_size sizes the root-constants parameter, so it is part of the identity.
    auto wider = inlined;
    wider.inline_constants.value().block_size = 128;
    CHECK(sg::impl::pipeline_layout_hash(wider) != sg::impl::pipeline_layout_hash(inlined));
}

TEST("sg layout carries the hash it was created with")
{
    sg::binding const bindings[] = {uniform("Params", 0)};
    auto const layout = group_of(bindings);

    CHECK(layout->structural_hash() == sg::impl::binding_group_layout_hash(bindings, {}));
}
