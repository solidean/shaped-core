#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <nexus/fuzz/fuzz.hh>
#include <nexus/test.hh>
#include <versioned-document/value.hh>
#include <versioned-document/value_builder.hh>
#include <versioned-document/value_debug.hh>

using namespace cc::primitive_defines;

using vdoc::value;
using vdoc::value_builder;
using vdoc::value_kind;
using vdoc::value_view;

namespace
{
// Mutated buffers must not grow without bound: concatenating and wrapping are exponential over a long run,
// and a fuzz test that quietly turns into a memory benchmark stops finding anything.
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

void append_u32(buffer& b, u32 v)
{
    for (isize i = 0; i < 4; ++i)
        b.push_back(byte((v >> (i * 8)) & 0xFF));
}

/// Touches every accessor the decoder just vouched for.
/// If validation and the accessors ever disagree about where an entry ends, this is where it shows up as a read out of bounds.
void walk(value_view v)
{
    auto const k = v.kind();
    if (k != value_kind::array && k != value_kind::object)
        return;

    for (isize i = 0; i < v.size(); ++i)
    {
        if (k == value_kind::object)
        {
            auto const key = v.key_at(i);
            CHECK(v.try_find(key).has_value());
        }

        walk(v.element_at(i));
    }
}
} // namespace

TEST("vdoc - value decoder fuzz")
{
    auto test = nx::fuzz::test::create();

    // seeds: encodings this library actually produces, so a mutation starts from something plausible
    test->add_value("null", buffer_of(value::of_null().bytes()));
    test->add_value("bool", buffer_of(value::of(true).bytes()));
    test->add_value("int", buffer_of(value::of(-1234).bytes()));
    test->add_value("number", buffer_of(value::of(1.5).bytes()));
    test->add_value("string", buffer_of(value::of("wall").bytes()));
    test->add_value("array", buffer_of(value_builder::array().push(1).push("x").build().bytes()));
    test->add_value("object", buffer_of(value_builder::object().set("a", 1).set("b", value::of_null()).build().bytes()));

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

    test->add_op("wrap in an array header",
                 [](buffer const& inner)
                 {
                     if (inner.size() + 9 > max_buffer_size)
                         return buffer::create_copy_of(as_span(inner));

                     auto out = buffer();
                     out.push_back(byte(u8(value_kind::array)));
                     append_u32(out, u32(4 + inner.size()));
                     append_u32(out, 1);
                     for (isize i = 0; i < inner.size(); ++i)
                         out.push_back(inner[i]);

                     return out;
                 });

    // The one thing that must hold for EVERY byte sequence, however malformed:
    // try_decode returns instead of crashing, and whatever it accepts is fully usable.
    test->add_invariant("decoding is total",
                        [](buffer const& b)
                        {
                            auto const decoded = vdoc::try_decode(as_span(b));
                            if (decoded.has_error())
                                return;

                            auto const v = decoded.value();

                            // an accepted value covers its whole input, or "no trailing bytes" is not being enforced
                            CHECK(v.bytes().size() == b.size());

                            // its own canonical bytes decode back to it, which is the property everything durable rests on
                            auto const again = vdoc::try_decode(v.bytes());
                            REQUIRE(again.has_value());
                            CHECK(again.value() == v);
                            CHECK(hash(again.value()) == hash(v));

                            CHECK(vdoc::encoded_size(v.bytes()) == v.bytes().size());
                            CHECK(vdoc::skip_value(v.bytes()).size() == 0);

                            walk(v);

                            // the dumper runs on anything the decoder accepted, since it is what a failure prints
                            CHECK(vdoc::to_debug_string(v).size() > 0);
                        });

    SECTION("fuzz")
    {
        CHECK(test->execute_fuzz_test());
    }
}
