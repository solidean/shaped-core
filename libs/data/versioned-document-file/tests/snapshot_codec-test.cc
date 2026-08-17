#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <versioned-document-file/impl/snapshot_codec.hh>
#include <versioned-document/op_builder.hh>
#include <versioned-document/op_graph.hh>
#include <versioned-document/value_builder.hh>

/// The snapshot encoding on its own, with no store anywhere near it.
///
/// A conformance failure says "the file would not reopen"; these say which half of the codec is wrong, which is the
/// difference between a minute and an afternoon.

using namespace cc::primitive_defines;
using namespace vdoc::file::impl;

namespace
{
/// Two documents compared exactly: same paths in the same order, same writers in the same order, same value bytes.
[[nodiscard]] bool same_document(vdoc::raw_document const& a, vdoc::raw_document const& b)
{
    if (a.entities.size() != b.entities.size())
        return false;

    for (isize ei = 0; ei < a.entities.size(); ++ei)
    {
        auto const& ea = a.entities[ei];
        auto const& eb = b.entities[ei];
        if (!(ea.entity == eb.entity) || ea.value.components.size() != eb.value.components.size())
            return false;

        for (isize ci = 0; ci < ea.value.components.size(); ++ci)
        {
            auto const& ca = ea.value.components[ci];
            auto const& cb = eb.value.components[ci];
            if (!(ca.component == cb.component) || ca.value.properties.size() != cb.value.properties.size())
                return false;

            for (isize pi = 0; pi < ca.value.properties.size(); ++pi)
            {
                auto const& pa = ca.value.properties[pi];
                auto const& pb = cb.value.properties[pi];
                if (!(pa.property == pb.property) || pa.value.writers.size() != pb.value.writers.size())
                    return false;

                for (isize wi = 0; wi < pa.value.writers.size(); ++wi)
                {
                    auto const& wa = pa.value.writers[wi];
                    auto const& wb = pb.value.writers[wi];
                    if (!(wa.writer == wb.writer))
                        return false;

                    auto const ba = wa.value.bytes();
                    auto const bb = wb.value.bytes();
                    if (ba.size() != bb.size())
                        return false;
                    for (isize i = 0; i < ba.size(); ++i)
                        if (ba[i] != bb[i])
                            return false;
                }
            }
        }
    }

    return true;
}

/// A document with several entities, components, properties and value kinds, plus a genuinely multi-valued property.
[[nodiscard]] vdoc::raw_document build_document(vdoc::op_graph& graph, cc::vector<vdoc::op_id>& heads)
{
    auto const wall = vdoc::entity_id::of("wall-17");
    auto const door = vdoc::entity_id::of("door-3");
    auto const transform = vdoc::component_type_id::of("transform");
    auto const mesh = vdoc::component_type_id::of("mesh");

    auto root = vdoc::op_builder();
    root.set_raw(wall, transform, vdoc::property_id::of("x"), vdoc::value::of(1.5));
    root.set_raw(wall, transform, vdoc::property_id::of("name"), vdoc::value::of("north wall"));
    root.set_raw(wall, mesh, vdoc::property_id::of("asset"), vdoc::value::of("meshes/wall"));
    root.set_raw(door, transform, vdoc::property_id::of("open"), vdoc::value::of(true));
    auto const root_id = graph.add(root.build(graph));

    // Two concurrent writers of one path, so the encoding has to carry a multi-valued property.
    auto left = vdoc::op_builder();
    left.set_parents(cc::span<vdoc::op_id const>(&root_id, 1));
    left.set_raw(door, transform, vdoc::property_id::of("angle"), vdoc::value::of(i64(30)));
    auto const left_id = graph.add(left.build(graph));

    auto right = vdoc::op_builder();
    right.set_parents(cc::span<vdoc::op_id const>(&root_id, 1));
    right.set_raw(door, transform, vdoc::property_id::of("angle"), vdoc::value::of(i64(90)));
    auto const right_id = graph.add(right.build(graph));

    heads.push_back(left_id);
    heads.push_back(right_id);
    return graph.materialize(heads);
}
} // namespace

TEST("vdoc::file - a snapshot round-trips through its codec")
{
    auto graph = vdoc::op_graph();
    auto heads = cc::vector<vdoc::op_id>();
    auto const doc = build_document(graph, heads);

    // The shape being encoded has to be worth encoding, or the round-trip proves nothing.
    REQUIRE(doc.entities.size() == 2);
    auto const angle = vdoc::property_path{.entity = vdoc::entity_id::of("door-3"),
                                           .component = vdoc::component_type_id::of("transform"),
                                           .property = vdoc::property_id::of("angle")};
    REQUIRE(doc.try_get(angle) != nullptr);
    CHECK(doc.try_get(angle)->is_multi_valued());

    auto const bytes = encode_snapshot(doc);
    auto decoded = try_decode_snapshot(snapshot_encoding_v1, bytes);
    REQUIRE(decoded.has_value());

    CHECK(same_document(decoded.value().document(), doc));

    // The snapshot owns its bytes, so it survives the graph that produced it.
    auto const owned = decoded.value().owned_byte_size();
    graph = vdoc::op_graph();
    CHECK(owned > 0);
    CHECK(same_document(decoded.value().document(), decoded.value().document()));
}

TEST("vdoc::file - the snapshot encoding is canonical")
{
    auto graph = vdoc::op_graph();
    auto heads = cc::vector<vdoc::op_id>();
    auto const doc = build_document(graph, heads);

    // Two encodings of the same document are byte-identical, which is what makes a round-trip test a memcmp.
    auto const a = encode_snapshot(doc);
    auto const b = encode_snapshot(doc);
    REQUIRE(a.size() == b.size());
    for (isize i = 0; i < a.size(); ++i)
        CHECK(a[i] == b[i]);

    // And re-encoding what was decoded reproduces the same bytes.
    auto decoded = try_decode_snapshot(snapshot_encoding_v1, a);
    REQUIRE(decoded.has_value());
    auto const again = encode_snapshot(decoded.value().document());
    REQUIRE(again.size() == a.size());
    for (isize i = 0; i < a.size(); ++i)
        CHECK(again[i] == a[i]);
}

TEST("vdoc::file - an empty document round-trips")
{
    auto const doc = vdoc::raw_document();
    auto const bytes = encode_snapshot(doc);

    auto decoded = try_decode_snapshot(snapshot_encoding_v1, bytes);
    REQUIRE(decoded.has_value());
    CHECK(decoded.value().document().entities.size() == 0);
    CHECK(decoded.value().owned_byte_size() == bytes.size());
}

TEST("vdoc::file - an unknown encoding is refused rather than guessed at")
{
    auto graph = vdoc::op_graph();
    auto heads = cc::vector<vdoc::op_id>();
    auto const bytes = encode_snapshot(build_document(graph, heads));

    auto const decoded = try_decode_snapshot("snapshot-v99", bytes);
    REQUIRE(decoded.has_error());
    CHECK(decoded.error() == snapshot_decode_error::unknown_encoding);
}

TEST("vdoc::file - a truncated snapshot is a decode error, never a crash")
{
    auto graph = vdoc::op_graph();
    auto heads = cc::vector<vdoc::op_id>();
    auto const bytes = encode_snapshot(build_document(graph, heads));

    // Every prefix must be refused, which is the whole of what "total" means for a decoder.
    for (isize n = 0; n < bytes.size(); ++n)
    {
        auto const prefix = cc::span<byte const>(bytes).subspan({.offset = 0, .size = n});
        CHECK(try_decode_snapshot(snapshot_encoding_v1, prefix).has_error());
    }
}

TEST("vdoc::file - trailing bytes are refused")
{
    auto graph = vdoc::op_graph();
    auto heads = cc::vector<vdoc::op_id>();
    auto bytes = encode_snapshot(build_document(graph, heads));
    bytes.push_back(byte(0));

    auto const decoded = try_decode_snapshot(snapshot_encoding_v1, bytes);
    REQUIRE(decoded.has_error());
    CHECK(decoded.error() == snapshot_decode_error::trailing_bytes);
}

TEST("vdoc::file - garbage is a decode error, never a crash")
{
    // A deliberately stupid sweep: the decoder must be total over arbitrary bytes, not merely over damaged snapshots.
    auto state = u32(12345);
    auto const next = [&]
    {
        state = state * 1664525u + 1013904223u;
        return byte((state >> 16) & 0xFF);
    };

    for (isize round = 0; round < 200; ++round)
    {
        auto bytes = cc::vector<byte>();
        for (isize i = 0; i < 64; ++i)
            bytes.push_back(next());

        // Whatever it decides, it must decide it — and must not read past what it was given.
        (void)try_decode_snapshot(snapshot_encoding_v1, bytes);
    }

    CHECK(true);
}
