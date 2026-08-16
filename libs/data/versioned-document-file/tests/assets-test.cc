#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <versioned-document-file/assets.hh>

using namespace cc::primitive_defines;

namespace
{
vdoc::file::blob_hash hash_of(cc::string_view text)
{
    return vdoc::file::blob_hash::of(cc::span<byte const>(reinterpret_cast<byte const*>(text.data()), text.size()));
}

/// The ordering as a sign, since a check macro cannot capture a strong_ordering compared against a literal zero.
int order_of(vdoc::file::blob_hash const& lhs, vdoc::file::blob_hash const& rhs)
{
    auto const order = lhs.compare_bytes(rhs);
    if (order < 0)
        return -1;
    if (order > 0)
        return 1;
    return 0;
}
} // namespace

TEST("vdoc::file - a blob hash is the content hash of the decoded bytes")
{
    auto const a = hash_of("wall panel");
    auto const b = hash_of("wall panel");
    auto const c = hash_of("wall panels");

    CHECK(a == b); // identical bytes collapse, which is what makes blobs shared
    CHECK(a != c);

    // The default is the all-zero hash no real blob has, so an unset part is recognizable.
    CHECK(vdoc::file::blob_hash() != a);
    CHECK(vdoc::file::blob_hash() == vdoc::file::blob_hash());

    // Empty input is a legal blob, and hashes to something rather than to zero.
    CHECK(hash_of("") != vdoc::file::blob_hash());
}

TEST("vdoc::file - a blob hash round-trips through its canonical 32 bytes")
{
    auto const original = hash_of("mesh/lod0");

    byte encoded[vdoc::file::blob_hash::byte_size] = {};
    original.to_bytes(encoded);
    CHECK(vdoc::file::blob_hash::from_bytes(encoded) == original);
}

TEST("vdoc::file - blob hashes order by their canonical bytes")
{
    // The byte order, not hash256's limb order: the two disagree, and only the byte order matches the hex digests.
    // A durable set keyed on this is what gives a publish a reproducible write order.
    auto hashes = cc::vector<vdoc::file::blob_hash>();
    for (auto const* text : {"a", "b", "c", "d", "e", "f", "g", "h"})
        hashes.push_back(hash_of(text));

    for (auto const& lhs : hashes)
    {
        CHECK(order_of(lhs, lhs) == 0);
        for (auto const& rhs : hashes)
        {
            if (lhs == rhs)
                continue;

            // Antisymmetric, and never equal for distinct content.
            CHECK(order_of(lhs, rhs) == -order_of(rhs, lhs));
            CHECK(order_of(lhs, rhs) != 0);
            CHECK(vdoc::file::blob_hash::by_bytes{}(lhs, rhs) == (order_of(lhs, rhs) < 0));
        }
    }

    // The comparison walks the canonical form, so it agrees with comparing those bytes directly.
    auto const& first = hashes[0];
    auto const& second = hashes[1];
    byte a[vdoc::file::blob_hash::byte_size] = {};
    byte b[vdoc::file::blob_hash::byte_size] = {};
    first.to_bytes(a);
    second.to_bytes(b);

    isize differing = 0;
    while (differing < vdoc::file::blob_hash::byte_size && a[differing] == b[differing])
        ++differing;
    REQUIRE(differing < vdoc::file::blob_hash::byte_size);
    CHECK((order_of(first, second) < 0) == (u8(a[differing]) < u8(b[differing])));
}

TEST("vdoc::file - an asset with no parts is legal")
{
    // Metadata but no bytes: an asset that names something the application knows how to make.
    auto const record = vdoc::file::asset_record{.asset_id = "meshes/wall-panel", .kind = "mesh"};

    CHECK(record.parts.empty());
    CHECK(record.is_resolvable); // nothing to resolve is not the same as failing to resolve
    CHECK(record.meta.is_null());
}

TEST("vdoc::file - a blob upload defaults to raw with data")
{
    auto const upload = vdoc::file::blob_upload{.hash = hash_of("payload"), .format = "png", .decoded_size = 7};

    CHECK(upload.encoding == "raw"); // the only encoding v1 writes
    CHECK(upload.has_data);          // "you already have this" is the opt-in, not the default
}
