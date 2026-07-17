#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/container/vector.hh>
#include <clean-core/thread/impl/chase_lev_deque.hh>
#include <nexus/test.hh>

#if CC_HAS_THREADS
#include <thread>
#endif

// White-box tests for the Chase-Lev deque behind async_thread_pool's workers.
//
// The single-threaded tests pin the shape (LIFO owner end, FIFO steal end, growth, the empty/abort split). The
// concurrent one pins the only two properties that actually matter for the pool's correctness: every pushed
// value comes out EXACTLY ONCE -- never lost, never duplicated. Since the deque will hold strong refcounts,
// losing a value leaks a whole graph and duplicating one double-frees it; "it didn't crash" would catch neither.
//
// These carry more weight than usual: there is no TSan preset (all three sanitizer presets are address+undefined),
// so nothing else here checks the hand-rolled memory orderings.

using cc::impl::chase_lev_deque;
using cc::impl::steal_result;

namespace
{
using deque = chase_lev_deque<int*>;

// Stable distinct addresses to push: the deque stores raw pointers, and the tests only ever compare identity.
int* tag(int i)
{
    static int slots[1 << 14];
    CC_ASSERT(i >= 0 && i < int(sizeof(slots) / sizeof(slots[0])), "tag index out of range");
    return slots + i;
}

int tag_index(int* p)
{
    return int(p - tag(0));
}
} // namespace

TEST("chase_lev_deque - the owner end is LIFO")
{
    deque d(16);
    for (int i = 0; i < 8; ++i)
        d.push(tag(i));

    for (int i = 7; i >= 0; --i) // newest first
    {
        int* out = nullptr;
        REQUIRE(d.try_take(out));
        CHECK(tag_index(out) == i);
    }

    int* out = nullptr;
    CHECK(!d.try_take(out));
}

TEST("chase_lev_deque - the steal end is FIFO")
{
    deque d(16);
    for (int i = 0; i < 8; ++i)
        d.push(tag(i));

    for (int i = 0; i < 8; ++i) // oldest first: the coldest entry migrates
    {
        int* out = nullptr;
        REQUIRE(d.try_steal(out) == steal_result::success);
        CHECK(tag_index(out) == i);
    }
}

TEST("chase_lev_deque - an empty deque reports empty, not abort")
{
    // The pool branches on this: `empty` means skip this victim, `abort` means retry. Collapsing them makes it
    // either spin on a contended victim or stop looking while work exists.
    deque d(16);

    int* out = nullptr;
    CHECK(!d.try_take(out));
    CHECK(d.try_steal(out) == steal_result::empty);

    d.push(tag(1));
    REQUIRE(d.try_take(out)); // drained again by the owner
    CHECK(d.try_steal(out) == steal_result::empty);
}

TEST("chase_lev_deque - a single element goes to exactly one of take and steal")
{
    deque d(16);

    d.push(tag(3));
    int* a = nullptr;
    REQUIRE(d.try_take(a)); // owner wins uncontended
    CHECK(tag_index(a) == 3);
    int* b = nullptr;
    CHECK(d.try_steal(b) == steal_result::empty);

    d.push(tag(4));
    REQUIRE(d.try_steal(b) == steal_result::success); // thief wins uncontended
    CHECK(tag_index(b) == 4);
    CHECK(!d.try_take(a));
}

TEST("chase_lev_deque - growth preserves every element and its order")
{
    deque d(2); // tiny, so this crosses several doublings
    CHECK(d.capacity() == 2);

    constexpr int n = 1000;
    for (int i = 0; i < n; ++i)
        d.push(tag(i));
    CHECK(d.capacity() >= n);
    CHECK(d.size_estimate() == n);

    for (int i = n - 1; i >= 0; --i) // still LIFO across the grows
    {
        int* out = nullptr;
        REQUIRE(d.try_take(out));
        CHECK(tag_index(out) == i);
    }
}

TEST("chase_lev_deque - growth preserves order seen from the steal end")
{
    deque d(2);
    constexpr int n = 500;
    for (int i = 0; i < n; ++i)
        d.push(tag(i));

    for (int i = 0; i < n; ++i)
    {
        int* out = nullptr;
        REQUIRE(d.try_steal(out) == steal_result::success);
        CHECK(tag_index(out) == i);
    }
}

TEST("chase_lev_deque - indices keep working across many push/take cycles")
{
    // The indices only ever increase and are masked into the ring, so a long run of interleaved push/take must
    // keep wrapping correctly rather than drifting.
    deque d(4);
    int expected = 0;
    for (int round = 0; round < 500; ++round)
    {
        d.push(tag(round % 64));
        d.push(tag((round + 1) % 64));

        int* out = nullptr;
        REQUIRE(d.try_take(out));
        CHECK(tag_index(out) == (round + 1) % 64);
        REQUIRE(d.try_take(out));
        CHECK(tag_index(out) == round % 64);
        expected = 0;
    }
    (void)expected;
    CHECK(d.size_estimate() == 0);
}

TEST("chase_lev_deque - a fresh deque is empty")
{
    deque d(8);
    CHECK(d.size_estimate() == 0);
    int* out = nullptr;
    CHECK(!d.try_take(out));
    CHECK(d.try_steal(out) == steal_result::empty);
}

#if CC_HAS_THREADS

TEST("chase_lev_deque - concurrent owner and thieves claim every value exactly once")
{
    // THE test for this type. One owner pushing and taking across several growths while K thieves steal;
    // afterwards every value must have been claimed exactly once. Loss and duplication are the only two failure
    // modes that matter -- in the pool they are a leaked graph and a double-free respectively -- and a crash-only
    // check would notice neither.
    constexpr int n = 1 << 13; // enough pushes to force several doublings out of the initial 8
    constexpr int thieves = 3;

    deque d(8);
    cc::atomic<int> claims[n];
    for (auto& c : claims)
        c.store(0, cc::memory_order_relaxed);

    cc::atomic<bool> done{false};
    cc::atomic<int> claimed_total{0};

    auto const claim = [&](int* p)
    {
        claims[tag_index(p)].fetch_add(1, cc::memory_order_relaxed);
        claimed_total.fetch_add(1, cc::memory_order_relaxed);
    };

    cc::vector<std::thread> ts;
    ts.reserve(thieves);
    for (int i = 0; i < thieves; ++i)
        ts.push_back(std::thread(
            [&]
            {
                while (!done.load(cc::memory_order_acquire))
                {
                    int* out = nullptr;
                    if (d.try_steal(out) == steal_result::success)
                        claim(out);
                }
                // drain whatever is left after the owner stopped, so the exactly-once total can be checked
                for (;;)
                {
                    int* out = nullptr;
                    auto const r = d.try_steal(out);
                    if (r == steal_result::success)
                        claim(out);
                    else if (r == steal_result::empty)
                        break; // `abort` means someone else raced us and there may still be work: retry
                }
            }));

    // the owner: push everything, taking some back interleaved so both ends are live at once
    for (int i = 0; i < n; ++i)
    {
        d.push(tag(i));
        if ((i & 3) == 0)
        {
            int* out = nullptr;
            if (d.try_take(out))
                claim(out);
        }
    }
    for (;;) // owner drains its end too
    {
        int* out = nullptr;
        if (!d.try_take(out))
            break;
        claim(out);
    }

    done.store(true, cc::memory_order_release);
    for (auto& t : ts)
        t.join();

    // the deque must be empty, and every value claimed exactly once
    CHECK(d.size_estimate() == 0);
    CHECK(claimed_total.load() == n);

    int lost = 0;
    int duplicated = 0;
    for (auto& c : claims)
    {
        int const v = c.load(cc::memory_order_relaxed);
        if (v == 0)
            ++lost;
        else if (v > 1)
            ++duplicated;
    }
    CHECK(lost == 0);
    CHECK(duplicated == 0);
}

TEST("chase_lev_deque - thieves racing for a single element never double-claim it")
{
    // Narrows the above onto the one case the Dekker in try_take/try_steal exists for: the deque holds exactly
    // one entry and everybody wants it. Repeated so the interleaving actually gets hit.
    constexpr int rounds = 2000;
    constexpr int thieves = 3;

    deque d(8);
    cc::atomic<int> claimed{0};
    cc::atomic<bool> stop{false};
    cc::atomic<int> round_gate{0};

    cc::vector<std::thread> ts;
    ts.reserve(thieves);
    for (int i = 0; i < thieves; ++i)
        ts.push_back(std::thread(
            [&]
            {
                while (!stop.load(cc::memory_order_acquire))
                {
                    int* out = nullptr;
                    if (d.try_steal(out) == steal_result::success)
                        claimed.fetch_add(1, cc::memory_order_relaxed);
                }
            }));

    int owner_took = 0;
    for (int r = 0; r < rounds; ++r)
    {
        d.push(tag(r % 64));
        round_gate.fetch_add(1, cc::memory_order_relaxed); // keep the loop from being optimized into nothing

        int* out = nullptr;
        if (d.try_take(out))
            ++owner_took;
    }

    stop.store(true, cc::memory_order_release);
    for (auto& t : ts)
        t.join();

    // whatever the owner did not get, a thief did -- and nobody got the same push twice
    for (;;)
    {
        int* out = nullptr;
        if (!d.try_take(out))
            break;
        ++owner_took;
    }
    CHECK(owner_took + claimed.load() == rounds);
}

#endif // CC_HAS_THREADS
