#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/interned_string.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

#include <compare> // std::is_lt and friends, for asserting about a strong_ordering

#if CC_HAS_THREADS
#include <clean-core/thread/atomic.hh>

#include <thread>
#endif

using namespace cc::primitive_defines;

namespace
{
// Enough distinct strings that the shards all see traffic and the arena spills past one block.
cc::vector<cc::string> many_strings(isize count)
{
    cc::vector<cc::string> out;
    out.reserve(count);
    for (isize i = 0; i < count; ++i)
        out.push_back(cc::format("entity/{}/component", i));
    return out;
}
} // namespace

TEST("interned_string - the same bytes intern to the same handle")
{
    auto table = cc::string_interner();

    auto const a = table.intern("transform");
    auto const b = table.intern("transform");
    CHECK(a == b);
    CHECK(hash(a) == hash(b));
    CHECK(table.size() == 1);

    // a copy of the bytes, so this cannot be passing by pointer identity of the input
    auto const owned = cc::string::create_copy_of("transform");
    CHECK(table.intern(owned) == a);
    CHECK(table.size() == 1);
}

TEST("interned_string - distinct bytes never share a handle")
{
    auto table = cc::string_interner();

    auto const strings = many_strings(2000);
    cc::vector<cc::interned_string> handles;
    for (auto const& s : strings)
        handles.push_back(table.intern(s));

    CHECK(table.size() == 2000);

    // every handle is distinct, and every one still views the bytes it was made from
    auto seen = cc::map<cc::interned_string, isize>();
    for (isize i = 0; i < handles.size(); ++i)
    {
        CHECK(!seen.contains(handles[i]));
        seen[handles[i]] = i;
        CHECK(handles[i].as_string_view() == strings[i]);
    }
}

TEST("interned_string - the empty string is the default handle, in every table")
{
    auto table_a = cc::string_interner();
    auto table_b = cc::string_interner();

    auto const empty = cc::interned_string();
    CHECK(empty.empty());
    CHECK(empty.size() == 0);
    CHECK(empty.as_string_view() == "");
    CHECK(hash(empty) == hash(cc::string_view()));

    CHECK(table_a.intern("") == empty);
    CHECK(table_b.intern("") == empty);
    CHECK(cc::intern("") == empty);

    // and it costs no entry
    CHECK(table_a.size() == 0);
}

TEST("interned_string - hashes like the string it views")
{
    auto table = cc::string_interner();
    auto const s = cc::string_view("$schema_version");
    CHECK(hash(table.intern(s)) == hash(s));
}

TEST("interned_string - ordering follows the bytes, not the intern order")
{
    auto const strings = many_strings(500);

    // Two tables fed the same strings in opposite orders.
    // If ordering leaked the intern order or the entry address, the two sorted sequences below would differ —
    // and a document's sorted arrays would stop being reproducible from one run to the next.
    auto forward = cc::string_interner();
    auto backward = cc::string_interner();

    cc::vector<cc::interned_string> from_forward;
    cc::vector<cc::interned_string> from_backward;
    for (isize i = 0; i < strings.size(); ++i)
    {
        from_forward.push_back(forward.intern(strings[i]));
        from_backward.push_back(backward.intern(strings[strings.size() - 1 - i]));
    }

    cc::sort(from_forward, cc::interned_string::by_bytes{});
    cc::sort(from_backward, cc::interned_string::by_bytes{});

    for (isize i = 0; i < from_forward.size(); ++i)
        CHECK(from_forward[i].as_string_view() == from_backward[i].as_string_view());

    // and the order really is the byte order
    for (isize i = 1; i < from_forward.size(); ++i)
        CHECK(from_forward[i - 1].as_string_view() < from_forward[i].as_string_view());
}

TEST("interned_string - compare_bytes agrees with the viewed bytes")
{
    auto table = cc::string_interner();

    auto const a = table.intern("alpha");
    auto const b = table.intern("beta");
    // std::is_lt and friends rather than `< 0`: a strong_ordering compares only against a literal zero, which
    // does not survive CHECK's expression decomposition.
    CHECK(std::is_lt(a.compare_bytes(b)));
    CHECK(std::is_gt(b.compare_bytes(a)));
    CHECK(std::is_eq(a.compare_bytes(table.intern("alpha"))));
    CHECK(a != b);

    CHECK(cc::interned_string::by_bytes{}(a, b));
    CHECK(!cc::interned_string::by_bytes{}(b, a));
    CHECK(!cc::interned_string::by_bytes{}(a, a));

    // the empty string sorts first, being a prefix of everything
    CHECK(std::is_lt(cc::interned_string().compare_bytes(a)));
}

TEST("interned_string - compare_bytes orders by unsigned byte value")
{
    auto table = cc::string_interner();

    // 0xC3 0xA4 is 'a-umlaut' in UTF-8. Comparing char directly is SIGNED on our platforms, which would make this
    // byte negative and sort the whole string before every ASCII one — an order no other implementation shares,
    // and one that would reach a file the moment a sorted structure is written out.
    auto const ascii = table.intern("z");
    auto const high = table.intern("\xC3\xA4");

    CHECK(std::is_lt(ascii.compare_bytes(high)));
    CHECK(std::is_gt(high.compare_bytes(ascii)));
    CHECK(cc::interned_string::by_bytes{}(ascii, high));

    // and the whole family agrees, since compare_bytes is a thin name over string_view::compare
    CHECK(ascii.as_string_view() < high.as_string_view());
    CHECK(cc::string_view("z").compare(cc::string_view("\xC3\xA4")) < 0);
}

TEST("interned_string - compare_identity is a total order, and only that")
{
    auto table = cc::string_interner();

    auto const a = table.intern("alpha");
    auto const b = table.intern("beta");

    // The one thing it promises: a strict total order over whatever handles exist in this process.
    // Which way round a and b fall is deliberately not asserted — it is a different answer every run.
    CHECK(std::is_lt(a.compare_identity(b)) != std::is_lt(b.compare_identity(a)));
    CHECK(std::is_eq(a.compare_identity(a)));
    CHECK(std::is_eq(a.compare_identity(table.intern("alpha"))));

    CHECK(cc::interned_string::by_identity{}(a, b) != cc::interned_string::by_identity{}(b, a));
    CHECK(!cc::interned_string::by_identity{}(a, a));
}

TEST("interned_string - tables are independent of one another")
{
    auto table_a = cc::string_interner();
    auto table_b = cc::string_interner();

    CHECK(table_a.intern("mesh") != table_b.intern("mesh"));
    CHECK(table_a.intern("mesh").as_string_view() == table_b.intern("mesh").as_string_view());

    // and neither is the process-wide one
    CHECK(cc::intern("mesh") != table_a.intern("mesh"));
    CHECK(cc::intern("mesh") == cc::intern("mesh"));
}

#if CC_HAS_THREADS
TEST("interned_string - concurrent interning yields one entry per distinct string")
{
    auto table = cc::string_interner();

    auto const strings = many_strings(1000);
    constexpr isize thread_count = 8;

    // Every thread interns the whole set, so each string is raced for by all of them.
    cc::vector<cc::vector<cc::interned_string>> results;
    results.resize_to_defaulted(thread_count);

    cc::atomic<bool> go = {false};
    cc::vector<std::thread> threads;
    for (isize t = 0; t < thread_count; ++t)
        threads.emplace_back(
            [&, t]
            {
                while (!go.load(cc::memory_order_acquire))
                {
                }
                for (auto const& s : strings)
                    results[t].push_back(table.intern(s));
            });

    go.store(true, cc::memory_order_release);
    for (auto& t : threads)
        t.join();

    CHECK(table.size() == strings.size());

    // whoever won each race, all eight threads came away with the same handle
    for (isize i = 0; i < strings.size(); ++i)
        for (isize t = 1; t < thread_count; ++t)
            CHECK(results[t][i] == results[0][i]);
}
#endif
