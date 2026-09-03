#include "resolver.hh"

#include <clean-core/container/map.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <clean-net/fwd.hh>
#include <clean-net/impl/async_glue.hh>
#include <clean-net/impl/native_resolve.hh>
#include <clean-net/impl/reactor.hh>
#include <clean-net/io/io_system.hh>

// A resolve is a blocking call the reactor is not allowed to make, so it is made somewhere else and the answer comes
// back the way every other answer does.
//
// THE THREE PIECES.
// The WORKER is a cc::threaded_actor that runs the blocking lookup; with SC_THREADS=OFF it is unthreaded and the
// lookup happens inside cc::thread_pump_all(), which is the one stall this library admits to.
// The SLOT is where the worker leaves its answer, and the only thing the two threads share.
// The OPERATION is a `manual` reactor operation, so a resolve gets the same deadline and the same cancellation as
// everything else rather than a second timeout mechanism.
//
// WHY THE SLOT AND NOT THE OPERATION ITSELF.
// An operation frees itself once it completes, and a resolve can complete on its deadline while the worker is still
// inside getaddrinfo -- a call nobody can abort.
// The worker therefore never touches the operation except through the slot's lock, and the operation clears itself
// out of the slot before it dies.

namespace cnet
{
namespace
{
/// One host's addresses, and when they stop being trustworthy.
struct cache_entry
{
    cc::vector<ip_address> addresses;
    i64 expires_ns = 0;
};

/// Everything a resolve needs that outlives the resolver handle.
///
/// Shared, because an operation still in flight when the resolver is destroyed must not write into a dead cache.
struct resolver_state
{
    io_system& io;
    i64 cache_ttl_ms = 60'000;
    cc::unique_function<cc::result<cc::vector<ip_address>, error>(cc::string_view host)> lookup;
    cc::mutex<cc::map<cc::string, cache_entry>> cache;

    resolver_state(io_system& s, i64 ttl_ms) : io(s), cache_ttl_ms(ttl_ms) {}
};

/// Where the worker leaves its answer.
struct slot_data
{
    bool answered = false;
    cc::vector<ip_address> addresses;
    cc::optional<error> failure;

    /// The operation waiting for this, or null once it has completed.
    /// Read and cleared only under the lock, which is what makes signalling a completing operation safe.
    impl::io_operation* op = nullptr;
};

using resolve_slot = cc::mutex<slot_data>;

/// The addresses of `family`, in the order they arrived.
[[nodiscard]] cc::vector<ip_address> of_family(cc::vector<ip_address> const& addresses, address_family_preference family)
{
    if (family == address_family_preference::race)
        return addresses;

    auto const wanted = family == address_family_preference::v6_only ? ip_family::v6 : ip_family::v4;

    auto picked = cc::vector<ip_address>();
    for (auto const& a : addresses)
        if (a.family() == wanted)
            picked.push_back(a);
    return picked;
}

[[nodiscard]] error no_address_of_family(cc::string_view host, address_family_preference family)
{
    auto const what = family == address_family_preference::v6_only ? "IPv6" : "IPv4";
    return {.code = error_code::name_not_resolved,
            .native_code = 0,
            .message = cc::format("{} has no {} address", host, what)};
}

/// A resolve waiting for its worker, its deadline, or its token.
struct resolve_op final : impl::io_operation
{
    cc::shared_async<cc::vector<ip_address>> promise;
    cc::unique_ptr<resolve_op> self;
    impl::cancel_registration cancellation;

    cc::shared_ptr<resolver_state> state;
    cc::shared_ptr<resolve_slot> slot;
    cc::string host;
    address_family_preference family = address_family_preference::race;

    void on_complete(cc::optional<error> failure) override
    {
        auto const keep_alive_until_return = cc::move(self);
        cancellation.detach();

        // Leaving the slot first: the worker may still be inside a call nobody can abort, and it must find no
        // operation rather than a freed one when it comes back.
        auto answer = slot->lock(
            [](slot_data& d)
            {
                d.op = nullptr;
                return d.answered ? cc::optional<cc::vector<ip_address>>(cc::move(d.addresses))
                                  : cc::optional<cc::vector<ip_address>>();
            });

        if (failure.has_value())
        {
            promise->push_error(to_async_error(cc::move(failure.value())));
            return;
        }

        if (!answer.has_value())
        {
            // The worker failed, and left why in the slot.
            auto worker_failure = slot->lock([](slot_data& d) { return cc::move(d.failure); });
            promise->push_error(to_async_error(worker_failure.has_value()
                                                   ? cc::move(worker_failure.value())
                                                   : error{.code = error_code::name_not_resolved,
                                                           .native_code = 0,
                                                           .message = cc::format("could not resolve {}", host)}));
            return;
        }

        // The whole answer is cached, whatever this caller asked for, so a v4-only caller warms the cache for a
        // v6-only one.
        auto const now = state->io.time_source().now_ns();
        state->cache.lock(
            [&](cc::map<cc::string, cache_entry>& c)
            {
                c.entry(host).get_or_emplace()
                    = {.addresses = answer.value(), .expires_ns = now + state->cache_ttl_ms * 1000 * 1000};
            });

        auto picked = of_family(answer.value(), family);
        if (picked.empty())
        {
            promise->push_error(to_async_error(no_address_of_family(host, family)));
            return;
        }
        promise->push_value(cc::move(picked));
    }
};

struct lookup_request
{
    cc::shared_ptr<resolver_state> state;
    cc::shared_ptr<resolve_slot> slot;
    cc::string host;
};

using worker_handle = cc::threaded_actor<lookup_request>;

/// The thread the blocking call happens on, and nothing else.
class resolver_actor final : public cc::threaded_actor_impl<lookup_request>
{
protected:
    [[nodiscard]] cc::string_view actor_name() const noexcept override { return "cnet.resolver"; }

    void on_message(lookup_request msg) override
    {
        // THIS BLOCKS, for as long as the network takes, and that is the entire reason this class exists.
        auto answer = msg.state->lookup.is_valid() ? msg.state->lookup(msg.host) : impl::resolve_blocking(msg.host);

        // Signalling under the lock is what keeps it safe: an operation that is completing right now is blocked in
        // here, so it is still alive, and one that already completed left null behind.
        msg.slot->lock(
            [&](slot_data& d)
            {
                d.answered = answer.has_value();
                if (answer.has_value())
                    d.addresses = cc::move(answer).value();
                else
                    d.failure = cc::move(answer).error();

                if (d.op != nullptr)
                    msg.state->io.signal(d.op);
            });
    }
};
} // namespace

/// What the resolver holds, so that threaded_actor.hh stays out of its header.
class impl::resolver_worker
{
public:
    resolver_worker(cc::shared_ptr<resolver_state> state) : _state(cc::move(state)) {}

    ~resolver_worker()
    {
        if (_handle.is_valid())
            _handle->shutdown();
    }

    void start()
    {
        _handle = cc::make_threaded_actor<resolver_actor>();
        _handle->start(cc::threaded_actor_mode::threaded_if_possible);
    }

    [[nodiscard]] resolver_state& state() const { return *_state; }
    [[nodiscard]] cc::shared_ptr<resolver_state> const& shared_state() const { return _state; }

    void enqueue(cc::shared_ptr<resolve_slot> slot, cc::string host)
    {
        auto const slot_copy = slot;
        if (_handle->enqueue_message(lookup_request{.state = _state, .slot = cc::move(slot), .host = cc::move(host)}))
            return;

        // The worker is shutting down and nothing will ever run this, so the waiting operation is answered here
        // rather than left to its deadline -- every resolve is answered exactly once.
        slot_copy->lock(
            [&](slot_data& d)
            {
                d.answered = false;
                d.failure = error{.code = error_code::cancelled,
                                  .native_code = 0,
                                  .message = cc::string("the resolver is shutting down")};
                if (d.op != nullptr)
                    _state->io.signal(d.op);
            });
    }

private:
    cc::shared_ptr<resolver_state> _state;
    cc::unique_ptr<worker_handle> _handle;
};

// ---- the resolver --------------------------------------------------------------------------------------

resolver::resolver(cc::unique_ptr<impl::resolver_worker> worker) : _worker(cc::move(worker))
{
}

resolver::~resolver() = default;

bool resolver::is_supported()
{
    return impl::resolve_is_supported();
}

cc::result<cc::unique_ptr<resolver>, error> resolver::try_create(io_system& io, resolver_description desc)
{
    // A supplied lookup needs nothing from the platform, which is what makes a resolver testable on a machine that
    // cannot resolve at all.
    if (!impl::resolve_is_supported() && !desc.lookup.is_valid())
        return cc::error(unsupported_here("name resolution"));

    auto state = cc::make_shared<resolver_state>(io, desc.cache_ttl_ms);
    state->lookup = cc::move(desc.lookup);

    auto worker = cc::make_unique<impl::resolver_worker>(cc::move(state));
    worker->start();

    return cc::make_unique<resolver>(cc::move(worker));
}

cc::unique_ptr<resolver> resolver::create(io_system& io, resolver_description desc)
{
    return try_create(io, cc::move(desc)).or_throw();
}

void resolver::clear_cache()
{
    _worker->state().cache.lock([](cc::map<cc::string, cache_entry>& c) { c.clear(); });
}

isize resolver::cached_host_count() const
{
    return _worker->state().cache.lock([](cc::map<cc::string, cache_entry> const& c) { return c.size(); });
}

cc::shared_async<cc::vector<ip_address>> resolver::resolve(cc::string_view host,
                                                           resolve_options const& options,
                                                           cancel_token const& token)
{
    using addresses_t = cc::vector<ip_address>;

    auto& state = _worker->state();

    // An address in text is already an answer, and a caller should not have to know which kind of string it holds.
    if (auto const literal = ip_address::parse(host); literal.has_value())
    {
        auto picked = of_family(addresses_t{literal.value()}, options.family);
        if (picked.empty())
            return impl::failed_async<addresses_t>(no_address_of_family(host, options.family));

        auto promise = cc::make_async_manual<addresses_t>();
        promise->push_value(cc::move(picked));
        return promise;
    }

    if (token.is_cancelled())
        return impl::failed_async<addresses_t>(
            {.code = error_code::cancelled, .native_code = 0, .message = cc::string("the operation was cancelled")});

    // A cache hit is the common case and never reaches the worker, which is what confines a blocking lookup -- and,
    // in a threads-off build, the stall -- to first contact with a host.
    auto const now = state.io.time_source().now_ns();
    auto cached = state.cache.lock(
        [&](cc::map<cc::string, cache_entry>& c) -> cc::optional<addresses_t>
        {
            auto* const found = c.get_ptr(host);
            if (found == nullptr)
                return {};
            if (found->expires_ns <= now)
                return {};
            return found->addresses;
        });

    if (cached.has_value())
    {
        auto picked = of_family(cached.value(), options.family);
        if (picked.empty())
            return impl::failed_async<addresses_t>(no_address_of_family(host, options.family));

        auto promise = cc::make_async_manual<addresses_t>();
        promise->push_value(cc::move(picked));
        return promise;
    }

    auto slot = cc::make_shared<resolve_slot>();

    auto op = cc::make_unique<resolve_op>();
    op->kind = impl::io_op_kind::manual;
    op->deadline_ns = deadline_to_absolute(state.io, options.timeout);
    op->state = _worker->shared_state();
    op->slot = slot;
    op->host = cc::string(host);
    op->family = options.family;

    auto promise = cc::make_async_manual<addresses_t>();
    op->promise = promise;

    auto* const raw = op.get();
    raw->self = cc::move(op);

    slot->lock([raw](slot_data& d) { d.op = raw; });
    state.io.submit(raw);
    raw->cancellation.attach(token, state.io, raw);

    // Handed over only once the operation is in the reactor, so an answer that arrives at once has something to
    // signal.
    _worker->enqueue(cc::move(slot), cc::string(host));

    CC_LOG_TRACE("resolving {}", host);
    return promise;
}
} // namespace cnet
