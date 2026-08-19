#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <versioned-document/direct_layer.hh>
#include <versioned-document/op_builder.hh>
#include <versioned-document/op_graph.hh>

/// A layer written directly, with no op graph behind it.
///
/// Two properties matter here and neither is about composition: its writes are DIFFED, so an unchanged property costs
/// nothing, and its version is bumped by its own mutators, so "forgot to say it changed" is unreachable.
///
/// See ../docs/concepts/layering.md.

using namespace cc::primitive_defines;

using vdoc::component_type_id;
using vdoc::direct_layer;
using vdoc::entity_id;
using vdoc::op_builder;
using vdoc::op_graph;
using vdoc::property_id;
using vdoc::property_path;
using vdoc::value;

namespace
{
[[nodiscard]] property_path path_of(cc::string_view e, cc::string_view c, cc::string_view p)
{
    return property_path{.entity = entity_id::of(e),
                         .component = component_type_id::of(c),
                         .property = property_id::of(p)};
}
} // namespace

TEST("vdoc - a synthetic writer id is stable, name-derived and unlike any op id")
{
    auto const a = vdoc::synthetic_writer_id("base");
    auto const b = vdoc::synthetic_writer_id("base");
    auto const c = vdoc::synthetic_writer_id("forced");

    CHECK(a == b); // a function of the name alone, so writer sorts reproduce across runs
    CHECK(!(a == c));

    // never the all-zero id, which already means "absent parent" and "nothing chosen"
    CHECK(!(a == vdoc::op_id()));

    // and domain-separated from an op's preimage, so it cannot collide with a real writer
    auto graph = op_graph();
    auto const real = graph.add(op_builder().set_raw(path_of("base", "T", "x"), value::of(1)).build(graph));
    CHECK(!(a == real));
}

TEST("vdoc - writing the same bytes again reports nothing changed")
{
    // This is what makes a wholesale per-frame rebuild affordable: the lookup happens anyway, so the compare is free
    // and an unchanged property never reaches re-interpretation.
    auto layer = direct_layer("base");

    layer.set(path_of("e1", "T", "x"), value::of(1.0));
    auto const after_first = layer.version();
    CHECK(after_first > 0);

    layer.set(path_of("e1", "T", "x"), value::of(1.0));
    CHECK(layer.version() == after_first); // identical bytes, so nothing moved

    layer.set(path_of("e1", "T", "x"), value::of(2.0));
    CHECK(layer.version() > after_first);
}

TEST("vdoc - a rebuild drops the paths it did not write again")
{
    // A producer that recomputes everything expresses removal by simply not writing it, which is the whole reason
    // begin_rebuild exists.
    auto layer = direct_layer("base");

    layer.set(path_of("e1", "T", "x"), value::of(1.0));
    layer.set(path_of("e1", "T", "y"), value::of(2.0));
    layer.set(path_of("e2", "T", "x"), value::of(3.0));

    layer.begin_rebuild();
    layer.set(path_of("e1", "T", "x"), value::of(1.0)); // unchanged, and still counts as written
    layer.set(path_of("e2", "T", "x"), value::of(9.0));
    layer.finish_rebuild();

    CHECK(layer.document().try_get(path_of("e1", "T", "x")) != nullptr);
    CHECK(layer.document().try_get(path_of("e1", "T", "y")) == nullptr); // not rewritten, so gone
    CHECK(layer.document().try_get(path_of("e2", "T", "x"))->single().as_f64() == 9.0);

    // and the entity whose every property went is pruned, which is the shape a materialization would produce
    layer.begin_rebuild();
    layer.finish_rebuild();
    CHECK(layer.document().entities.empty());
}

TEST("vdoc - abstaining removes a path and clear empties the layer")
{
    auto layer = direct_layer("base");

    layer.set(path_of("e1", "T", "x"), value::of(1.0));
    layer.set(path_of("e1", "T", "y"), value::of(2.0));

    layer.abstain(path_of("e1", "T", "x"));
    CHECK(layer.document().try_get(path_of("e1", "T", "x")) == nullptr);
    CHECK(layer.document().try_get(path_of("e1", "T", "y")) != nullptr);

    auto const before = layer.version();
    layer.abstain(path_of("e1", "T", "x")); // already gone, so nothing to withdraw
    CHECK(layer.version() == before);

    layer.clear();
    CHECK(layer.document().entities.empty());
    CHECK(layer.version() > before);
}

TEST("vdoc - mark_dirty moves the version without touching the document")
{
    // The escape hatch for a producer that already knows what changed and does not want the byte compares.
    auto layer = direct_layer("base");
    layer.set(path_of("e1", "T", "x"), value::of(1.0));

    auto const before = layer.version();
    layer.mark_dirty(path_of("e1", "T", "x"));

    CHECK(layer.version() > before);
    CHECK(layer.document().try_get(path_of("e1", "T", "x"))->single().as_f64() == 1.0);
}

TEST("vdoc - every value in a direct layer is attributed to its own writer id")
{
    auto layer = direct_layer("base");
    layer.set(path_of("e1", "T", "x"), value::of(1.0));

    auto const* const p = layer.document().try_get(path_of("e1", "T", "x"));
    REQUIRE(p != nullptr);
    REQUIRE(!p->is_multi_valued());
    CHECK(p->writers[0].writer == layer.writer());
    CHECK(layer.writer() == vdoc::synthetic_writer_id("base"));
}
