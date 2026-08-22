#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/listener.hh>
#include <clean-core/record/overhead.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/system.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>

// Shared scaffolding for the cc::rec tests.
//
// The recording system is a process-wide singleton with one initialize/shutdown pair, and `shutdown()` requires that
// no other thread is recording — see libs/base/clean-core/docs/systems/recording.md, "Lifecycle constraints".
// Nexus opens an ambient scope per test, and an ambient scope publishes a delta, so EVERY test in this binary is a
// recording thread the moment a fixture has the system up.
// That is why these run alone rather than under a shared tag: an unrelated test in flight would be writing into the
// pool a fixture is tearing down.

#define REC_TEST(name_) TEST(name_, nx::config::exclusive(), nx::config::owns_recorder)

namespace cc_rec_test
{
using namespace cc::primitive_defines;

struct rec_fixture;
struct collector;
struct scoped_listener;
struct scoped_domain_mask;
struct scoped_overhead;
} // namespace cc_rec_test

/// Brings the system up for one test and tears it down again, whatever the body does.
struct cc_rec_test::rec_fixture
{
    explicit rec_fixture(cc::rec::config cfg) { cc::rec::initialize(cfg); }
    ~rec_fixture() { cc::rec::shutdown(); }

    rec_fixture(rec_fixture const&) = delete;
    rec_fixture& operator=(rec_fixture const&) = delete;
};

namespace cc_rec_test
{
/// The deterministic configuration: no background thread, so a flush_blocking() is the only thing that ever drains.
/// This is what lets the core be tested without injecting a clock or a scheduler, which would tax every real use.
inline cc::rec::config deterministic_config()
{
    auto cfg = cc::rec::config{};
    cfg.threaded = false;
    cfg.chunk_bytes = 64 * 1024;
    cfg.budget_bytes = 4 * 1024 * 1024;
    cfg.overflow = cc::rec::overflow_policy::grow_unbounded;
    cfg.ready_chunks = 2;
    return cfg;
}

/// Whether this build can spawn a second thread at all.
///
/// `SC_THREADS=OFF` is a real configuration rather than a formality: under Emscripten without pthreads a `std::thread`
/// constructor THROWS ("Not supported"), which surfaces as an uncaught exception rather than as a failed check.
/// So a test that needs a second thread says so and skips, the same way the sampler tests do.
[[nodiscard]] constexpr bool threads_available()
{
    return CC_HAS_THREADS != 0;
}

/// A recording's event sequence, block by block, as something two recordings can be compared on.
///
/// Counts are a weak assertion for anything that MOVES events: a transform that relocates every sample to the end
/// preserves every count there is and still changed the answer.
/// The layout is what a reader sees, so it is what a test that cares about placement has to pin.
/// One string rather than a list, so a CHECK can simply compare them and print both when they differ.
inline cc::string event_layout(cc::rec::recording const& r)
{
    cc::string out;
    for (auto const& b : r.blocks())
    {
        out += "|";
        auto const v = b.view();
        for (auto it = v.begin(); it != v.end(); ++it)
        {
            auto const e = *it;
            out += cc::format("{}@{};", int(e.kind()), e.cycles);
        }
    }
    return out;
}
} // namespace cc_rec_test

/// Collects every event it is offered, so a test can assert on what actually reached a listener.
struct cc_rec_test::collector final : cc::rec::listener
{
    struct entry
    {
        cc::string name;
        cc::string text;
        cc::rec::event_kind kind = cc::rec::event_kind::invalid;
        cc::rec::level level = cc::rec::level::info;
        u64 cycles = 0;
        cc::string domain;
        u16 layer = 0;
        cc::optional<f64> value;
        cc::optional<i64> depth;
        cc::rec::unit const* quantity = nullptr;
    };

    void on_chunk(cc::rec::chunk_view const& view) override
    {
        for (auto it = view.begin(); it != view.end(); ++it)
        {
            auto const e = *it;
            events.push_back({
                .name = cc::string(e.name()),
                .text = cc::string(e.payload_as_text()),
                .kind = e.kind(),
                .level = e.level(),
                .cycles = e.cycles,
                .domain = cc::string(e.domain()->name()),
                .layer = view.layer,
                .value = e.field_as_double("value"),
                .depth = e.field_as_int("depth"),
                .quantity = e.quantity(),
            });
        }
        ++chunk_count;
    }

    [[nodiscard]] cc::string_view listener_name() const override { return "collector"; }

    [[nodiscard]] isize count_named(cc::string_view n) const
    {
        isize c = 0;
        for (auto const& e : events)
            if (cc::string_view(e.name) == n)
                ++c;
        return c;
    }

    /// The first event with this name, or null.
    [[nodiscard]] entry const* first_named(cc::string_view n) const
    {
        for (auto const& e : events)
            if (cc::string_view(e.name) == n)
                return &e;
        return nullptr;
    }

    /// Every event of one kind, in arrival order.
    [[nodiscard]] cc::vector<entry const*> of_kind(cc::rec::event_kind k) const
    {
        cc::vector<entry const*> out;
        for (auto const& e : events)
            if (e.kind == k)
                out.push_back(&e);
        return out;
    }

    cc::vector<entry> events;
    isize chunk_count = 0;
};

/// Registers a listener for a scope and unregisters it again, so a test never leaves one behind.
struct cc_rec_test::scoped_listener
{
    explicit scoped_listener(cc::rec::listener& l) : _handle(cc::rec::register_listener(l)) {}
    ~scoped_listener() { cc::rec::unregister_listener(_handle); }

    scoped_listener(scoped_listener const&) = delete;
    scoped_listener& operator=(scoped_listener const&) = delete;

    [[nodiscard]] isize layer() const { return _handle.layer(); }

private:
    cc::rec::listener_handle _handle;
};

/// Puts the overhead model back the way it was, so one test's measurement cannot leak into another's expectations.
struct cc_rec_test::scoped_overhead
{
    explicit scoped_overhead(cc::rec::overhead_model const& model) : _saved(cc::rec::overhead())
    {
        cc::rec::set_overhead(model);
    }
    ~scoped_overhead() { cc::rec::set_overhead(_saved); }

    scoped_overhead(scoped_overhead const&) = delete;
    scoped_overhead& operator=(scoped_overhead const&) = delete;

private:
    cc::rec::overhead_model _saved;
};

/// Puts a domain's enable mask back the way it was, so one test's reconfiguration cannot leak into another.
struct cc_rec_test::scoped_domain_mask
{
    explicit scoped_domain_mask(cc::rec::domain& d) : _d(&d), _saved(d.enabled_mask()) {}
    ~scoped_domain_mask() { _d->set_enabled_mask(_saved); }

    scoped_domain_mask(scoped_domain_mask const&) = delete;
    scoped_domain_mask& operator=(scoped_domain_mask const&) = delete;

private:
    cc::rec::domain* _d;
    u32 _saved;
};
