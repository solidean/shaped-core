// The cost of reserving one event, which is the path every recorded event takes.
//
// `cc::rec::open_event` is out of line in a static library and nothing here is built with LTO, so the call is a real
// call and its arguments are real runtime values — exactly what a caller sees.
//
// **The reservation is never committed, on purpose.**
// An uncommitted writer leaves the chunk cursor where it was, so the loop measures the reserve path in a steady state:
// the same branch outcome every iteration, no rotation, and no chunk the pool has to keep allocating.

#include <clean-core/record/record.hh>
#include <clean-core/record/system.hh>
#include <clean-core/record/writer.hh>
#include <nexus/bench/barriers.hh>
#include <nexus/bench/run.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
/// The system, up for the duration of the benchmark and down again afterwards.
struct bench_recorder
{
    explicit bench_recorder(cc::rec::config cfg) { cc::rec::initialize(cfg); }
    ~bench_recorder() { cc::rec::shutdown(); }

    bench_recorder(bench_recorder const&) = delete;
    bench_recorder& operator=(bench_recorder const&) = delete;
};
} // namespace

BENCHMARK("bench-rec-open-event - reserve one event")
{
    auto cfg = cc::rec::config{};
    cfg.threaded = false; // no consumer thread, so nothing competes for the chunk the loop writes into
    cfg.overflow = cc::rec::overflow_policy::grow_unbounded;
    bench_recorder const recorder(cfg);

    CC_REC_DEFINE_DESC(bench_desc, cc::rec::event_kind::log, cc::rec::level::info,
                       cc::rec::enable_bit_of(cc::rec::level::info), "bench-open-event", nullptr, nullptr, 0,
                       cc::rec::desc::variable_payload);

    REQUIRE(cc::rec::is_recording(bench_desc));

    // The batch form: the body owns the inner loop, so the harness costs nothing per reservation.
    constexpr auto run_cfg = nx::bench::run_config{.min_time_secs = 0.2, .max_samples = 2048};

    (void)nx::bench::run("open_event(desc, 64, 1)", run_cfg,
                         [&](isize count)
                         {
                             for (isize i = 0; i < count; ++i)
                             {
                                 auto const w = cc::rec::open_event(bench_desc, 64, 1);
                                 nx::bench::sink(w.is_open());
                             }
                         });
}
