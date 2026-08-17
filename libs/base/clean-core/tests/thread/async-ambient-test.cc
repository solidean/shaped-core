#include <clean-core/container/vector.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_ambient.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// The ambient context: the chain on its own, then which context a node's frame actually runs under.
// The cross-thread half lives in async-pool-test.cc, which is where the threads are.
//
// The rule these pin down, and it is drive-site rather than creation-site:
//
//   A subtree driven from one scheduler work item is billed entirely to that item's context.
//   A node inline-driven inside another node's poll is billed to that node's context, transitively.
//   A node's token is written once, by a thread executing or creating it — never by one merely waking it.

namespace
{
// Two independent "consumers", to pin that the chain composes and that tags do not collide.
CC_ASYNC_AMBIENT_TAG(tag_a)
CC_ASYNC_AMBIENT_TAG(tag_b)

int value_a = 1;
int value_b = 2;

/// What a frame sees when it asks for tag_a's context, as an i64 so it can be an async value.
/// 0 means "no context installed".
[[nodiscard]] i64 observed()
{
    auto* const v = cc::async_ambient_lookup(tag_a());
    return v == nullptr ? 0 : *static_cast<int*>(v);
}

/// Neither of this file's scopes is installed.
///
/// Deliberately per-tag rather than `async_current_ambient() == nullptr`: the chain is SHARED across consumers.
/// nexus has a link of its own in it, which is how a CHECK finds the test it belongs to.
/// So a bare head is not the question; whether our tags are present is.
bool no_scope_of_ours()
{
    return cc::async_ambient_lookup(tag_a()) == nullptr && cc::async_ambient_lookup(tag_b()) == nullptr;
}
} // namespace

TEST("async-ambient - nothing installed by default")
{
    CHECK(no_scope_of_ours());
}

TEST("async-ambient - a scope installs and pops")
{
    {
        cc::async_ambient_scope const s(tag_a(), &value_a);
        CHECK(cc::async_current_ambient() != nullptr);
        CHECK(cc::async_ambient_lookup(tag_a()) == &value_a);
        CHECK(cc::async_ambient_lookup(tag_b()) == nullptr); // a miss walks the whole chain and reports absence
    }
    CHECK(no_scope_of_ours());
    CHECK(cc::async_ambient_lookup(tag_a()) == nullptr);
}

TEST("async-ambient - two consumers compose in one chain")
{
    cc::async_ambient_scope const outer(tag_a(), &value_a);
    cc::async_ambient_scope const inner(tag_b(), &value_b);

    // The head is inner's, and a lookup for the outer consumer still finds it — that is the whole point of the chain.
    CHECK(cc::async_current_ambient() == inner.link());
    CHECK(cc::async_ambient_lookup(tag_a()) == &value_a);
    CHECK(cc::async_ambient_lookup(tag_b()) == &value_b);
}

TEST("async-ambient - the innermost scope for a tag wins")
{
    int shadowing = 42;

    cc::async_ambient_scope const outer(tag_a(), &value_a);
    CHECK(cc::async_ambient_lookup(tag_a()) == &value_a);
    {
        cc::async_ambient_scope const inner(tag_a(), &shadowing);
        CHECK(cc::async_ambient_lookup(tag_a()) == &shadowing);
    }
    CHECK(cc::async_ambient_lookup(tag_a()) == &value_a); // popping the inner one restores the outer
}

TEST("async-ambient - tags from separate declarations are distinct addresses")
{
    // The ICF hazard CC_ASYNC_AMBIENT_TAG exists to dodge: two identical zero-valued constants can be folded to one
    // address by MSVC /OPT:ICF or the gold/lld ICF passes, which would silently alias two consumers' contexts.
    CHECK(tag_a() != tag_b());
    CHECK(tag_a() == tag_a()); // and a tag is stable across calls
}

TEST("async-ambient - a lookup can be given an explicit head")
{
    void* captured = nullptr;
    {
        cc::async_ambient_scope const s(tag_a(), &value_a);
        captured = s.link();
        cc::impl::async_ambient_retain(captured); // stand in for a node carrying the context past the scope
    }

    // The scope is popped, but the chain the captured head names is intact.
    CHECK(no_scope_of_ours());
    CHECK(cc::async_ambient_lookup(tag_a()) == nullptr);
    CHECK(cc::async_ambient_lookup_in(captured, tag_a()) == &value_a);

    cc::impl::async_ambient_release(captured);
}

TEST("async-ambient - a held link outlives its scope, and the parent chain with it")
{
    void* held = nullptr;
    {
        cc::async_ambient_scope const outer(tag_a(), &value_a);
        {
            cc::async_ambient_scope const inner(tag_b(), &value_b);
            held = inner.link();
            cc::impl::async_ambient_retain(held);
        }
        // inner popped, but our reference keeps it alive
        CHECK(cc::async_ambient_lookup_in(held, tag_b()) == &value_b);
    }

    // outer popped too.
    // The held link holds its parent STRONGLY, so the outer link is still reachable through it.
    // This is what makes it safe for a node to outlive the scope that named it.
    CHECK(cc::async_ambient_lookup_in(held, tag_b()) == &value_b);
    CHECK(cc::async_ambient_lookup_in(held, tag_a()) == &value_a);

    cc::impl::async_ambient_release(held); // frees inner, then outer, in one iterative walk
}

TEST("async-ambient - a scope pops cleanly with work still outstanding")
{
    // cc deliberately does NOT assert here: prewarming — starting work under a scope that ends before the work does
    // — is a legitimate pattern, and the refcount is what makes it safe.
    // A consumer wanting the stricter rule reads outstanding() and reports it in its own terms.
    void* held = nullptr;
    {
        cc::async_ambient_scope const s(tag_a(), &value_a);
        held = s.link();
        cc::impl::async_ambient_retain(held); // stands in for a node scheduled under the scope and never awaited
        CHECK(s.outstanding() == 1);
    }

    CHECK(no_scope_of_ours());
    CHECK(cc::async_ambient_lookup_in(held, tag_a()) == &value_a); // still readable by whoever carries it
    cc::impl::async_ambient_release(held);
}

TEST("async-ambient - outstanding() counts holders beyond the scope")
{
    cc::async_ambient_scope const s(tag_a(), &value_a);
    CHECK(s.outstanding() == 0); // a fresh scope: only the scope itself holds the link

    auto* const head = s.link();
    cc::impl::async_ambient_retain(head);
    CHECK(s.outstanding() == 1);
    cc::impl::async_ambient_retain(head);
    CHECK(s.outstanding() == 2);

    cc::impl::async_ambient_release(head);
    cc::impl::async_ambient_release(head);
    CHECK(s.outstanding() == 0); // back to clean — what a leak-checking consumer wants to see at scope exit
}

TEST("async-ambient - store adjusts counts and is idempotent in value")
{
    cc::async_ambient_scope const s(tag_a(), &value_a);
    auto* const head = s.link();

    void* slot = nullptr;
    cc::impl::async_ambient_store(slot, head);
    CHECK(slot == head);
    CHECK(s.outstanding() == 1);

    // The repeat writers a node sees: same value, so no count churn.
    cc::impl::async_ambient_store(slot, head);
    cc::impl::async_ambient_store(slot, head);
    CHECK(s.outstanding() == 1);

    cc::impl::async_ambient_store(slot, nullptr);
    CHECK(slot == nullptr);
    CHECK(s.outstanding() == 0);
}

TEST("async-ambient - a deep chain frees iteratively")
{
    // Deep enough that a recursive free would be a real stack risk, and cheap to build.
    constexpr int depth = 4096;

    void* head = nullptr;
    {
        cc::vector<cc::unique_ptr<cc::async_ambient_scope>> scopes;
        scopes.reserve(depth);
        for (int i = 0; i < depth; ++i)
            scopes.push_back(cc::make_unique<cc::async_ambient_scope>(tag_a(), &value_a));

        head = scopes.back()->link();
        cc::impl::async_ambient_retain(head);

        for (int i = depth - 1; i >= 0; --i) // LIFO, as the scope contract requires
            scopes[i] = nullptr;
    }

    CHECK(no_scope_of_ours());
    CHECK(cc::async_ambient_lookup_in(head, tag_a()) == &value_a);
    cc::impl::async_ambient_release(head); // drops all `depth` links in one loop
}

// ============================================================================
// which context a node's frame runs under
// ============================================================================

TEST("async-ambient - a node driven under a scope observes it")
{
    auto n = cc::make_async_lazy<i64>([] { return observed(); });

    cc::async_ambient_scope const s(tag_a(), &value_a);
    CHECK(cc::async_blocking_get_singlethreaded(n) == value_a);
}

TEST("async-ambient - drive site wins over creation site")
{
    // Created with NO scope active, then driven under one.
    // Creation-site attribution would report 0 here.
    auto n = cc::make_async_lazy<i64>([] { return observed(); });

    cc::async_ambient_scope const s(tag_a(), &value_a);
    CHECK(cc::async_blocking_get_singlethreaded(n) == value_a);
}

TEST("async-ambient - a lazy node outliving its creating scope is driven under the LIVE one")
{
    // The case creation-site attribution could not handle: C0 creates a lazy node, C0 dies, C1 drives it.
    // Under creation-site the node would carry a dead C0.
    // Here it simply runs under C1.
    cc::shared_async<i64> lazy;
    {
        cc::async_ambient_scope const c0(tag_a(), &value_a);
        lazy = cc::make_async_lazy<i64>([] { return observed(); });
    }
    CHECK(no_scope_of_ours()); // c0 is gone

    cc::async_ambient_scope const c1(tag_a(), &value_b);
    CHECK(cc::async_blocking_get_singlethreaded(lazy) == value_b);
}

TEST("async-ambient - an inline-driven dependency inherits its driver's context")
{
    // The eager depth-first drive: the dep is polled on the parent's stack and never scheduled, so it stores no
    // token of its own and simply runs under whatever is installed.
    auto dep = cc::make_async_lazy<i64>([] { return observed(); });
    auto parent = cc::make_async_lazy<i64>([](i64 from_dep) { return from_dep * 100 + observed(); }, dep);

    cc::async_ambient_scope const s(tag_a(), &value_a);
    CHECK(cc::async_blocking_get_singlethreaded(parent) == value_a * 100 + value_a);
}

TEST("async-ambient - a nested scope inside a frame flows to the nodes it spawns")
{
    // Contagion without per-edge machinery: a frame that pushes a scope and spawns work has that work observe it.
    auto outer = cc::make_async_lazy<i64>(
        []
        {
            cc::async_ambient_scope const inner(tag_a(), &value_b);
            auto child = cc::make_async_lazy<i64>([] { return observed(); });
            return cc::async_blocking_get_singlethreaded(child);
        });

    cc::async_ambient_scope const s(tag_a(), &value_a);
    CHECK(cc::async_blocking_get_singlethreaded(outer) == value_b); // the inner scope, not the outer
}

TEST("async-ambient - the scope is restored after a drive")
{
    auto n = cc::make_async_lazy<i64>([] { return observed(); });
    {
        cc::async_ambient_scope const s(tag_a(), &value_a);
        CHECK(cc::async_blocking_get_singlethreaded(n) == value_a);
        CHECK(cc::async_ambient_lookup(tag_a()) == &value_a); // poll() restored what it installed
    }
    CHECK(no_scope_of_ours());
}

TEST("async-ambient - a resolved node holds no context")
{
    // "A completed node carries no ambient at all": the token lives in the unresolved arm, which resolution destroys.
    // So the reference is released even though the handle is still held.
    cc::singlethreaded_scheduler sched;
    cc::shared_async<i64> n;
    {
        cc::async_worker_scope const worker(sched); // schedule() needs somewhere to route
        cc::async_ambient_scope const s(tag_a(), &value_a);

        n = cc::make_async_lazy<i64>([] { return observed(); });
        n->schedule(); // a cold->scheduled hand-off: THIS is what writes the token
        CHECK(s.outstanding() == 1);

        sched.run_until([&] { return n->is_ready(); });
        CHECK(*n->try_value() == value_a);
        CHECK(s.outstanding() == 0); // resolved, so the arm and its reference are gone
    }
    CHECK(n->is_ready());
}

TEST("async-ambient - a woken node keeps the context it parked with")
{
    // The property write-once buys, and the reason the wake path must not write.
    // A dependent parks under A; the dependency it waits on is completed from B, which schedules the dependent.
    // If the waker stamped its own context, B would leak into A's private continuation and on through A's subgraph.
    cc::singlethreaded_scheduler sched;
    auto gate = cc::make_async_manual<i64>();
    auto dependent = cc::make_async_lazy<i64>([](i64) { return observed(); }, gate);

    {
        cc::async_worker_scope const worker(sched);
        cc::async_ambient_scope const a(tag_a(), &value_a);

        dependent->schedule();
        sched.run_until([&] { return dependent->is_ready(); }); // parks on the manual node
        CHECK(!dependent->is_ready());
        CHECK(a.outstanding() == 1); // parked, so it holds A
    }

    {
        cc::async_worker_scope const worker(sched);
        cc::async_ambient_scope const b(tag_a(), &value_b);

        gate->push_value(7); // completing under B wakes and schedules the dependent
        sched.run_until([&] { return dependent->is_ready(); });

        CHECK(b.outstanding() == 0); // B never got written into anything
    }

    CHECK(*dependent->try_value() == value_a); // still A, not B
}

TEST("async-ambient - a yielding node keeps its context across the re-queue")
{
    // An inline-driven node was never scheduled, so it carries no token.
    // If yielding did not write one, it would come back off the queue with nothing installed.
    // Driving the second half OUTSIDE the scope is what exposes that.
    cc::singlethreaded_scheduler sched;

    auto dep = cc::make_async_lazy<i64>(
        [yielded = false](cc::async_context<i64>& actx) mutable -> cc::async_step_status
        {
            if (!yielded)
            {
                yielded = true;
                return actx.yield(); // re-queues via reschedule_self
            }
            return actx.success(observed());
        });
    auto parent = cc::make_async_lazy<i64>([](i64 from_dep) { return from_dep; }, dep);

    {
        cc::async_worker_scope const worker(sched);
        cc::async_ambient_scope const a(tag_a(), &value_a);

        parent->schedule();
        // One step: parent polls, inline-drives dep, dep yields and re-queues itself.
        // The singlethreaded scheduler has no steal-capable peers, so dep is never published — it really is driven inline.
        CHECK(sched.run_one());
        CHECK(!dep->is_ready());
        CHECK(a.outstanding() >= 1); // the yield wrote A into dep
    }

    // Scope gone.
    // Anything the re-poll observes now must come from the node's own token.
    CHECK(no_scope_of_ours());
    {
        cc::async_worker_scope const worker(sched);
        sched.run_until([&] { return parent->is_ready(); });
    }
    CHECK(*parent->try_value() == value_a);
}

TEST("async-ambient - a scheduled-but-undriven node keeps its context alive")
{
    // Prewarming: work started under a scope that ends before the work does.
    // The refcount is what makes this safe rather than a dangling read, and it is why cc does not assert on it.
    cc::singlethreaded_scheduler sched;
    cc::shared_async<i64> warm;
    void* held = nullptr;
    {
        cc::async_worker_scope const worker(sched);
        cc::async_ambient_scope const s(tag_a(), &value_a);

        warm = cc::make_async_lazy<i64>([] { return observed(); });
        warm->schedule(); // queued, not driven
        CHECK(s.outstanding() == 1);

        held = s.link();
        cc::impl::async_ambient_retain(held);
    }

    // The scope is gone, but the node still carries the context — and still runs under it.
    CHECK(no_scope_of_ours());
    {
        cc::async_worker_scope const worker(sched);
        sched.run_until([&] { return warm->is_ready(); });
    }
    CHECK(*warm->try_value() == value_a);
    cc::impl::async_ambient_release(held);
}
