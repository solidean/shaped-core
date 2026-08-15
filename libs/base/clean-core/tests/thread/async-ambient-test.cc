#include <clean-core/container/vector.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/async_ambient.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// The ambient-context chain, exercised on its own — no async nodes involved.
// The node-side half (which context a frame runs under, and who writes it) is covered in async-test.cc / async-pool-test.cc.

namespace
{
// Two independent "consumers", to pin that the chain composes and that tags do not collide.
CC_ASYNC_AMBIENT_TAG(tag_a)
CC_ASYNC_AMBIENT_TAG(tag_b)

int value_a = 1;
int value_b = 2;
} // namespace

TEST("async-ambient - nothing installed by default")
{
    CHECK(cc::async_current_ambient() == nullptr);
    CHECK(cc::async_ambient_lookup(tag_a()) == nullptr);
}

TEST("async-ambient - a scope installs and pops")
{
    {
        cc::async_ambient_scope const s(tag_a(), &value_a);
        CHECK(cc::async_current_ambient() != nullptr);
        CHECK(cc::async_ambient_lookup(tag_a()) == &value_a);
        CHECK(cc::async_ambient_lookup(tag_b()) == nullptr); // a miss walks the whole chain and reports absence
    }
    CHECK(cc::async_current_ambient() == nullptr);
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

    // The scope is gone and TLS is clean, but the chain the captured head names is intact.
    CHECK(cc::async_current_ambient() == nullptr);
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

    CHECK(cc::async_current_ambient() == nullptr);
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

    CHECK(cc::async_current_ambient() == nullptr);
    CHECK(cc::async_ambient_lookup_in(head, tag_a()) == &value_a);
    cc::impl::async_ambient_release(head); // drops all `depth` links in one loop
}
