#include "op_graph_corpus.hh"

#include <nexus/test.hh>

/// The corpus itself, checked against the brute-force oracle before anything relies on it.
///
/// Everything the snapshot cache asserts is "the cached pass equals the plain pass over this corpus".
/// That says nothing unless the plain pass is right and the corpus actually contains the shapes that break a cache,
/// so both are pinned here first.

using namespace vdoc_test;

TEST("vdoc - the corpus agrees with the brute-force oracle")
{
    auto const corpus = generate_corpus();
    REQUIRE(corpus.size() > 20);

    for (auto const& c : corpus)
    {
        REQUIRE(!c.ops.empty());
        REQUIRE(!c.head_sets.empty());

        for (auto const& heads : c.head_sets)
            for (auto const& path : c.paths)
            {
                auto const actual = writers_of(c.graph.materialize(heads), path);
                auto const expected = oracle_writers(c.graph, heads, path);
                CHECK(same_ids(actual, expected));
            }
    }
}

TEST("vdoc - the corpus contains the shapes a snapshot cache can break on")
{
    auto const corpus = generate_corpus();

    auto multi_valued = false;
    auto ancestor_descendant = false;
    auto several_tips = false;

    for (auto const& c : corpus)
        for (auto const& heads : c.head_sets)
        {
            auto const doc = c.graph.materialize(heads);
            for (auto const& path : c.paths)
                if (auto const* const p = doc.try_get(path); p != nullptr && p->is_multi_valued())
                    multi_valued = true;

            if (heads.size() > 1)
            {
                several_tips = true;

                // an {ancestor, descendant} head set is the one a two-source gate would wrongly accept
                for (auto const& a : heads)
                    for (auto const& b : heads)
                    {
                        if (a == b)
                            continue;

                        auto const from_a = c.graph.collect_reachable(cc::span<vdoc::op_id const>(&a, 1));
                        for (auto const& reached : from_a)
                            ancestor_descendant = ancestor_descendant || (reached == b);
                    }
            }
        }

    CHECK(multi_valued);
    CHECK(several_tips);
    CHECK(ancestor_descendant);
}
