#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <versioned-document/op.hh>
#include <versioned-document/value_builder.hh>

#include <algorithm>

using namespace cc::primitive_defines;

using vdoc::assignment;
using vdoc::component_type_id;
using vdoc::entity_id;
using vdoc::op;
using vdoc::op_decode_error;
using vdoc::op_id;
using vdoc::op_verification;
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

/// The encoded empty object, which is what an op with nothing to say for itself carries as metadata.
[[nodiscard]] value empty_metadata()
{
    return vdoc::value_builder::object().build();
}

/// Builds the two blobs an op is made of, from assignments given in any order.
struct op_parts
{
    cc::vector<byte> metadata_bytes;
    cc::vector<byte> assignment_bytes;
    cc::vector<value> owned_values;
};

[[nodiscard]] op_parts parts_of(cc::span<property_path const> paths, cc::span<i64 const> values)
{
    auto out = op_parts();

    auto const metadata = empty_metadata();
    out.metadata_bytes = cc::vector<byte>::create_copy_of(metadata.bytes());

    // the values must outlive the assignment views that point at them
    out.owned_values.reserve(values.size());
    for (isize i = 0; i < values.size(); ++i)
        out.owned_values.push_back(value::of_i64(values[i]));

    auto entries = cc::vector<assignment>();
    for (isize i = 0; i < paths.size(); ++i)
        entries.push_back(assignment{.path = paths[i], .value = out.owned_values[i]});

    // encode_assignments requires the canonical order, which is op_builder's job in the real flow
    std::sort(entries.begin(), entries.end(),
              [](assignment const& a, assignment const& b) { return a.path.compare_bytes(b.path) < 0; });

    out.assignment_bytes = vdoc::encode_assignments(entries);
    return out;
}

/// Decodes what parts_of built, stamping the id the content actually has.
[[nodiscard]] cc::result<op, op_decode_error> decode(cc::span<op_id const> parents, op_parts const& p)
{
    auto const id = vdoc::compute_op_id(parents, p.metadata_bytes, p.assignment_bytes);
    return vdoc::try_decode_op(id, parents, p.metadata_bytes, p.assignment_bytes);
}

[[nodiscard]] bool same_bytes(cc::span<byte const> a, cc::span<byte const> b)
{
    if (a.size() != b.size())
        return false;
    for (isize i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}

[[nodiscard]] op_id id_of_byte(u8 b)
{
    byte bytes[op_id::byte_size] = {};
    for (isize i = 0; i < op_id::byte_size; ++i)
        bytes[i] = byte(b);
    return op_id::from_bytes(bytes);
}
} // namespace

TEST("vdoc - identical content gives one op id, whatever order the caller supplied")
{
    cc::vector<property_path> const forward = {path_of("e1", "Transform", "position"),
                                               path_of("e1", "Transform", "rotation"), path_of("e2", "Mesh", "asset")};
    cc::vector<property_path> const backward = {forward[2], forward[1], forward[0]};

    cc::vector<i64> const values_forward = {10, 20, 30};
    cc::vector<i64> const values_backward = {30, 20, 10};

    auto const a = parts_of(forward, values_forward);
    auto const b = parts_of(backward, values_backward);

    // the same assignments, supplied in opposite orders, canonicalize to the same bytes
    CHECK(same_bytes(a.assignment_bytes, b.assignment_bytes));
    CHECK(vdoc::compute_op_id({}, a.metadata_bytes, a.assignment_bytes)
          == vdoc::compute_op_id({}, b.metadata_bytes, b.assignment_bytes));
}

TEST("vdoc - any single-byte change gives a different op id")
{
    cc::vector<property_path> const paths = {path_of("e1", "Transform", "position")};
    cc::vector<i64> const values = {10};
    auto const p = parts_of(paths, values);

    auto const base = vdoc::compute_op_id({}, p.metadata_bytes, p.assignment_bytes);

    for (isize i = 0; i < p.assignment_bytes.size(); ++i)
    {
        auto mutated = p.assignment_bytes;
        mutated[i] = byte(u8(mutated[i]) ^ 0xFF);
        CHECK(vdoc::compute_op_id({}, p.metadata_bytes, mutated) != base);
    }
}

TEST("vdoc - the parent list is part of the id, in its own order")
{
    cc::vector<property_path> const paths = {path_of("e1", "Transform", "position")};
    cc::vector<i64> const values = {10};
    auto const p = parts_of(paths, values);

    cc::vector<op_id> const none = {};
    cc::vector<op_id> const one = {id_of_byte(0x11)};
    cc::vector<op_id> const two = {id_of_byte(0x11), id_of_byte(0x22)};
    cc::vector<op_id> const swapped = {id_of_byte(0x22), id_of_byte(0x11)};

    auto const id_of = [&](cc::span<op_id const> parents)
    { return vdoc::compute_op_id(parents, p.metadata_bytes, p.assignment_bytes); };

    CHECK(id_of(none) != id_of(one));
    CHECK(id_of(one) != id_of(two));

    // parents are hashed verbatim, in the op's own order — canonicalizing them is the builder's job, not the hash's
    CHECK(id_of(two) != id_of(swapped));
}

TEST("vdoc - op_id orders by its canonical 32 bytes, not by hash256 limbs")
{
    // hash256's defaulted <=> orders four u64 limbs assembled little-endian, which is NOT byte order.
    // The parent sort feeds the hash preimage, so getting this wrong would put a different parent sequence into
    // every merge op's id.
    byte low[op_id::byte_size] = {};
    byte high[op_id::byte_size] = {};

    // differ only in the FIRST canonical byte, which is the least significant byte of limb l0
    low[0] = byte(0x01);
    high[0] = byte(0x02);

    auto const a = op_id::from_bytes(low);
    auto const b = op_id::from_bytes(high);

    CHECK(std::is_lt(a.compare_bytes(b)));
    CHECK(op_id::by_bytes{}(a, b));

    // and a difference in the LAST byte, the most significant of limb l3, orders the same way
    byte last_low[op_id::byte_size] = {};
    byte last_high[op_id::byte_size] = {};
    last_low[op_id::byte_size - 1] = byte(0x01);
    last_high[op_id::byte_size - 1] = byte(0x02);
    CHECK(std::is_lt(op_id::from_bytes(last_low).compare_bytes(op_id::from_bytes(last_high))));

    // a byte >= 0x80 must still sort ABOVE a small one, which a signed comparison would get backwards
    byte signed_trap[op_id::byte_size] = {};
    signed_trap[0] = byte(0x80);
    CHECK(std::is_lt(a.compare_bytes(op_id::from_bytes(signed_trap))));
}

TEST("vdoc - a decoded op round-trips its assignments")
{
    cc::vector<property_path> const paths
        = {path_of("e1", "Transform", "position"), path_of("e1", "Transform", "rotation")};
    cc::vector<i64> const values = {10, 20};
    auto const p = parts_of(paths, values);

    auto const decoded = decode({}, p);
    REQUIRE(decoded.has_value());

    auto const& o = decoded.value();
    CHECK(!o.is_skeleton());
    CHECK(o.metadata().kind() == vdoc::value_kind::object);

    auto const list = o.try_decode_assignments();
    REQUIRE(list.has_value());
    REQUIRE(list.value().size() == 2);

    CHECK(list.value()[0].path == paths[0]);
    CHECK(list.value()[0].value.as_i64() == 10);
    CHECK(list.value()[1].path == paths[1]);
    CHECK(list.value()[1].value.as_i64() == 20);

    // the cursor and the eager form agree, since one is what the other walks
    auto walked = 0;
    for (auto const a : o.assignments())
    {
        CHECK(a.path == list.value()[walked].path);
        ++walked;
    }
    CHECK(walked == 2);
}

TEST("vdoc - an op with no assignments is valid, and is not a skeleton")
{
    auto const p = parts_of({}, {});

    auto const decoded = decode({}, p);
    REQUIRE(decoded.has_value());

    // zero assignments is what a re-set of unchanged content produces; the content is empty, not gone
    CHECK(!decoded.value().is_skeleton());
    CHECK(decoded.value().assignments().at_end());
    CHECK(vdoc::verify_op(decoded.value()) == op_verification::verified);
}

TEST("vdoc - verification re-hashes the stored bytes and catches a mutated one")
{
    cc::vector<property_path> const paths = {path_of("e1", "Transform", "position")};
    cc::vector<i64> const values = {10};
    auto const p = parts_of(paths, values);

    auto const decoded = decode({}, p);
    REQUIRE(decoded.has_value());
    CHECK(vdoc::verify_op(decoded.value()) == op_verification::verified);

    // flip one stored byte behind the id's back: this is corruption, and it must read as one
    auto tampered = decoded.value();
    tampered.payload.value().assignment_bytes[7] = byte(u8(tampered.payload.value().assignment_bytes[7]) ^ 0x01);
    CHECK(vdoc::verify_op(tampered) == op_verification::mismatch);
}

TEST("vdoc - a skeleton op is unverifiable, never a mismatch")
{
    // This is the false-alarm case that matters most: pruning routinely produces skeletons, and reporting one as
    // tampering would train everyone to ignore the one alarm the whole scheme exists to raise.
    cc::vector<op_id> const parents = {id_of_byte(0x11)};

    auto const skeleton = vdoc::try_decode_skeleton_op(id_of_byte(0xAB), parents);
    REQUIRE(skeleton.has_value());

    CHECK(skeleton.value().is_skeleton());
    CHECK(vdoc::verify_op(skeleton.value()) == op_verification::unverifiable);
    CHECK(vdoc::verify_op(skeleton.value()) != op_verification::mismatch);

    // its place in the DAG survives, which is the entire reason the row is kept
    CHECK(skeleton.value().parents.size() == 1);
    CHECK(skeleton.value().parents[0] == parents[0]);

    // and it has no content to offer, without that reading as "empty content"
    CHECK(skeleton.value().assignments().at_end());
    CHECK(skeleton.value().metadata().is_null());
}

TEST("vdoc - a decoded op retains its input bytes exactly")
{
    // The structural form of "no load path ever re-serializes": what comes back is what went in, byte for byte.
    // Verification then re-hashes THESE bytes, so there is no encoder on the path for a future change to reach.
    cc::vector<property_path> const paths = {path_of("e1", "Transform", "position"), path_of("e2", "Mesh", "asset")};
    cc::vector<i64> const values = {10, 20};
    auto const p = parts_of(paths, values);

    auto const decoded = decode({}, p);
    REQUIRE(decoded.has_value());

    CHECK(same_bytes(decoded.value().payload.value().metadata_bytes, p.metadata_bytes));
    CHECK(same_bytes(decoded.value().payload.value().assignment_bytes, p.assignment_bytes));

    // and the id it verifies against is recomputed from exactly those retained bytes
    CHECK(vdoc::compute_op_id(decoded.value().parents, decoded.value().payload.value().metadata_bytes,
                              decoded.value().payload.value().assignment_bytes)
          == decoded.value().id);
}

TEST("vdoc - decoding rejects an id that does not match the bytes")
{
    cc::vector<property_path> const paths = {path_of("e1", "Transform", "position")};
    cc::vector<i64> const values = {10};
    auto const p = parts_of(paths, values);

    auto const decoded = vdoc::try_decode_op(id_of_byte(0x00), {}, p.metadata_bytes, p.assignment_bytes);
    REQUIRE(decoded.has_error());
    CHECK(decoded.error() == op_decode_error::hash_mismatch);
}

TEST("vdoc - decoding rejects unsorted and duplicated parents")
{
    cc::vector<property_path> const paths = {path_of("e1", "Transform", "position")};
    cc::vector<i64> const values = {10};
    auto const p = parts_of(paths, values);

    cc::vector<op_id> const unsorted = {id_of_byte(0x22), id_of_byte(0x11)};
    cc::vector<op_id> const duplicated = {id_of_byte(0x11), id_of_byte(0x11)};

    auto const a = vdoc::try_decode_op(vdoc::compute_op_id(unsorted, p.metadata_bytes, p.assignment_bytes), unsorted,
                                       p.metadata_bytes, p.assignment_bytes);
    REQUIRE(a.has_error());
    CHECK(a.error() == op_decode_error::unsorted_parents);

    auto const b = vdoc::try_decode_op(vdoc::compute_op_id(duplicated, p.metadata_bytes, p.assignment_bytes),
                                       duplicated, p.metadata_bytes, p.assignment_bytes);
    REQUIRE(b.has_error());
    CHECK(b.error() == op_decode_error::duplicate_parent);
}

TEST("vdoc - decoding rejects an unknown assignment encoding tag, naming it")
{
    cc::vector<property_path> const paths = {path_of("e1", "Transform", "position")};
    cc::vector<i64> const values = {10};
    auto const p = parts_of(paths, values);

    auto future = p.assignment_bytes;
    future[0] = byte(2); // a tag this build predates

    auto const decoded
        = vdoc::try_decode_op(vdoc::compute_op_id({}, p.metadata_bytes, future), {}, p.metadata_bytes, future);
    REQUIRE(decoded.has_error());

    // not a corruption report: the bytes may be perfectly good, this build just cannot read them
    CHECK(decoded.error() == op_decode_error::unknown_assignment_encoding);
    CHECK(decoded.error() != op_decode_error::hash_mismatch);
}

TEST("vdoc - decoding rejects a truncated assignment blob")
{
    cc::vector<property_path> const paths = {path_of("e1", "Transform", "position")};
    cc::vector<i64> const values = {10};
    auto const p = parts_of(paths, values);

    for (isize cut = 1; cut < p.assignment_bytes.size(); ++cut)
    {
        auto truncated = cc::vector<byte>();
        for (isize i = 0; i < cut; ++i)
            truncated.push_back(p.assignment_bytes[i]);

        auto const decoded
            = vdoc::try_decode_op(vdoc::compute_op_id({}, p.metadata_bytes, truncated), {}, p.metadata_bytes, truncated);
        CHECK(decoded.has_error());
    }
}

TEST("vdoc - decoding rejects non-canonical metadata")
{
    cc::vector<property_path> const paths = {path_of("e1", "Transform", "position")};
    cc::vector<i64> const values = {10};
    auto const p = parts_of(paths, values);

    cc::vector<byte> const not_a_value = {byte(0xEE)};

    auto const decoded = vdoc::try_decode_op(vdoc::compute_op_id({}, not_a_value, p.assignment_bytes), {}, not_a_value,
                                             p.assignment_bytes);
    REQUIRE(decoded.has_error());
    CHECK(decoded.error() == op_decode_error::invalid_value);
}
