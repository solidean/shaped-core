#include "record-test-types.hh"

#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/record/async_scope.hh>
#include <clean-core/record/impl/thread_state.hh>
#include <clean-core/record/pinned_value.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/sampling.hh>
#include <clean-core/record/serialize.hh>
#include <clean-core/record/system.hh>
#include <clean-core/record/trace.hh>
#include <clean-core/record/writer.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/native_thread.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>

// The C++ half of the Python reader's round trip (libs/base/clean-core/tools/ccrec-self-test.py).
//
// It records a workload touching every event kind and payload type, saves it, loads it back through the C++ loader,
// and writes a manifest of what that loader reads: every block, event and decoded field, and every scope pair.
// The Python reader must read the same file to the same manifest, so a format change that does not update the reader
// fails there.
// With `CC_REC_READER_FIXTURE_DIR` set, both files are left in that directory for the reader; without it they go to
// temporary files and only the C++ half runs.

using namespace cc::primitive_defines;
using namespace cc_rec_test;

namespace
{
/// Lowercase hex of `bytes`, so a manifest line never holds a tab or a newline it did not put there.
cc::string hex(cc::span<byte const> bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    auto out = cc::string();
    for (auto const b : bytes)
    {
        out += digits[u8(b) >> 4];
        out += digits[u8(b) & 15];
    }
    return out;
}

cc::string hex(cc::string_view text)
{
    return hex(cc::span<byte const>(reinterpret_cast<byte const*>(text.data()), text.size()));
}

/// A double's bits, so a value compares exactly rather than through decimal formatting.
u64 bits_of(f64 v)
{
    u64 out = 0;
    cc::memcpy(&out, &v, sizeof(out));
    return out;
}

/// One field as the C++ accessors read it, `-` where they read nothing.
cc::string field_value(cc::rec::event_view const& e, cc::rec::field const& f)
{
    using tc = cc::rec::type_code;
    auto const name = cc::string_view(f.name);
    switch (f.type)
    {
    case tc::boolean:
    case tc::i8_:
    case tc::i16_:
    case tc::i32_:
    case tc::i64_:
    {
        auto const v = e.field_as_int(name);
        return v.has_value() ? cc::format("{}", v.value()) : cc::string("-");
    }
    case tc::u8_:
    case tc::u16_:
    case tc::u32_:
    case tc::u64_:
    case tc::pointer:
    {
        auto const v = e.field_as_u64(name);
        return v.has_value() ? cc::format("{}", v.value()) : cc::string("-");
    }
    case tc::f32_:
    case tc::f64_:
    {
        auto const v = e.field_as_double(name);
        return v.has_value() ? cc::format("{}", bits_of(v.value())) : cc::string("-");
    }
    case tc::cstring:
    case tc::inline_text:
    {
        auto const v = e.field_as_text(name);
        return v.has_value() ? hex(v.value()) : cc::string("-");
    }
    case tc::pinned_bytes:
        return hex(e.field_as_bytes(name));
    case tc::u64_array:
    {
        auto out = cc::string("[");
        for (auto const v : e.field_as_u64_array(name))
            out += cc::format("{},", v);
        return out + "]";
    }
    case tc::desc_ref:
    {
        auto const* const d = e.field_as_desc(name);
        return d != nullptr ? hex(cc::string_view(d->name)) : cc::string("-");
    }
    default:
        return cc::string("-");
    }
}

/// What the C++ loader reads out of a recording, one fact per line; the format is ccrec-self-test.py's to parse.
cc::string manifest_of(cc::rec::loaded_recording const& loaded)
{
    auto const& r = loaded.events();
    auto out = cc::string("ccrec-manifest\t1\n");
    out += cc::format("header\t{}\t{}\t{}\n", cc::rec::serialized_version, bits_of(loaded.cycles_per_second()),
                      loaded.is_truncated() ? 1 : 0);

    for (auto const& b : r.blocks())
    {
        auto const v = b.view();
        isize count = 0;
        for (auto it = v.begin(); it != v.end(); ++it)
            ++count;

        out += cc::format("block\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", v.thread.index, u64(v.thread.id),
                          v.chunk_seq, v.layer, v.base_cycles, bits_of(v.base_wall_secs), v.seal_cycles,
                          bits_of(v.seal_wall_secs), hex(v.thread.name), count);

        for (auto it = v.begin(); it != v.end(); ++it)
        {
            auto const e = *it;
            out += cc::format("event\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", e.cycles, int(e.kind()),
                              int(e.level()), e.flags, e.core, e.payload.size(), hex(e.domain()->name()),
                              hex(e.site().file), e.site().line, hex(e.name()));
            for (auto const& f : e.fields())
                out += cc::format("field\t{}\t{}\t{}\n", hex(cc::string_view(f.name)), int(f.type), field_value(e, f));
        }
    }

    for (auto const& s : r.scopes())
        out += cc::format("scope\t{}\t{}\t{}\t{}\t{}\t{}\n", u64(s.thread), s.begin_cycles, s.end_cycles, s.depth,
                          s.is_open ? 1 : 0, hex(s.name()));

    for (auto const& m : r.messages())
        out += cc::format("message\t{}\n", hex(m));

    for (auto const& rel : r.trace_relations())
    {
        out += cc::format("relation\t{}\t", hex(cc::string_view(rel.type != nullptr ? rel.type->name : "")));
        for (auto const id : rel.members)
            out += cc::format("{},", u64(id));
        out += "\n";
    }
    return out;
}

/// Hands each worker's turn to the next, so the two "worker" scopes are certain to overlap.
cc::atomic<int> g_step = 0;

void wait_for_step(int step)
{
    while (g_step.load() < step)
        cc::this_thread_yield();
}

/// The innermost scope; the first worker's last one also carries the filler, a sample and a thread_run.
void leaf(int which, i32 k)
{
    CC_RECORD_SCOPE("leaf");
    if (which != 0 || k != 2)
        return;

    for (isize i = 0; i < 4000; ++i)
        CC_RECORD_MARK("filler");

    // A sample and a thread_run of this very thread, written the way the sampler writes them.
    // A live sampler is neither deterministic nor everywhere, and the layouts are what the reader has to agree on.
    auto& w = cc::rec::impl::t_writer;
    auto const thread_index = w.state->index;
    auto const chunk_offset = u32(w.cur - w.current->data);
    auto const chunk_seq = w.current->seq;
    {
        u64 const frames[] = {0x1000, 0x2000, 0x3000};
        auto writer = cc::rec::open_event(cc::rec::impl::sample_desc(), cc::rec::impl::sample_frames_offset + 24,
                                          cc::rec::impl::sample_frames_offset + 24);
        if (!writer.is_open())
            return;
        auto const out = writer.payload();
        u64 const native_tid = 4242;
        u32 const count = 3;
        cc::memcpy(out.data() + 0, &thread_index, 4);
        cc::memcpy(out.data() + 4, &chunk_offset, 4);
        cc::memcpy(out.data() + 8, &chunk_seq, 8);
        cc::memcpy(out.data() + 16, &native_tid, 8);
        cc::memcpy(out.data() + 24, &count, 4);
        cc::memcpy(out.data() + cc::rec::impl::sample_frames_offset, frames, sizeof(frames));
        writer.commit(cc::rec::impl::sample_frames_offset + 24);
    }
    {
        auto writer = cc::rec::open_event(cc::rec::impl::thread_run_desc(), cc::rec::impl::thread_run_payload_size,
                                          cc::rec::impl::thread_run_payload_size);
        if (!writer.is_open())
            return;
        auto const out = writer.payload();
        u32 const in_scope = 1;
        u64 const native_tid = 4242;
        u64 const interval = 1000;
        u64 const ran = 250;
        cc::memcpy(out.data() + 0, &thread_index, 4);
        cc::memcpy(out.data() + 4, &in_scope, 4);
        cc::memcpy(out.data() + 8, &native_tid, 8);
        cc::memcpy(out.data() + 16, &interval, 8);
        cc::memcpy(out.data() + 24, &ran, 8);
        writer.commit(cc::rec::impl::thread_run_payload_size);
    }
}

/// Two workers, the first nested three deep around enough filler to rotate its chunk, so a preamble names open scopes.
void worker_body(int which)
{
    cc::rec::set_current_thread_record_name(which == 0 ? "reader-worker-0" : "reader-worker-1");

    CC_RECORD_SCOPE("worker");
    if (which == 0)
    {
        g_step.store(1);
        wait_for_step(2);
    }
    else
    {
        wait_for_step(1);
        g_step.store(2);
    }

    for (i32 k = 0; k < 3; ++k)
    {
        CC_RECORD_SCOPE("task");
        CC_RECORD("task_index", k);

        leaf(which, k);
    }

    if (which == 0)
        wait_for_step(3);
    else
        g_step.store(3);
}

/// The main thread's part: one value of every payload type, stats, markers, logs, pinned bytes, a relation and an async scope.
void main_body()
{
    cc::rec::set_current_thread_record_name("reader-main");
    CC_RECORD_SCOPE("fixture");

    CC_RECORD("v_bool", true);
    CC_RECORD("v_i8", i8(-5));
    CC_RECORD("v_i16", i16(-300));
    CC_RECORD("v_i32", i32(-70000));
    CC_RECORD("v_i64", i64(-1234567890123));
    CC_RECORD("v_u8", u8(200));
    CC_RECORD("v_u16", u16(65000));
    CC_RECORD("v_u32", u32(4000000000u));
    CC_RECORD("v_u64", u64(0xFEDCBA9876543210ull));
    CC_RECORD("v_f32", 1.5f);
    CC_RECORD("v_f64", -0.1);
    CC_RECORD("v_ptr", reinterpret_cast<void const*>(uintptr_t(0x1234)));
    CC_RECORD("v_literal", "a literal");
    CC_RECORD("v_text", cc::string("runtime text"));
    CC_RECORD_NAMED(cc::string("named-") + cc::format("{}", 7), i32(7));

    CC_RECORD_STAT("queue_depth", cc::rec::unit_count, 3);
    CC_RECORD_ACCUM("bytes", cc::rec::unit_bytes, 4096);
    CC_RECORD_MARK("fixture-mark");

    CC_LOG_INFO("plain message");
    CC_LOG_WARNING("took {} tries", 2);
    CC_LOG_ERROR("fixture error {}", 7);

    {
        auto payload = cc::vector<byte>();
        for (isize i = 0; i < 300; ++i)
            payload.push_back(byte((i * 7) & 0xFF));
        auto const pinned = cc::make_pinned_data(cc::move(payload)).reinterpret_as<byte const>();
        CC_RECORD_PINNED("fixture.blob", pinned);
    }

    CC_RECORD_RELATION(cc::rec::relation_parent_of, cc::rec::trace_id(0x1001), cc::rec::trace_id(0x1002),
                       cc::rec::trace_id(0x1003));

    {
        CC_RECORD_ASYNC_SCOPE_WITH_ID("fixture-async", cc::rec::trace_id(0x2002));
        CC_RECORD_MARK("in-async");
    }

    g_step.store(0);
    auto w0 = cc::native_thread::start([] { worker_body(0); });
    auto w1 = cc::native_thread::start([] { worker_body(1); });
    w0.join();
    w1.join();
}

void write_text(cc::string_view path, cc::string_view text)
{
    auto file = cc::file_write_stream_adapter::create(path);
    REQUIRE(file.has_value());
    auto stream = file.value().stream();
    REQUIRE(stream.write(cc::as_bytes(text)).has_value());
    REQUIRE(stream.flush().has_value());
}
} // namespace

REC_TEST("record/serialize - the Python reader's fixture round-trips through the C++ loader")
{
    if (!threads_available())
        SKIP("this build has no threads (SC_THREADS=OFF), and the fixture needs two workers at once");

    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener rl;
    {
        scoped_listener const reg(rl);
        main_body();
        cc::rec::flush_blocking();
    }
    auto live = rl.take();

    // Summaries too, which only a compaction writes: every stat lane folded once more, as a block of its own.
    auto const stats = live.stats_summarized(u64(1) << 62).of_kind(cc::rec::event_kind::summary);
    REQUIRE(stats.event_count() > 0);
    live.append(stats);

    auto const dir = cc::environment_variable("CC_REC_READER_FIXTURE_DIR");
    auto const keep = dir.has_value();
    auto const recording_path
        = keep ? cc::format("{}/fixture.ccrec", dir.value()) : cc::temp_file_path("cc-record-reader", ".ccrec");
    auto const manifest_path
        = keep ? cc::format("{}/fixture.manifest", dir.value()) : cc::temp_file_path("cc-record-reader", ".manifest");

    // Compressed, so both readers take the envelope as well as what is inside it.
    REQUIRE(cc::rec::save_serialized_recording(cc::rec::compress_serialized(cc::rec::serialize(live)), recording_path)
                .has_value());
    auto loaded = cc::rec::load_recording(recording_path);
    REQUIRE(loaded.has_value());

    // The C++ half on its own: what the reader is compared against has to be right in the first place.
    auto const& r = loaded.value().events();
    CHECK(r.event_count() == live.event_count());
    CHECK(r.scopes("worker").size() == 2);
    CHECK(r.scopes("task").size() == 6);
    CHECK(r.count("filler") == 4000);
    CHECK(r.first_value("v_u16").value() == 65000.0);
    CHECK(r.first_text("v_literal").value() == "a literal");

    write_text(manifest_path, manifest_of(loaded.value()));

    if (!keep)
    {
        CHECK(cc::remove_file(recording_path));
        CHECK(cc::remove_file(manifest_path));
    }
}
