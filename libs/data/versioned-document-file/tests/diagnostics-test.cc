#include <nexus/test.hh>
#include <versioned-document-file/diagnostics.hh>

using namespace cc::primitive_defines;

namespace
{
vdoc::op_id id_of(u64 seed)
{
    return vdoc::op_id(cc::hash256{.l0 = seed, .l1 = seed * 3, .l2 = seed * 5, .l3 = seed * 7});
}
} // namespace

TEST("vdoc::file - an empty report is what a clean load produces")
{
    auto const report = vdoc::file::load_report();

    CHECK(report.is_empty());
    CHECK(report.count_of(vdoc::file::load_issue_kind::op_decode_failed) == 0);
    CHECK(!report.contains(vdoc::file::load_issue_kind::op_decode_failed));
    CHECK(report.find_first(vdoc::file::load_issue_kind::op_decode_failed) == nullptr);
}

TEST("vdoc::file - a report counts and finds by kind")
{
    auto report = vdoc::file::load_report();
    report.issues.push_back({.kind = vdoc::file::load_issue_kind::op_decode_failed, .op = id_of(1)});
    report.issues.push_back({.kind = vdoc::file::load_issue_kind::unknown_table, .name = "future_state"});
    report.issues.push_back({.kind = vdoc::file::load_issue_kind::op_decode_failed, .op = id_of(2)});

    CHECK(!report.is_empty());
    CHECK(report.count_of(vdoc::file::load_issue_kind::op_decode_failed) == 2);
    CHECK(report.count_of(vdoc::file::load_issue_kind::unknown_table) == 1);
    CHECK(report.count_of(vdoc::file::load_issue_kind::missing_parent) == 0);

    // find_first walks in load order, which is [the format](../docs/format.md#loading)'s, so it is deterministic without being sorted.
    auto const* first = report.find_first(vdoc::file::load_issue_kind::op_decode_failed);
    REQUIRE(first != nullptr);
    CHECK(first->op == id_of(1));

    report.clear();
    CHECK(report.is_empty());
}

TEST("vdoc::file - a hash mismatch is a distinct kind from a decode failure")
{
    // The two are never merged: a mismatch means corruption or tampering, a decode failure means bytes that never were an op.
    // A skeleton is unverifiable and reaches neither.
    auto const decode_failed
        = vdoc::file::load_issue{.kind = vdoc::file::load_issue_kind::op_decode_failed, .op = id_of(1)};
    auto const mismatch = vdoc::file::load_issue{.kind = vdoc::file::load_issue_kind::op_hash_mismatch, .op = id_of(1)};
    auto const same_again = vdoc::file::load_issue{.kind = vdoc::file::load_issue_kind::op_decode_failed, .op = id_of(1)};

    CHECK(decode_failed != mismatch);
    CHECK(decode_failed == same_again);
}

TEST("vdoc::file - an issue carries the id it concerns, and leaves the rest empty")
{
    // A missing parent names both ends: which op named it, and which id was named.
    auto const missing
        = vdoc::file::load_issue{.kind = vdoc::file::load_issue_kind::missing_parent, .op = id_of(9), .parent = id_of(4)};

    CHECK(missing.op == id_of(9));
    CHECK(missing.parent == id_of(4));
    CHECK(missing.blob == vdoc::file::blob_hash());
    CHECK(missing.name.empty());

    // A blob-side issue names the asset and the hash, and no op is involved.
    auto const orphan = vdoc::file::load_issue{.kind = vdoc::file::load_issue_kind::asset_blob_missing,
                                               .blob = vdoc::file::blob_hash(cc::hash256{.l0 = 17}),
                                               .name = "meshes/wall-panel"};

    CHECK(orphan.op == vdoc::op_id());
    CHECK(orphan.name == "meshes/wall-panel");
}
