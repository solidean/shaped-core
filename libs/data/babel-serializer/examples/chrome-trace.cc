#include <babel-serializer/trace/chrome_trace.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/recording.hh>
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

/// Stands in for a frame: nested scopes, a stat that moves, and a value worth correlating against the duration.
void render_frame(int index)
{
    CC_RECORD_SCOPE("frame");
    CC_RECORD("frame_index", index);

    {
        CC_RECORD_SCOPE("cull");
        auto const visible = int(grind(u64(index) + 1, 200000 + index * 50000) % 4096);
        CC_RECORD("visible_objects", visible);
        CC_RECORD_STAT("visible", cc::rec::unit_count, visible);
    }

    {
        CC_RECORD_SCOPE("draw");
        {
            CC_RECORD_SCOPE("opaque");
            CC_RECORD_ACCUM("draw_calls", cc::rec::unit_count, 120);
            CC_RECORD_ACCUM("bytes_uploaded", cc::rec::unit_bytes, 48 * 1024);
            (void)grind(7, 250000);
        }
        {
            CC_RECORD_SCOPE("transparent");
            CC_RECORD_ACCUM("draw_calls", cc::rec::unit_count, 18);
            (void)grind(11, 80000);

            // Every third frame takes the slow path, which is exactly the kind of thing a marker makes findable.
            if (index % 3 == 0)
            {
                CC_RECORD_MARK("sort-fallback");
                CC_LOG_WARNING("transparent sort fell back on frame {}", index);
                (void)grind(13, 300000);
            }
        }
    }

    {
        CC_RECORD_SCOPE("present");
        (void)grind(17, 60000);
    }
}
} // namespace example_render

namespace example_assets
{
using namespace cc::primitive_defines;

CC_REC_DECLARE_DOMAIN(g_rec_domain);
CC_REC_DEFINE_DOMAIN(g_rec_domain, "example.assets");

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

        auto const checksum = example_render::grind(u64(i) * 31 + 5, 500000);
        CC_RECORD("checksum", checksum);
    }

    CC_LOG_INFO("asset loading finished");
}
} // namespace example_assets

EXAMPLE("babel-serializer/chrome-trace")
{
    using namespace cc::primitive_defines;

    // Nothing is recorded until the system is up — a library never decides this on the program's behalf.
    cc::rec::initialize();
    cc::rec::set_current_thread_record_name("main");

    cc::rec::recording captured;
    {
        // Capturing is "hold references to the chunks", so this costs no copying at all.
        cc::rec::recording_listener capture;
        auto const handle = cc::rec::register_listener(capture);

        CC_LOG_INFO("starting the synthetic workload");

        auto loader = std::thread(example_assets::load_assets);
        for (auto frame = 0; frame < 8; ++frame)
            example_render::render_frame(frame);
        loader.join();

        CC_LOG_INFO("workload done");

        // Everything published before this call has been offered to every listener by the time it returns.
        cc::rec::flush_blocking();
        cc::rec::unregister_listener(handle);

        captured = capture.take();
    }

    //
    // Everything here reads the recording as a value; no exporter is involved yet.
    //

    cc::println("recorded {} events across {} block(s)", captured.event_count(), captured.block_count());
    cc::println("  {} scope(s), {} log message(s)", captured.scopes().size(), captured.messages().size());
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

    //
    // And out to a file a viewer can open.
    //

    auto const path = cc::format("{}/cc-record-example.json", cc::temp_directory_path());

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
