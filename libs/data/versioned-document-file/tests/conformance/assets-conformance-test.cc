#include "store_fixture.hh"

#include <clean-core/string/format.hh>
#include <clean-core/thread/mutex.hh>
#include <nexus/test.hh>
#include <versioned-document/value_builder.hh>

/// The assets-and-blobs half of the conformance suite.
///
/// Same rule as the rest of it: every test runs on both store implementations, so a behaviour only one arm could
/// produce is not a behaviour this library has.

using namespace cc::primitive_defines;
using namespace vdoc::file;
using namespace vdoc::file::test;

namespace
{
store_handle open_or_fail(store_medium& medium)
{
    auto opened = medium.open();
    REQUIRE(opened.has_value());
    return cc::move(opened.value());
}

cc::vector<byte> bytes_of(cc::string_view text)
{
    return cc::vector<byte>::create_copy_of(cc::span<byte const>(reinterpret_cast<byte const*>(text.data()), text.size()));
}

cc::string text_of(cc::span<byte const> bytes)
{
    auto out = cc::string();
    for (auto const b : bytes)
        out += char(b);
    return out;
}

/// A payload big enough to span several chunks, with position-dependent content so a misread range is visible.
cc::vector<byte> large_payload(isize size)
{
    auto out = cc::vector<byte>();
    out.resize_to_uninitialized(size);
    for (isize i = 0; i < size; ++i)
        out[i] = byte(u8((i * 31 + 7) & 0xFF));
    return out;
}

blob_upload upload_of(cc::span<byte const> decoded, cc::string_view format = "bin")
{
    auto made = blob_upload::of(decoded, format);
    REQUIRE(made.has_value());
    return cc::move(made.value());
}

asset_record asset_of(cc::string_view id, cc::span<blob_upload const> parts, cc::string_view kind = "mesh")
{
    auto record = asset_record{.asset_id = cc::string(id), .kind = cc::string(kind)};
    for (auto const& part : parts)
        record.parts.push_back({.hash = part.hash, .format = part.format});
    return record;
}
} // namespace

INVOCABLE_TEST("vdoc::file - identical bytes are stored once, whatever route they arrive by", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const payload = bytes_of("the very same bytes");

    {
        auto const s = open_or_fail(*medium);
        auto const blob = upload_of(payload);

        // Two assets, one payload: sharing is the point rather than an optimization, so one blob is the assertion.
        auto const published = wait_for(s->publish({.assets = {asset_of("a", cc::span<blob_upload const>(&blob, 1)),
                                                               asset_of("b", cc::span<blob_upload const>(&blob, 1))},
                                                    .blobs = {blob, blob}}));
        REQUIRE(published.has_value());
        CHECK(published.value().blobs_written == 1);
        s->close();
    }

    CHECK(medium->count_blobs() == 1);

    // A second publish of the same bytes writes nothing at all, on a store that learned the hash from a LOAD rather
    // than from having written it.
    auto const reopened = open_or_fail(*medium);
    auto const again = wait_for(reopened->publish({.blobs = {upload_of(payload)}}));
    REQUIRE(again.has_value());
    CHECK(again.value().blobs_written == 0);
    CHECK(medium->count_blobs() == 1);
}

INVOCABLE_TEST("vdoc::file - deleting one of two assets leaves the other's blob intact", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const payload = bytes_of("shared between two assets");

    auto const s = open_or_fail(*medium);
    auto const blob = upload_of(payload);
    auto const published = wait_for(s->publish({.assets = {asset_of("kept", cc::span<blob_upload const>(&blob, 1)),
                                                           asset_of("dropped", cc::span<blob_upload const>(&blob, 1))},
                                                .blobs = {blob}}));
    REQUIRE(published.has_value());

    auto const roots = cc::vector<cc::string>{cc::string("kept")};
    auto const reclaimed = wait_for(s->reclaim(roots));
    REQUIRE(reclaimed.has_value());
    CHECK(reclaimed.value().assets_removed == 1);
    CHECK(reclaimed.value().blobs_removed == 0); // still named by "kept"

    CHECK(medium->count_blobs() == 1);
    auto const source = s->make_blob_source();
    auto const fetched = wait_for(*s, source->load(blob.hash));
    REQUIRE(fetched.has_value());
    CHECK(text_of(fetched.value()) == text_of(payload));
}

INVOCABLE_TEST("vdoc::file - ordered parts round-trip exactly", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const first = upload_of(bytes_of("part one"), "png");
    auto const second = upload_of(bytes_of("part two"), "bin");
    auto const shared = upload_of(bytes_of("shared with the other asset"), "raw-bytes");

    {
        auto const s = open_or_fail(*medium);

        // Deliberately NOT in hash order: stored order is kept verbatim, so what comes back must be what went in.
        auto ordered = asset_record{.asset_id = "ordered", .kind = "mesh"};
        ordered.parts.push_back({.hash = second.hash, .format = second.format, .name = "second"});
        ordered.parts.push_back({.hash = first.hash, .format = first.format, .name = "first"});
        ordered.parts.push_back({.hash = shared.hash, .format = shared.format});

        // An asset with NO parts is legal, and means metadata without bytes.
        auto bare = asset_record{.asset_id = "bare", .kind = "marker"};
        bare.meta = vdoc::value_builder::object().set("note", cc::string_view("no bytes here")).build();

        auto neighbour = asset_record{.asset_id = "neighbour", .kind = "mesh"};
        neighbour.parts.push_back({.hash = shared.hash, .format = shared.format});

        auto const published
            = wait_for(s->publish({.assets = {ordered, bare, neighbour}, .blobs = {first, second, shared}}));
        REQUIRE(published.has_value());
        CHECK(published.value().blobs_written == 3);
        s->close();
    }

    auto const reopened = open_or_fail(*medium);
    CHECK(reopened->report().is_empty());

    auto const* ordered = reopened->assets().get_ptr(cc::string("ordered"));
    REQUIRE(ordered != nullptr);
    REQUIRE(ordered->parts.size() == 3);
    CHECK(ordered->parts[0].hash == second.hash);
    CHECK(ordered->parts[1].hash == first.hash);
    CHECK(ordered->parts[2].hash == shared.hash);
    CHECK(ordered->parts[0].format == "bin");
    CHECK(ordered->is_resolvable);

    auto const* bare = reopened->assets().get_ptr(cc::string("bare"));
    REQUIRE(bare != nullptr);
    CHECK(bare->parts.empty());
    CHECK(bare->is_resolvable); // no parts is not the same as parts whose blobs are missing

    // Addressed by NAME, and the third part carried no name so it defaults to $main.
    auto const named = ordered->try_find_part("first");
    REQUIRE(named.has_value());
    CHECK(named.value()->hash == first.hash);
    CHECK(ordered->main_part().has_value());
    CHECK(ordered->main_part().value()->hash == shared.hash);
}

INVOCABLE_TEST("vdoc::file - part names are the contract, and position within a name disambiguates",
               (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const lod0 = upload_of(bytes_of("the full mesh"));
    auto const lod1 = upload_of(bytes_of("the half mesh"));
    auto const preview = upload_of(bytes_of("a thumbnail"), "png");

    {
        auto const s = open_or_fail(*medium);
        auto record = asset_record{.asset_id = "wall", .kind = "mesh"};

        // Deliberately interleaved, so a lookup that quietly depended on whole-list position would come out wrong.
        record.parts.push_back({.hash = lod0.hash, .format = lod0.format, .name = "lod"});
        record.parts.push_back({.hash = preview.hash, .format = preview.format, .name = "preview"});
        record.parts.push_back({.hash = lod1.hash, .format = lod1.format, .name = "lod"});

        REQUIRE(wait_for(s->publish({.assets = {record}, .blobs = {lod0, lod1, preview}})).has_value());
        s->close();
    }

    auto const reopened = open_or_fail(*medium);

    // Two parts share "lod", which is ordinary and is NOT reported: only a name that looks singular is.
    CHECK(!reopened->report().contains(load_issue_kind::asset_part_unnamed));

    auto const* record = reopened->assets().get_ptr(cc::string("wall"));
    REQUIRE(record != nullptr);

    // A unique name resolves to exactly one part, wherever it sits in the list.
    auto const found = record->try_find_part("preview");
    REQUIRE(found.has_value());
    CHECK(found.value()->hash == preview.hash);

    // A shared name is ambiguous rather than "the first one" — THIS is the rule's enforcement.
    auto const ambiguous = record->try_find_part("lod");
    REQUIRE(ambiguous.has_error());
    CHECK(ambiguous.error() == part_lookup_error::ambiguous);

    // A name nothing carries is a different failure from a name several carry.
    auto const absent = record->try_find_part("collision");
    REQUIRE(absent.has_error());
    CHECK(absent.error() == part_lookup_error::not_found);

    // Index disambiguates WITHIN the name, in declaration order.
    auto const range = record->parts_named("lod");
    REQUIRE(range.size() == 2);
    CHECK(range[0].hash == lod0.hash);
    CHECK(range[1].hash == lod1.hash);

    auto seen = isize(0);
    for (auto const& part : range)
    {
        CHECK(part.name == "lod");
        ++seen;
    }
    CHECK(seen == 2);

    REQUIRE(record->part_at("lod", 1).has_value());
    CHECK(record->part_at("lod", 1).value()->hash == lod1.hash);
    CHECK(!record->part_at("lod", 2).has_value()); // an explicit index runs out rather than erroring
    CHECK(!record->part_at("nothing", 0).has_value());

    CHECK(record->parts_named("nothing").empty());
}

INVOCABLE_TEST("vdoc::file - a single-part asset costs no ceremony and round-trips as $main", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const blob = upload_of(bytes_of("the only bytes there are"));

    {
        auto const s = open_or_fail(*medium);

        // No name given anywhere: the default carries it, which is the whole point of $main.
        auto record = asset_record{.asset_id = "solo", .kind = "mesh"};
        record.parts.push_back({.hash = blob.hash, .format = blob.format});

        REQUIRE(wait_for(s->publish({.assets = {record}, .blobs = {blob}})).has_value());
        s->close();
    }

    auto const reopened = open_or_fail(*medium);
    CHECK(reopened->report().is_empty());

    auto const* record = reopened->assets().get_ptr(cc::string("solo"));
    REQUIRE(record != nullptr);
    CHECK(record->parts[0].name == main_part_name);

    auto const main = record->main_part();
    REQUIRE(main.has_value());
    CHECK(main.value()->hash == blob.hash);

    auto const source = reopened->make_blob_source();
    auto const fetched = wait_for(*reopened, source->load(main.value()->hash));
    REQUIRE(fetched.has_value());
    CHECK(text_of(fetched.value()) == "the only bytes there are");
}

INVOCABLE_TEST("vdoc::file - several $main parts are reported and then error rather than picking one",
               (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const first = upload_of(bytes_of("first unnamed"));
    auto const second = upload_of(bytes_of("second unnamed"));

    {
        auto const s = open_or_fail(*medium);

        // A publish that never named anything: both parts land on $main, which is a broken asset rather than a choice.
        auto record = asset_record{.asset_id = "muddled", .kind = "mesh"};
        record.parts.push_back({.hash = first.hash, .format = first.format});
        record.parts.push_back({.hash = second.hash, .format = second.format});

        REQUIRE(wait_for(s->publish({.assets = {record}, .blobs = {first, second}})).has_value());
        s->close();
    }

    auto const reopened = open_or_fail(*medium);
    CHECK(reopened->report().contains(load_issue_kind::asset_duplicate_part_name));

    auto const* record = reopened->assets().get_ptr(cc::string("muddled"));
    REQUIRE(record != nullptr);

    // BOTH parts survive the load — dropping one would make the error below unreachable and lose the caller's data.
    REQUIRE(record->parts.size() == 2);

    auto const main = record->main_part();
    REQUIRE(main.has_error());
    CHECK(main.error() == part_lookup_error::ambiguous);

    // main_parts() is the escape hatch: it is what a caller reaches for to see what it actually published.
    auto const all = record->main_parts();
    REQUIRE(all.size() == 2);
    CHECK(all[0].hash == first.hash);
    CHECK(all[1].hash == second.hash);
}

INVOCABLE_TEST("vdoc::file - an empty part name is reported", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const blob = upload_of(bytes_of("addressed by nothing"));

    {
        auto const s = open_or_fail(*medium);
        auto record = asset_record{.asset_id = "nameless", .kind = "mesh"};

        // Only reachable by asking for it, since the default is $main — so it is a mistake worth naming.
        record.parts.push_back({.hash = blob.hash, .format = blob.format, .name = ""});

        REQUIRE(wait_for(s->publish({.assets = {record}, .blobs = {blob}})).has_value());
        s->close();
    }

    auto const reopened = open_or_fail(*medium);
    CHECK(reopened->report().contains(load_issue_kind::asset_part_unnamed));

    // Reported, kept, and still distinguishable from $main: the empty name round-trips as itself.
    auto const* record = reopened->assets().get_ptr(cc::string("nameless"));
    REQUIRE(record != nullptr);
    REQUIRE(record->parts.size() == 1);
    CHECK(record->parts[0].name.empty());
    CHECK(record->main_part().has_error());
}

INVOCABLE_TEST("vdoc::file - a blob spanning several chunks round-trips, at an offset and across a boundary",
               (store_impl const& impl))
{
    auto const medium = impl.make_medium();

    // Comfortably past the 1 MiB chunk size, and not a multiple of it, so the last chunk is partial.
    auto const payload = large_payload((1 << 20) * 2 + 12345);

    {
        auto const s = open_or_fail(*medium);
        auto const blob = upload_of(payload);
        auto const published
            = wait_for(s->publish({.assets = {asset_of("big", cc::span<blob_upload const>(&blob, 1))}, .blobs = {blob}}));
        REQUIRE(published.has_value());
        s->close();
    }

    auto const reopened = open_or_fail(*medium);
    CHECK(reopened->report().is_empty());

    auto const* record = reopened->assets().get_ptr(cc::string("big"));
    REQUIRE(record != nullptr);
    auto const hash = record->parts[0].hash;

    auto const source = reopened->make_blob_source();

    auto const whole = wait_for(*reopened, source->load(hash));
    REQUIRE(whole.has_value());
    REQUIRE(whole.value().size() == payload.size());
    for (isize i = 0; i < payload.size(); ++i)
        REQUIRE(whole.value()[i] == payload[i]);

    // A range wholly inside the first chunk.
    auto const head = wait_for(*reopened, source->load_range(hash, 16, 64));
    REQUIRE(head.has_value());
    REQUIRE(head.value().size() == 64);
    for (isize i = 0; i < 64; ++i)
        REQUIRE(head.value()[i] == payload[16 + i]);

    // A range that STRADDLES a chunk boundary, which is the case chunking makes possible to get wrong.
    auto const boundary = (1 << 20);
    auto const across = wait_for(*reopened, source->load_range(hash, boundary - 100, 200));
    REQUIRE(across.has_value());
    REQUIRE(across.value().size() == 200);
    for (isize i = 0; i < 200; ++i)
        REQUIRE(across.value()[i] == payload[boundary - 100 + i]);

    // A negative size means "to the end", and the tail lives in the partial last chunk.
    auto const tail = wait_for(*reopened, source->load_range(hash, payload.size() - 50, -1));
    REQUIRE(tail.has_value());
    REQUIRE(tail.value().size() == 50);
    for (isize i = 0; i < 50; ++i)
        REQUIRE(tail.value()[i] == payload[payload.size() - 50 + i]);

    // Past the end is an error rather than an empty answer, so a bad offset cannot read as an empty blob.
    CHECK(wait_for(*reopened, source->load_range(hash, payload.size() + 1, 10)).has_error());
}

INVOCABLE_TEST("vdoc::file - a torn blob is reported and the file still opens", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const payload = large_payload((1 << 20) + 500);

    {
        auto const s = open_or_fail(*medium);
        auto const blob = upload_of(payload);
        auto const published = wait_for(
            s->publish({.assets = {asset_of("torn", cc::span<blob_upload const>(&blob, 1))}, .blobs = {blob}}));
        REQUIRE(published.has_value());
        s->close();
    }

    REQUIRE(medium->delete_first_blob_chunk());

    auto const reopened = open_or_fail(*medium);
    CHECK(reopened->report().contains(load_issue_kind::asset_blob_incomplete));

    auto const* record = reopened->assets().get_ptr(cc::string("torn"));
    REQUIRE(record != nullptr);
    CHECK(!record->is_resolvable);

    // Reading it reports the tear rather than handing back a short fill that would look like valid bytes.
    auto const source = reopened->make_blob_source();
    CHECK(wait_for(*reopened, source->load(record->parts[0].hash)).has_error());
}

INVOCABLE_TEST("vdoc::file - an empty upload is a no-op when present and an error when absent", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const payload = bytes_of("already stored");
    auto const blob = upload_of(payload);

    auto const s = open_or_fail(*medium);
    REQUIRE(wait_for(s->publish({.blobs = {blob}})).has_value());

    // "You already have this": nothing is read back just to be rewritten.
    auto empty = blob;
    empty.has_data = false;
    empty.data = {};
    auto const republished
        = wait_for(s->publish({.assets = {asset_of("a", cc::span<blob_upload const>(&blob, 1))}, .blobs = {empty}}));
    REQUIRE(republished.has_value());
    CHECK(republished.value().blobs_written == 0);
    CHECK(medium->count_blobs() == 1);

    // The same claim about bytes nothing has is a publish error, caught before a transaction is ever opened.
    auto unknown = upload_of(bytes_of("never stored anywhere"));
    unknown.has_data = false;
    unknown.data = {};
    CHECK(wait_for(s->publish({.blobs = {unknown}})).has_error());
}

INVOCABLE_TEST("vdoc::file - an unknown encoding is skipped and the rest of the file loads", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_sample_history();

    {
        auto const s = open_or_fail(*medium);
        copy_ops_into(*s, history.graph, history.ops);
        auto const blob = upload_of(bytes_of("bytes under an encoding from the future"));
        auto const published = wait_for(s->publish({.refs = {{cc::string("main"), history.head()}},
                                                    .assets = {asset_of("future", cc::span<blob_upload const>(&blob, 1))},
                                                    .blobs = {blob}}));
        REQUIRE(published.has_value());
        s->close();
    }

    REQUIRE(medium->set_first_blob_encoding("brotli-from-2030"));

    // A load issue, never a failed open: the document is entirely readable, and one blob is not.
    auto const reopened = open_or_fail(*medium);
    CHECK(reopened->report().contains(load_issue_kind::unknown_encoding));
    CHECK(reopened->ops().size() == history.ops.size());

    auto const* record = reopened->assets().get_ptr(cc::string("future"));
    REQUIRE(record != nullptr);
    CHECK(!record->is_resolvable);
}

INVOCABLE_TEST("vdoc::file - publishing under an encoding this build lacks is a publish error", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const s = open_or_fail(*medium);

    // The asymmetry with the load path, pinned: a FILE naming an unknown codec is an issue, a CALLER asking for one is
    // an error, because that is this build asking itself for something it cannot do.
    CHECK(blob_upload::of(bytes_of("x"), "bin", "brotli-from-2030").has_error());

    auto made_up = upload_of(bytes_of("plain bytes"));
    made_up.encoding = "brotli-from-2030";
    CHECK(wait_for(s->publish({.blobs = {made_up}})).has_error());
}

INVOCABLE_TEST("vdoc::file - a blob source after close reports rather than hangs", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const payload = bytes_of("fetched before and after");

    auto const s = open_or_fail(*medium);
    auto const blob = upload_of(payload);
    REQUIRE(wait_for(s->publish({.blobs = {blob}})).has_value());

    auto const source = s->make_blob_source();
    REQUIRE(wait_for(*s, source->load(blob.hash)).has_value());

    s->close();
    CHECK(source->is_severed());

    // Completing with an error is what a dead handle owes a caller; hanging is not.
    auto const after = source->load(blob.hash);
    REQUIRE(after->is_ready());
    CHECK(after->has_error());
    CHECK(source->load_range(blob.hash, 0, 4)->has_error());
}

INVOCABLE_TEST("vdoc::file - a blob fetch under a held caller lock does not deadlock", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const payload = bytes_of("fetched from under a lock");

    auto const s = open_or_fail(*medium);
    auto const blob = upload_of(payload);
    REQUIRE(wait_for(s->publish({.blobs = {blob}})).has_value());

    auto const source = s->make_blob_source();

    // The contract this pins is enqueue-and-return: load() may be called with a caller's own lock held, so it must
    // neither block nor run the fetch on this thread.
    // The lock is genuinely held ACROSS the call, so a store that ran the fetch inline — and with it any continuation
    // reaching back into the caller — would self-deadlock here rather than merely be slow.
    // Note what is deliberately NOT asserted: whether the async is already resolved when load() returns.
    // The file arm has a thread of its own and may legitimately have finished, so checking that would be a race
    // rather than a contract.
    auto caller_state = cc::mutex<bool>(false);

    auto fetched = cc::shared_async<cc::vector<byte>>();
    {
        auto guard = caller_state.lock_scoped();
        fetched = source->load(blob.hash);
        *guard = true;
    }

    auto const result = wait_for(*s, fetched);
    REQUIRE(result.has_value());
    CHECK(text_of(result.value()) == text_of(payload));

    // Re-taking the lock proves nothing on the fetch path parked while still holding it.
    auto const seen = caller_state.lock_scoped();
    CHECK(*seen);
}

INVOCABLE_TEST("vdoc::file - reclamation keeps the dependency closure of its roots", (store_impl const& impl))
{
    auto const medium = impl.make_medium();

    auto const root_blob = upload_of(bytes_of("the root's own bytes"));
    auto const near_blob = upload_of(bytes_of("a direct dependency"));
    auto const far_blob = upload_of(bytes_of("reached only transitively"));
    auto const orphan_blob = upload_of(bytes_of("reached from nothing"));

    {
        auto const s = open_or_fail(*medium);

        auto root = asset_of("scene", cc::span<blob_upload const>(&root_blob, 1));
        root.dependencies = {cc::string("material"), cc::string("a-builtin-that-lives-elsewhere")};

        auto near = asset_of("material", cc::span<blob_upload const>(&near_blob, 1));
        near.dependencies = {cc::string("texture"), cc::string("scene")}; // a CYCLE back to the root, which is ordinary

        auto far = asset_of("texture", cc::span<blob_upload const>(&far_blob, 1));
        auto orphan = asset_of("abandoned", cc::span<blob_upload const>(&orphan_blob, 1));

        auto const published = wait_for(
            s->publish({.assets = {root, near, far, orphan}, .blobs = {root_blob, near_blob, far_blob, orphan_blob}}));
        REQUIRE(published.has_value());
        s->close();
    }

    {
        auto const s = open_or_fail(*medium);

        // A dependency naming something outside this file is skipped in silence, never reported: a file is one asset
        // source among many.
        CHECK(s->report().is_empty());

        auto const* root = s->assets().get_ptr(cc::string("scene"));
        REQUIRE(root != nullptr);
        CHECK(root->dependencies.size() == 2);

        auto const roots = cc::vector<cc::string>{cc::string("scene")};
        auto const reclaimed = wait_for(s->reclaim(roots));
        REQUIRE(reclaimed.has_value());
        CHECK(reclaimed.value().assets_removed == 1); // "abandoned" alone
        CHECK(reclaimed.value().blobs_removed == 1);
        s->close();
    }

    auto const reopened = open_or_fail(*medium);
    CHECK(reopened->assets().contains(cc::string("scene")));
    CHECK(reopened->assets().contains(cc::string("material")));
    CHECK(reopened->assets().contains(cc::string("texture"))); // kept transitively
    CHECK(!reopened->assets().contains(cc::string("abandoned")));

    // The orphan's blob went with it, and its chunks followed by cascade; the three reachable ones stayed.
    CHECK(medium->count_blobs() == 3);
}

INVOCABLE_TEST("vdoc::file - a remap that orphans a blob is a normal outcome", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const before = upload_of(bytes_of("the placeholder"));
    auto const after = upload_of(bytes_of("the replacement"));

    auto const s = open_or_fail(*medium);
    REQUIRE(
        wait_for(s->publish({.assets = {asset_of("mesh", cc::span<blob_upload const>(&before, 1))}, .blobs = {before}}))
            .has_value());

    // Re-pointing the name leaves the old blob named by nothing.
    REQUIRE(wait_for(s->publish({.assets = {asset_of("mesh", cc::span<blob_upload const>(&after, 1))}, .blobs = {after}}))
                .has_value());
    CHECK(medium->count_blobs() == 2); // the remap itself collects nothing

    // Collecting it is the case reclamation exists for, not a bug to prevent — so the asset survives and the blob goes.
    auto const roots = cc::vector<cc::string>{cc::string("mesh")};
    auto const reclaimed = wait_for(s->reclaim(roots));
    REQUIRE(reclaimed.has_value());
    CHECK(reclaimed.value().assets_removed == 0);
    CHECK(reclaimed.value().blobs_removed == 1);
    CHECK(medium->count_blobs() == 1);

    auto const source = s->make_blob_source();
    CHECK(wait_for(*s, source->load(after.hash)).has_value());
    CHECK(wait_for(*s, source->load(before.hash)).has_error()); // the orphan really is gone
}

INVOCABLE_TEST("vdoc::file - an unreadable dependency list costs precision, never the asset", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const blob = upload_of(bytes_of("still perfectly readable"));

    {
        auto const s = open_or_fail(*medium);
        auto record = asset_of("scene", cc::span<blob_upload const>(&blob, 1));
        record.dependencies = {cc::string("material")};
        REQUIRE(wait_for(s->publish({.assets = {record}, .blobs = {blob}})).has_value());
        s->close();
    }

    REQUIRE(medium->corrupt_first_asset_deps());

    auto const reopened = open_or_fail(*medium);

    // Reported, and the asset is still THERE with its parts intact: a dependency list is advisory, so an unreadable
    // one costs a sweep precision rather than costing the reader the asset.
    CHECK(reopened->report().contains(load_issue_kind::asset_decode_failed));

    auto const* record = reopened->assets().get_ptr(cc::string("scene"));
    REQUIRE(record != nullptr);
    CHECK(record->parts.size() == 1);
    CHECK(record->parts[0].hash == blob.hash);
    CHECK(record->dependencies.empty()); // reads exactly as an absent list would
    CHECK(record->is_resolvable);
}

INVOCABLE_TEST("vdoc::file - removing an asset unmaps it without collecting a shared blob", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const shared = upload_of(bytes_of("named by two assets"));

    {
        auto const s = open_or_fail(*medium);
        auto const published = wait_for(s->publish({.assets = {asset_of("one", cc::span<blob_upload const>(&shared, 1)),
                                                               asset_of("two", cc::span<blob_upload const>(&shared, 1))},
                                                    .blobs = {shared}}));
        REQUIRE(published.has_value());

        // Retroactive exactly like a remap: no op, no ref, and no bytes collected.
        auto const removed = wait_for(s->publish({.removed_assets = {cc::string("two")}}));
        REQUIRE(removed.has_value());
        CHECK(!s->assets().contains(cc::string("two")));
        s->close();
    }

    auto const reopened = open_or_fail(*medium);
    CHECK(reopened->assets().contains(cc::string("one")));
    CHECK(!reopened->assets().contains(cc::string("two")));
    CHECK(medium->count_blobs() == 1); // only a reclamation collects bytes
}

INVOCABLE_TEST("vdoc::file - remapping an asset is retroactive", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_sample_history();
    auto const before = upload_of(bytes_of("the placeholder mesh"));
    auto const after = upload_of(bytes_of("the finished mesh"));

    {
        auto const s = open_or_fail(*medium);
        copy_ops_into(*s, history.graph, history.ops);
        REQUIRE(wait_for(s->publish({.refs = {{cc::string("main"), history.head()}},
                                     .assets = {asset_of("meshes/wall", cc::span<blob_upload const>(&before, 1))},
                                     .blobs = {before}}))
                    .has_value());
        s->close();
    }

    auto const old_head = history.head();

    {
        auto const s = open_or_fail(*medium);

        // Re-pointing the name creates NO op and moves NO ref — it is in the same category as a workspace write.
        auto const saved_before = s->is_saved(old_head);
        REQUIRE(wait_for(s->publish({.assets = {asset_of("meshes/wall", cc::span<blob_upload const>(&after, 1))},
                                     .blobs = {after}}))
                    .has_value());
        CHECK(s->is_saved(old_head) == saved_before);
        s->close();
    }

    auto const reopened = open_or_fail(*medium);

    // The OLD head still materializes, and resolves the same name to the NEW content.
    // This is the design's one deliberate hole in immutability, so it is pinned rather than left to be discovered.
    CHECK(reopened->ops().contains(old_head));

    auto resolved = reopened->resolve_asset("meshes/wall");
    REQUIRE(resolved.has_value());
    REQUIRE(resolved.value().record.parts.size() == 1);
    CHECK(resolved.value().record.parts[0].hash == after.hash);

    auto const fetched = wait_for(*reopened, resolved.value().blobs->load(resolved.value().record.parts[0].hash));
    REQUIRE(fetched.has_value());
    CHECK(text_of(fetched.value()) == "the finished mesh");
}

INVOCABLE_TEST("vdoc::file - resolving an asset stops at metadata, parts and a source", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const blob = upload_of(bytes_of("resolved through a source"), "png");

    auto const s = open_or_fail(*medium);
    auto record = asset_of("icons/save", cc::span<blob_upload const>(&blob, 1), "texture");
    record.meta = vdoc::value_builder::object().set("author", cc::string_view("someone")).build();
    REQUIRE(wait_for(s->publish({.assets = {record}, .blobs = {blob}})).has_value());

    CHECK(!s->resolve_asset("nothing/here").has_value());

    auto resolved = s->resolve_asset("icons/save");
    REQUIRE(resolved.has_value());
    CHECK(resolved.value().record.kind == "texture");
    CHECK(resolved.value().record.parts[0].format == "png");
    REQUIRE(resolved.value().blobs != nullptr);

    // The record is a COPY, so a later publish that rewrites the index cannot make it dangle.
    auto const replacement = upload_of(bytes_of("something else entirely"), "png");
    REQUIRE(wait_for(s->publish({.assets = {asset_of("icons/save", cc::span<blob_upload const>(&replacement, 1))},
                                 .blobs = {replacement}}))
                .has_value());
    CHECK(resolved.value().record.parts[0].hash == blob.hash);

    auto const fetched = wait_for(*s, resolved.value().blobs->load(blob.hash));
    REQUIRE(fetched.has_value());
    CHECK(text_of(fetched.value()) == "resolved through a source");
}
