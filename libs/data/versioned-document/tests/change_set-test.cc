#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <versioned-document/change_set.hh>

/// The property every test here asserts: a change set over-reports at worst, and never under-reports.
///
/// That is what makes coarsening a speed dial rather than a correctness risk, so the randomized test at the bottom is
/// the one that matters most — it checks that no path a set covered stops being covered as the set is widened.
///
/// See [interpretation](../docs/concepts/interpretation.md#applying-an-op-incrementally).

using namespace cc::primitive_defines;

using vdoc::change_granularity;
using vdoc::change_set;
using vdoc::change_set_builder;
using vdoc::component_type_id;
using vdoc::entity_id;
using vdoc::property_id;
using vdoc::property_path;

namespace
{
[[nodiscard]] property_path path_of(cc::string_view e, cc::string_view c, cc::string_view p)
{
    return property_path{.entity = entity_id::of(e),
                         .component = component_type_id::of(c),
                         .property = property_id::of(p)};
}

/// A fixed LCG, so a failing randomized case is reproducible from its seed.
struct lcg
{
    u32 state = 1;

    [[nodiscard]] u32 next()
    {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }

    [[nodiscard]] u32 next_below(u32 bound) { return next() % bound; }
};
} // namespace

TEST("vdoc - a default change set covers nothing")
{
    auto const empty = change_set();

    CHECK(empty.is_empty());
    CHECK(!empty.is_everything());
    CHECK(empty.granularity() == change_granularity::property);
    CHECK(!empty.covers(path_of("e1", "Transform", "position")));
    CHECK(!empty.covers_entity(entity_id::of("e1")));
    CHECK(empty.entities().empty());
}

TEST("vdoc - everything covers every path, and is never empty")
{
    auto const all = change_set::everything();

    CHECK(all.is_everything());
    CHECK(!all.is_empty());
    CHECK(all.covers(path_of("anything", "AtAll", "really")));
    CHECK(all.covers_entity(entity_id::of("an entity no layer has")));
}

TEST("vdoc - a built set is sorted and deduplicated")
{
    auto builder = change_set_builder();

    // deliberately out of order, and with one path staged twice
    builder.add(path_of("e2", "Mesh", "asset"));
    builder.add(path_of("e1", "Transform", "y"));
    builder.add(path_of("e1", "Transform", "x"));
    builder.add(path_of("e1", "Transform", "y"));

    auto const dirty = cc::move(builder).build();

    REQUIRE(dirty.entries().size() == 3);
    CHECK(dirty.entries()[0] == path_of("e1", "Transform", "x"));
    CHECK(dirty.entries()[1] == path_of("e1", "Transform", "y"));
    CHECK(dirty.entries()[2] == path_of("e2", "Mesh", "asset"));

    CHECK(dirty.covers(path_of("e1", "Transform", "x")));
    CHECK(!dirty.covers(path_of("e1", "Transform", "z")));
}

TEST("vdoc - covers is exact at property granularity and a prefix match above it")
{
    auto builder = change_set_builder();
    builder.add(path_of("e1", "Transform", "x"));
    auto dirty = cc::move(builder).build();

    // a sibling property under the same component is NOT covered while the set is precise
    CHECK(dirty.covers(path_of("e1", "Transform", "x")));
    CHECK(!dirty.covers(path_of("e1", "Transform", "y")));
    CHECK(!dirty.covers(path_of("e1", "Mesh", "asset")));

    // but covers_entity answers at every granularity, because a blanked field sorts before every real one
    CHECK(dirty.covers_entity(entity_id::of("e1")));
    CHECK(!dirty.covers_entity(entity_id::of("e2")));

    dirty.coarsen_to(change_granularity::component);
    CHECK(dirty.covers(path_of("e1", "Transform", "y"))); // now the whole component is claimed
    CHECK(!dirty.covers(path_of("e1", "Mesh", "asset")));

    dirty.coarsen_to(change_granularity::entity);
    CHECK(dirty.covers(path_of("e1", "Mesh", "asset"))); // and now the whole entity
    CHECK(!dirty.covers(path_of("e2", "Transform", "x")));
}

TEST("vdoc - coarsening collapses what it blanks together, and is idempotent")
{
    auto builder = change_set_builder();
    builder.add(path_of("e1", "Transform", "x"));
    builder.add(path_of("e1", "Transform", "y"));
    builder.add(path_of("e1", "Mesh", "asset"));
    builder.add(path_of("e2", "Transform", "x"));

    auto dirty = cc::move(builder).build();
    REQUIRE(dirty.entries().size() == 4);

    dirty.coarsen_to(change_granularity::component);
    CHECK(dirty.entries().size() == 3); // the two Transform properties became one entry

    dirty.coarsen_to(change_granularity::component);
    CHECK(dirty.entries().size() == 3); // and again changes nothing

    dirty.coarsen_to(change_granularity::entity);
    CHECK(dirty.entries().size() == 2);
    CHECK(dirty.granularity() == change_granularity::entity);
}

TEST("vdoc - entities is sorted and unique whatever the granularity")
{
    auto builder = change_set_builder();
    builder.add(path_of("e2", "Mesh", "asset"));
    builder.add(path_of("e1", "Transform", "x"));
    builder.add(path_of("e1", "Transform", "y"));

    auto dirty = cc::move(builder).build();

    auto const at_property = dirty.entities();
    REQUIRE(at_property.size() == 2);
    CHECK(at_property[0] == entity_id::of("e1"));
    CHECK(at_property[1] == entity_id::of("e2"));

    // coarsening is a speed dial, so the entity answer cannot move
    dirty.coarsen_to(change_granularity::entity);

    auto const at_entity = dirty.entities();
    REQUIRE(at_entity.size() == at_property.size());
    for (isize i = 0; i < at_entity.size(); ++i)
        CHECK(at_entity[i] == at_property[i]);
}

TEST("vdoc - add_entity coarsens the whole set, because a set carries one granularity")
{
    auto builder = change_set_builder();
    builder.add(path_of("e1", "Transform", "x"));
    builder.add_entity(entity_id::of("e2"));

    auto const dirty = cc::move(builder).build();

    CHECK(dirty.granularity() == change_granularity::entity);

    // the earlier precise path was widened with the rest rather than left claiming only itself
    CHECK(dirty.covers(path_of("e1", "Mesh", "asset")));
    CHECK(dirty.covers(path_of("e2", "Transform", "x")));
    CHECK(!dirty.covers(path_of("e3", "Transform", "x")));
}

TEST("vdoc - a union merges two sets and absorbs everything")
{
    auto a_builder = change_set_builder();
    a_builder.add(path_of("e1", "Transform", "x"));
    a_builder.add(path_of("e3", "Mesh", "asset"));
    auto a = cc::move(a_builder).build();

    auto b_builder = change_set_builder();
    b_builder.add(path_of("e1", "Transform", "x")); // shared, so it must not double up
    b_builder.add(path_of("e2", "Transform", "y"));
    auto const b = cc::move(b_builder).build();

    a.union_with(b);

    REQUIRE(a.entries().size() == 3);
    CHECK(a.entries()[0] == path_of("e1", "Transform", "x"));
    CHECK(a.entries()[1] == path_of("e2", "Transform", "y"));
    CHECK(a.entries()[2] == path_of("e3", "Mesh", "asset"));

    a.union_with(change_set::everything());
    CHECK(a.is_everything());
    CHECK(a.entries().empty()); // the canonical form: everything carries no paths

    // and everything stays everything, rather than being narrowed by a later union
    a.union_with(b);
    CHECK(a.is_everything());
}

TEST("vdoc - a union takes the coarser of the two granularities")
{
    auto a_builder = change_set_builder();
    a_builder.add(path_of("e1", "Transform", "x"));
    auto a = cc::move(a_builder).build();

    auto b_builder = change_set_builder();
    b_builder.add_entity(entity_id::of("e2"));
    auto const b = cc::move(b_builder).build();

    a.union_with(b);

    // widening a rather than refining b is the only direction that cannot invent information
    CHECK(a.granularity() == change_granularity::entity);
    CHECK(a.covers_entity(entity_id::of("e1")));
    CHECK(a.covers_entity(entity_id::of("e2")));
    CHECK(a.covers(path_of("e1", "Mesh", "asset")));
}

TEST("vdoc - coarsening never drops a path the set covered")
{
    // The contract, checked the only way it can be: widen a random set step by step, and every path it ever answered
    // true for must keep answering true.
    // A regression here is silent under-invalidation, which no consumer can detect.
    auto rng = lcg();

    for (isize round = 0; round < 200; ++round)
    {
        auto builder = change_set_builder();
        auto staged = cc::vector<property_path>();

        auto const count = isize(rng.next_below(12)) + 1;
        for (isize i = 0; i < count; ++i)
        {
            auto const p = path_of(cc::format("e{}", rng.next_below(4)), cc::format("C{}", rng.next_below(3)),
                                   cc::format("p{}", rng.next_below(3)));
            builder.add(p);
            staged.push_back(p);
        }

        auto dirty = cc::move(builder).build();

        for (auto const& p : staged)
            REQUIRE(dirty.covers(p));

        for (auto const g : {change_granularity::component, change_granularity::entity})
        {
            dirty.coarsen_to(g);

            for (auto const& p : staged)
                CHECK(dirty.covers(p));

            // and the entity answer is invariant under widening too
            for (auto const& p : staged)
                CHECK(dirty.covers_entity(p.entity));
        }

        // widening only ever merges entries, so it cannot grow the set
        CHECK(dirty.entries().size() <= staged.size());
    }
}
