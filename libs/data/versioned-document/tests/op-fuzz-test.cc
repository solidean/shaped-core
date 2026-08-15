#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <nexus/fuzz/fuzz.hh>
#include <nexus/test.hh>
#include <versioned-document/op.hh>
#include <versioned-document/value_builder.hh>

using namespace cc::primitive_defines;

using vdoc::assignment;
using vdoc::component_type_id;
using vdoc::entity_id;
using vdoc::op_verification;
using vdoc::property_id;
using vdoc::property_path;
using vdoc::value;

namespace
{
// Same reasoning as the value fuzzer: concatenating and wrapping are exponential over a long run, and a fuzz test
// that quietly turns into a memory benchmark stops finding anything.
constexpr isize max_buffer_size = 4096;

using buffer = cc::vector<byte>;

[[nodiscard]] cc::span<byte const> as_span(buffer const& b)
{
    return cc::span<byte const>(b.data(), b.size());
}

[[nodiscard]] buffer buffer_of(cc::span<byte const> bytes)
{
    return buffer::create_copy_of(bytes);
}

[[nodiscard]] property_path path_of(cc::string_view e, cc::string_view c, cc::string_view p)
{
    return property_path{.entity = entity_id::of(e),
                         .component = component_type_id::of(c),
                         .property = property_id::of(p)};
}

/// An assignment blob this library actually produces, for the fuzzer to start mutating from.
[[nodiscard]] buffer seed_assignments(isize count)
{
    auto values = cc::vector<value>();
    for (isize i = 0; i < count; ++i)
        values.push_back(value::of_i64(i));

    auto entries = cc::vector<assignment>();
    for (isize i = 0; i < count; ++i)
    {
        auto name = cc::string("p");
        name += cc::to_string(i);
        entries.push_back(assignment{.path = path_of("e", "C", name), .value = values[i]});
    }

    return vdoc::encode_assignments(entries);
}

/// The metadata every fuzz case reuses: a well-formed empty object, so a failure is about the assignment blob.
[[nodiscard]] buffer valid_metadata()
{
    return buffer_of(vdoc::value_builder::object().build().bytes());
}
} // namespace

TEST("vdoc - op decoder fuzz")
{
    auto test = nx::fuzz::test::create();

    test->add_value("empty", buffer());
    test->add_value("no assignments", seed_assignments(0));
    test->add_value("one assignment", seed_assignments(1));
    test->add_value("several assignments", seed_assignments(5));

    test->add_op("flip a byte",
                 [](cc::random& rng, buffer& b)
                 {
                     if (b.empty())
                         return;

                     auto const i = rng.uniform(isize(0), b.size() - 1);
                     b[i] = byte(u8(b[i]) ^ u8(1 << rng.uniform(0, 7)));
                 });

    test->add_op("set a byte",
                 [](cc::random& rng, buffer& b)
                 {
                     if (b.empty())
                         return;

                     b[rng.uniform(isize(0), b.size() - 1)] = byte(u8(rng.uniform(0, 255)));
                 });

    test->add_op("truncate",
                 [](cc::random& rng, buffer& b)
                 {
                     auto const drop = rng.uniform(isize(1), b.size());
                     for (isize i = 0; i < drop && !b.empty(); ++i)
                         b.remove_back();
                 })
        ->when([](buffer const& b) { return !b.empty(); });

    test->add_op("append a byte", [](cc::random& rng, buffer& b) { b.push_back(byte(u8(rng.uniform(0, 255)))); })
        ->when([](buffer const& b) { return b.size() < max_buffer_size; });

    test->add_op("concatenate",
                 [](buffer const& a, buffer const& b)
                 {
                     if (a.size() + b.size() > max_buffer_size)
                         return buffer::create_copy_of(as_span(a));

                     auto out = buffer::create_copy_of(as_span(a));
                     for (isize i = 0; i < b.size(); ++i)
                         out.push_back(b[i]);

                     return out;
                 });

    // The one thing that must hold for EVERY byte sequence, however malformed: try_decode_op returns instead of
    // crashing, hanging or reading out of bounds — and never hands back an op that fails its own verification.
    test->add_invariant("decoding is total, and never yields an unverifiable op",
                        [](buffer const& b)
                        {
                            auto const metadata = valid_metadata();
                            auto const id = vdoc::compute_op_id({}, as_span(metadata), as_span(b));

                            auto const decoded = vdoc::try_decode_op(id, {}, as_span(metadata), as_span(b));
                            if (decoded.has_error())
                                return;

                            auto const& o = decoded.value();

                            // whatever came back must pass the check the loader just performed
                            CHECK(vdoc::verify_op(o) == op_verification::verified);

                            // an accepted op retained its input exactly — nothing on this path re-serialized anything
                            REQUIRE(!o.is_skeleton());
                            CHECK(o.payload.value().assignment_bytes.size() == b.size());

                            // every accessor the decoder just vouched for, so a disagreement about where an entry
                            // ends shows up here as a read out of bounds rather than silently
                            auto walked = isize(0);
                            for (auto const a : o.assignments())
                            {
                                CHECK(a.path.entity.size() >= 0);
                                CHECK(a.value.bytes().size() > 0);
                                ++walked;
                            }

                            auto const eager = o.try_decode_assignments();
                            REQUIRE(eager.has_value());
                            CHECK(eager.value().size() == walked);

                            // and the canonical order really is enforced, so no two entries share a path.
                            // std::is_lt rather than `< 0`: a strong_ordering only compares against a literal zero,
                            // which does not survive CHECK's expression decomposition.
                            for (isize i = 1; i < eager.value().size(); ++i)
                                CHECK(std::is_lt(eager.value()[i - 1].path.compare_bytes(eager.value()[i].path)));
                        });

    SECTION("fuzz")
    {
        CHECK(test->execute_fuzz_test());
    }
}
