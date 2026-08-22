#include <babel-serializer/trace/chrome_trace.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/record/console_listener.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/hot_functions.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/sampling.hh>
#include <clean-core/record/system.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>

#include <thread>

// Recording a synthetic workload and writing it out as a Chrome trace.
//
// Two namespaces with a domain each, because that is the real shape: a library declares one in its fwd.hh and never
// mentions it again, and every site inside it is attributed by ordinary name lookup.
// In the trace they become categories, so a viewer can show one subsystem at a time.

namespace example_render
{
using namespace cc::primitive_defines;

CC_REC_DECLARE_DOMAIN(g_rec_domain);
CC_REC_DEFINE_DOMAIN(g_rec_domain, "example.render");

/// Work the optimizer cannot fold away, so the scopes have a duration worth looking at.
///
/// A plain arithmetic recurrence is not enough — clang closes one of those into a constant, and the first version of
/// this example measured a frame at 12 us because of it.
/// A dependent load/store chain, where each step's address comes from the previous step's value, is not foldable.
u64 grind(u64 seed, int rounds)
{
    u64 table[256] = {};
    for (auto i = 0; i < 256; ++i)
        table[i] = seed + u64(i) * 0x9E3779B97F4A7C15ull;

    auto acc = seed;
    for (auto i = 0; i < rounds; ++i)
    {
        auto const slot = acc & 255;
        acc = acc * 6364136223846793005ull + table[slot];
        table[slot] = acc;
    }
    return acc;
}

// Three levels of UNINSTRUMENTED call under each scope, which is what gives the sampler something to say.
//
// A scope names the work a person thought was worth naming; these are the frames nobody named, and they are the whole
// reason to sample at all.
// Without them a sample inside a scope is one address, because there is nothing between the scope and the work.
//
// Each one does something with the result rather than forwarding it, because `return f(x)` is a TAIL CALL: the
// compiler turns it into a jump, no frame is pushed, and a sampler correctly reports a stack that is one level deep.
// A wrapper that only forwards is invisible to a profiler.
u64 volatile g_keep_frames = 0;

CC_DONT_INLINE u64 inner_math(u64 seed, int rounds)
{
    auto const r = grind(seed, rounds);
    g_keep_frames = r;
    return r;
}
CC_DONT_INLINE u64 middle_transform(u64 seed, int rounds)
{
    auto const r = inner_math(seed, rounds);
    g_keep_frames = r;
    return r;
}
CC_DONT_INLINE u64 outer_prepare(u64 seed, int rounds)
{
    auto const r = middle_transform(seed, rounds);
    g_keep_frames = r;
    return r;
}

/// Stands in for a frame: nested scopes, a stat that moves, and a value worth correlating against the duration.
void render_frame(int index)
{
    CC_RECORD_SCOPE("frame");
    CC_RECORD("frame_index", index);

    {
        CC_RECORD_SCOPE("cull");
        auto const visible = int(outer_prepare(u64(index) + 1, 200000 + index * 50000) % 4096);
        CC_RECORD("visible_objects", visible);
        CC_RECORD_STAT("visible", cc::rec::unit_count, visible);
    }

    {
        CC_RECORD_SCOPE("draw");
        {
            CC_RECORD_SCOPE("opaque");
            CC_RECORD_ACCUM("draw_calls", cc::rec::unit_count, 120);
            CC_RECORD_ACCUM("bytes_uploaded", cc::rec::unit_bytes, 48 * 1024);
            (void)outer_prepare(7, 250000);
        }
        {
            CC_RECORD_SCOPE("transparent");
            CC_RECORD_ACCUM("draw_calls", cc::rec::unit_count, 18);
            (void)middle_transform(11, 80000);

            // Every third frame takes the slow path, which is exactly the kind of thing a marker makes findable.
            if (index % 3 == 0)
            {
                CC_RECORD_MARK("sort-fallback");
                CC_LOG_WARNING("transparent sort fell back on frame {}", index);
                (void)outer_prepare(13, 300000);
            }
        }
    }

    {
        CC_RECORD_SCOPE("present");
        (void)inner_math(17, 60000);
    }
}
} // namespace example_render

namespace example_assets
{
using namespace cc::primitive_defines;

CC_REC_DECLARE_DOMAIN(g_rec_domain);
CC_REC_DEFINE_DOMAIN(g_rec_domain, "example.assets");

// This lane's own uninstrumented chain, so a sampled frame here is named for the work THIS thread is doing.
//
// It used to reach straight into example_render's helpers, and the trace was correct and unreadable for it: real
// `example_render::inner_math` spans nested inside `decode_texture`, which looks exactly like a misattribution and is
// not one.
// The lesson generalizes past this example — a shared leaf makes a sampled profile ambiguous about WHY it was reached,
// and the scope stack around it is what disambiguates.
u64 volatile g_keep_frames = 0;

CC_DONT_INLINE u64 checksum_block(u64 seed, int rounds)
{
    auto const r = example_render::grind(seed, rounds);
    g_keep_frames = r;
    return r;
}
CC_DONT_INLINE u64 inflate_block(u64 seed, int rounds)
{
    auto const r = checksum_block(seed, rounds);
    g_keep_frames = r;
    return r;
}
CC_DONT_INLINE u64 decode_pixels(u64 seed, int rounds)
{
    auto const r = inflate_block(seed, rounds);
    g_keep_frames = r;
    return r;
}

/// A second thread, so the trace has more than one lane and the ordering across them is visible.
void load_assets()
{
    cc::rec::set_current_thread_record_name("asset-loader");
    CC_RECORD_SCOPE("load_assets");

    for (auto i = 0; i < 5; ++i)
    {
        CC_RECORD_SCOPE("decode_texture");
        CC_RECORD("texture_index", i);
        CC_RECORD("source", "meshes/tree.png");
        CC_RECORD_ACCUM("bytes_uploaded", cc::rec::unit_bytes, 256 * 1024);

        auto const checksum = decode_pixels(u64(i) * 31 + 5, 500000);
        CC_RECORD("checksum", checksum);
    }

    CC_LOG_INFO("asset loading finished");
}
} // namespace example_assets

// owns_recorder because this drives cc::rec::initialize itself, and nx::run stands a recorder up for the whole binary.
// Without it the initialize below is a second one, which asserts.
//
// exclusive() comes with it, and untagged: the recorder is process-wide, so handing it to one entry takes it away
// from everything running alongside — which nexus asserts on rather than letting it produce a torn recording.
EXAMPLE("babel-serializer/chrome-trace", nx::config::exclusive(), nx::config::owns_recorder)
{
    using namespace cc::primitive_defines;

    // Nothing is recorded until the system is up — a library never decides this on the program's behalf.
    cc::rec::initialize();
    cc::rec::set_current_thread_record_name("main");

    // What an application writes to get its own log onto its terminal.
    // Registered first, so it is the layer everything else records BELOW — and default-constructed, so the run
    // honors CC_LOG_LEVEL, CC_LOG_COLOR and friends:
    //   CC_LOG_LEVEL=debug uv run dev.py example babel-serializer/chrome-trace
    cc::rec::install_default_console_listener();

    cc::rec::recording captured;
    {
        // Capturing is "hold references to the chunks", so this costs no copying at all.
        cc::rec::recording_listener capture;
        auto const handle = cc::rec::register_listener(capture);

        CC_LOG_INFO("starting the synthetic workload");

        // Sampling runs alongside the instrumentation and answers the other half of the question: the scopes below say
        // how long the named things took, and the samples say where the time went in everything nobody named.
        cc::rec::sampling_scope const sampling({.rate_hz = 4000.0});

        auto loader = std::thread(example_assets::load_assets);
        for (auto frame = 0; frame < 24; ++frame)
            example_render::render_frame(frame);
        loader.join();

        CC_LOG_INFO("workload done");

        // Everything published before this call has been offered to every listener by the time it returns.
        cc::rec::flush_blocking();
        cc::rec::unregister_listener(handle);

        // Samples arrive on the sampler's own stream carrying an anchor; splicing puts each one back on the thread
        // it caught, at the point that thread's stream had reached.
        captured = capture.take().spliced_samples();
    }

    //
    // Everything here reads the recording as a value; no exporter is involved yet.
    //

    cc::println("recorded {} events across {} block(s)", captured.event_count(), captured.block_count());
    cc::println("  {} scope(s), {} log message(s)", captured.scopes().size(), captured.messages().size());

    auto const stats = cc::rec::sampling_statistics();
    cc::println("  {} sample(s) taken, {} tick(s) found nothing to sample",
                captured.count_of_kind(cc::rec::event_kind::sample), stats.idle);
    isize deepest = 0;
    captured.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() == cc::rec::event_kind::sample)
                deepest = cc::max(deepest, e.field_as_u64_array("frames").size());
        });
    cc::println("  deepest sampled stack: {} frame(s)", deepest);
    cc::println("  (a sample stops at the innermost open scope, so it carries only the frames NOBODY named —");
    cc::println("   the exporter nests those inside that scope, so a viewer shows both depths at once)");
    cc::println("  the slow path was taken {} time(s)", captured.count("sort-fallback"));

    auto uploaded = 0.0;
    for (auto const v : captured.values("bytes_uploaded"))
        uploaded += v;
    cc::println("  {:.0f} KiB uploaded in total", uploaded / 1024);

    cc::println("");
    for (auto const& frame : captured.scopes("frame"))
        cc::println("  frame took {:.3f} ms", frame.duration_secs() * 1000);

    // The two domains are what become trace categories.
    cc::println("");
    cc::println("  {} event(s) from example.render", captured.from_domain(&example_render::g_rec_domain).event_count());
    cc::println("  {} event(s) from example.assets", captured.from_domain(&example_assets::g_rec_domain).event_count());

    // The same samples the trace shows, reduced to where the time actually went.
    // That reduction is the point: a sampled recording is answerable without opening a viewer, which is what lets a
    // test assert on it.
    auto const hot = cc::rec::hot_functions(captured);
    if (hot.sample_count > 0)
    {
        cc::println("");
        cc::print("{}", hot.to_string(10));
    }

    //
    // And out to a file a viewer can open.
    //

    // CC_TRACE_OUT overrides it, because an example runs with the build directory as its working directory and a
    // relative path would land somewhere nobody looks.
    auto const path = cc::environment_variable("CC_TRACE_OUT")
                          .value_or(cc::format("{}/cc-record-example.json", cc::temp_directory_path()));

    auto opened = cc::file_write_stream_adapter::create(path);
    if (opened.has_error())
    {
        cc::eprintln("could not open {}", path);
        return;
    }

    // A seekable stream narrows to a plain write_stream, which is all the exporter asks for.
    cc::write_stream stream = opened.value().stream();
    if (babel::chrome_trace::write(stream, captured).has_error())
    {
        cc::eprintln("could not encode the trace");
        return;
    }
    if (stream.flush().has_error())
    {
        cc::eprintln("could not flush {}", path);
        return;
    }

    cc::println("");
    cc::println("wrote {}", path);
    cc::println("open it in chrome://tracing or at https://ui.perfetto.dev");

    cc::rec::shutdown();
}
