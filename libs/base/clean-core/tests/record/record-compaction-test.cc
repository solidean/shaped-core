#include <clean-core/record/domain.hh>
#include <clean-core/record/impl/block_builder.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/sampling.hh>
#include <clean-core/record/scope.hh>
#include <clean-core/record/serialize.hh>
#include <clean-core/record/stat.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>

// The compaction transforms on recordings built by hand, so every timestamp, and so every threshold, is exact.

using namespace cc::primitive_defines;

namespace
{
constexpr cc::rec::desc work_begin = {
    .kind = cc::rec::event_kind::scope_begin,
    .name = "work",
    .dom = &cc::rec::g_system_domain,
    .fields = cc::rec::impl::scope_fields,
    .field_count = 1,
    .fixed_payload_size = 4,
};
constexpr cc::rec::desc work_end = {
    .kind = cc::rec::event_kind::scope_end,
    .name = "work",
    .dom = &cc::rec::g_system_domain,
    .fields = cc::rec::impl::scope_fields,
    .field_count = 1,
    .fixed_payload_size = 4,
};
constexpr cc::rec::desc inner_begin = {
    .kind = cc::rec::event_kind::scope_begin,
    .name = "inner",
    .dom = &cc::rec::g_system_domain,
    .fields = cc::rec::impl::scope_fields,
    .field_count = 1,
    .fixed_payload_size = 4,
};
constexpr cc::rec::desc inner_end = {
    .kind = cc::rec::event_kind::scope_end,
    .name = "inner",
    .dom = &cc::rec::g_system_domain,
    .fields = cc::rec::impl::scope_fields,
    .field_count = 1,
    .fixed_payload_size = 4,
};
constexpr cc::rec::desc bytes_stat = {
    .kind = cc::rec::event_kind::stat_accumulate,
    .name = "bytes",
    .dom = &cc::rec::g_system_domain,
    .fields = cc::rec::impl::stat_fields,
    .field_count = 1,
    .fixed_payload_size = 8,
};
constexpr cc::rec::desc depth_stat = {
    .kind = cc::rec::event_kind::stat_snapshot,
    .name = "depth",
    .dom = &cc::rec::g_system_domain,
    .fields = cc::rec::impl::stat_fields,
    .field_count = 1,
    .fixed_payload_size = 8,
};

/// One thread's stream, written event by event at the times a test names.
struct stream
{
    cc::rec::recorded_block metadata = {.thread_index = 1, .thread_name = "main"};
    cc::rec::impl::block_builder builder = cc::rec::impl::block_builder(metadata);
    u32 depth = 0;

    stream& begin(cc::rec::desc const& d, u64 cycles)
    {
        add(d, cycles, cc::span<byte const>(reinterpret_cast<byte const*>(&depth), 4));
        ++depth;
        return *this;
    }
    stream& end(cc::rec::desc const& d, u64 cycles)
    {
        --depth;
        add(d, cycles, cc::span<byte const>(reinterpret_cast<byte const*>(&depth), 4));
        return *this;
    }
    stream& stat(cc::rec::desc const& d, u64 cycles, f64 value)
    {
        add(d, cycles, cc::span<byte const>(reinterpret_cast<byte const*>(&value), 8));
        return *this;
    }

    /// Ends the block being written, as a sealed chunk would, so what follows goes into the stream's next one.
    stream& next_block()
    {
        blocks.push_back(builder.take());
        ++metadata.chunk_seq;
        return *this;
    }

    [[nodiscard]] cc::rec::recording take()
    {
        auto r = cc::rec::recording();
        for (auto const& b : blocks)
            r.append_block(b);
        r.append_block(builder.take());
        return r;
    }

    cc::vector<cc::rec::recorded_block> blocks;

private:
    void add(cc::rec::desc const& d, u64 cycles, cc::span<byte const> payload)
    {
        builder.add({.desc = &d, .cycles = cycles, .payload = payload});
    }
};

/// Every event as `name:kind`, in stream order, which is what most checks here compare against.
[[nodiscard]] cc::string listing(cc::rec::recording const& r)
{
    auto out = cc::string();
    r.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            auto const kind = e.kind() == cc::rec::event_kind::summary       ? "sum"
                            : e.kind() == cc::rec::event_kind::scope_begin   ? "begin"
                            : e.kind() == cc::rec::event_kind::scope_end     ? "end"
                            : e.kind() == cc::rec::event_kind::stat_snapshot ? "snapshot"
                                                                             : "stat";
            out += cc::format("{}{}:{}", out.empty() ? "" : " ", e.name(), kind);
        });
    return out;
}

[[nodiscard]] cc::vector<cc::rec::summary> summaries(cc::rec::recording const& r, cc::string_view name)
{
    auto out = cc::vector<cc::rec::summary>();
    r.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() == cc::rec::event_kind::summary && e.name() == name)
                out.push_back(e.as_summary().value());
        });
    return out;
}
} // namespace

TEST("record/compaction - a short scope becomes a summary of its duration, and what it enclosed stays")
{
    auto s = stream();
    s.begin(work_begin, 0).begin(inner_begin, 10).stat(bytes_stat, 12, 5).end(inner_end, 20).end(work_end, 100);
    s.begin(work_begin, 200).end(work_end, 2000);
    auto const r = s.take();

    auto const collapsed = r.scopes_collapsed(500);
    CHECK(listing(collapsed) == "bytes:stat inner:sum work:sum work:begin work:end");

    auto const work = summaries(collapsed, "work");
    REQUIRE(work.size() == 1);
    CHECK(work[0].count == 1);
    CHECK(work[0].sum == 100);
    CHECK(work[0].begin_cycles == 0);
    CHECK(work[0].end_cycles == 100);
    CHECK(summaries(collapsed, "inner")[0].sum == 10);

    // Nothing is short enough at a threshold below every duration.
    CHECK(listing(r.scopes_collapsed(5)) == listing(r));
}

TEST("record/compaction - a scope this recording saw only one end of is kept")
{
    auto s = stream();
    s.depth = 1; // a scope the stream started inside
    s.end(work_end, 5).begin(inner_begin, 10);
    auto const r = s.take();
    CHECK(listing(r.scopes_collapsed(1000)) == "work:end inner:begin");
}

TEST("record/compaction - a short scope across a block boundary is kept, since the later block's preamble names it")
{
    auto s = stream();
    s.begin(work_begin, 0).next_block().end(work_end, 10).begin(inner_begin, 20).end(inner_end, 30);
    auto const r = s.take();
    CHECK(listing(r.scopes_collapsed(1000)) == "work:begin work:end inner:sum");
}

TEST("record/compaction - a stat lane folds into one summary per window")
{
    auto s = stream();
    s.stat(bytes_stat, 1, 1).stat(bytes_stat, 2, 2).stat(bytes_stat, 3, 3).stat(bytes_stat, 15, 10);
    s.stat(depth_stat, 4, 7).stat(depth_stat, 5, 4);
    auto const r = s.take();

    auto const summarized = r.stats_summarized(10);
    auto const bytes = summaries(summarized, "bytes");
    REQUIRE(bytes.size() == 2);
    CHECK(bytes[0].count == 3);
    CHECK(bytes[0].sum == 6);
    CHECK(bytes[0].min == 1);
    CHECK(bytes[0].max == 3);
    CHECK(bytes[1].count == 1);
    CHECK(bytes[1].sum == 10);

    auto const depth = summaries(summarized, "depth");
    REQUIRE(depth.size() == 1);
    CHECK(depth[0].last == 4);
    CHECK(depth[0].max == 7);

    // Aligned windows, so a second pass at the same window finds nothing left to fold.
    CHECK(listing(summarized.stats_summarized(10)) == listing(summarized));
}

TEST("record/compaction - a lane never spans two owners, so each summary belongs to one")
{
    static constexpr cc::rec::field ambient_fields[] = {
        {.name = "trace", .type = cc::rec::type_code::u64_, .offset = 0, .size = 8},
        {.name = "owner", .type = cc::rec::type_code::u64_, .offset = 8, .size = 8},
    };
    static constexpr cc::rec::desc ambient = {
        .kind = cc::rec::event_kind::ambient_changed,
        .name = "async.ambient",
        .dom = &cc::rec::g_system_domain,
        .fields = ambient_fields,
        .field_count = 2,
        .fixed_payload_size = 16,
    };
    auto s = stream();
    auto const owned_by = [&](u64 owner, u64 cycles)
    {
        u64 const payload[2] = {0, owner};
        s.builder.add({.desc = &ambient,
                       .cycles = cycles,
                       .payload = cc::span<byte const>(reinterpret_cast<byte const*>(payload), 16)});
    };
    owned_by(7, 0);
    s.stat(bytes_stat, 1, 1).stat(bytes_stat, 2, 1);
    owned_by(7, 3); // the same owner again closes nothing
    s.stat(bytes_stat, 4, 1);
    owned_by(8, 5);
    s.stat(bytes_stat, 6, 1);
    auto const r = s.take();

    // One window covers it all, and still the lane splits where the owner changes, before the delta that changes it.
    CHECK(listing(r.stats_summarized(1'000'000))
          == "async.ambient:stat async.ambient:stat bytes:sum async.ambient:stat bytes:sum");
    auto const bytes = summaries(r.stats_summarized(1'000'000), "bytes");
    REQUIRE(bytes.size() == 2);
    CHECK(bytes[0].count == 3);
    CHECK(bytes[1].count == 1);
}

TEST("record/compaction - a lane ends where the scope around it does")
{
    auto s = stream();
    s.begin(work_begin, 0).stat(bytes_stat, 1, 1).stat(bytes_stat, 2, 1).end(work_end, 3000);
    s.stat(bytes_stat, 3001, 1);
    auto const r = s.take();

    // One window covers it all, and still the stats inside the scope stay inside it.
    CHECK(listing(r.stats_summarized(1'000'000)) == "work:begin bytes:sum work:end bytes:sum");
}

TEST("record/compaction - collapsed scopes summarize with each other")
{
    auto s = stream();
    for (auto i = u64(0); i < 5; ++i)
        s.begin(inner_begin, i * 10).end(inner_end, i * 10 + 2);
    auto const r = s.take();

    auto const folded = r.scopes_collapsed(100).stats_summarized(1000);
    auto const inner = summaries(folded, "inner");
    REQUIRE(inner.size() == 1);
    CHECK(inner[0].count == 5);
    CHECK(inner[0].sum == 10);
    CHECK(inner[0].begin_cycles == 0);
    CHECK(inner[0].end_cycles == 42);
}

TEST("record/compaction - the rounds grow until the recording is small enough")
{
    auto s = stream();
    for (auto i = u64(0); i < 200; ++i)
        s.begin(inner_begin, i * 1000).stat(bytes_stat, i * 1000 + 1, 1).end(inner_end, i * 1000 + 3);
    auto const r = s.take();

    auto report = cc::rec::compaction_report{};
    auto const small = r.compacted(
        {.min_scope_secs = 1e-6, .stat_window_secs = 1e-6, .growth = 10, .target_bytes = 1024, .cycles_per_second = 1e6},
        &report);
    CHECK(report.reached_target);
    CHECK(report.rounds > 1);
    CHECK(report.bytes_after == small.total_bytes());
    CHECK(report.bytes_after <= 1024);
    CHECK(report.bytes_before == r.total_bytes());

    auto total = u64(0);
    for (auto const& sum : summaries(small, "inner"))
        total += sum.count;
    CHECK(total == 200);
}

TEST("record/compaction - without sampling, what the code recorded about itself is left")
{
    static constexpr cc::rec::desc tick_begin = {
        .kind = cc::rec::event_kind::scope_begin,
        .name = cc::rec::impl::sample_tick_scope_name,
        .dom = &cc::rec::g_system_domain,
        .fields = cc::rec::impl::scope_fields,
        .field_count = 1,
        .fixed_payload_size = 4,
    };
    static constexpr cc::rec::desc tick_end = {
        .kind = cc::rec::event_kind::scope_end,
        .name = cc::rec::impl::sample_tick_scope_name,
        .dom = &cc::rec::g_system_domain,
        .fields = cc::rec::impl::scope_fields,
        .field_count = 1,
        .fixed_payload_size = 4,
    };

    auto s = stream();
    byte run[cc::rec::impl::thread_run_payload_size] = {};
    s.begin(work_begin, 0).begin(tick_begin, 1).end(tick_end, 2);
    s.builder.add({.desc = &cc::rec::impl::thread_run_desc(), .cycles = 3, .payload = run});
    s.stat(bytes_stat, 4, 1).end(work_end, 5);
    CHECK(listing(s.take().without_sampling()) == "work:begin bytes:stat work:end");
}

TEST("record/compaction - thinned sampling keeps one reading per sampled thread per window")
{
    auto s = stream();
    auto const reading = [&](u32 thread, u64 cycles)
    {
        byte run[cc::rec::impl::thread_run_payload_size] = {};
        cc::memcpy(run, &thread, sizeof(thread));
        s.builder.add({.desc = &cc::rec::impl::thread_run_desc(), .cycles = cycles, .payload = run});
    };
    reading(1, 0);
    reading(1, 3);
    reading(2, 4);
    reading(1, 12);
    reading(1, 15);
    reading(1, 25);
    s.stat(bytes_stat, 26, 1);
    auto const r = s.take();

    auto const count = [](cc::rec::recording const& rec, u32 thread)
    {
        auto n = 0;
        rec.for_each_event(
            [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
            {
                if (e.desc == &cc::rec::impl::thread_run_desc() && e.field_as_u64("thread_index").value_or(0) == thread)
                    ++n;
            });
        return n;
    };
    auto const thinned = r.with_sampling_thinned(10, 1);
    CHECK(count(thinned, 1) == 3);
    CHECK(count(thinned, 2) == 1);
    CHECK(summaries(thinned, "bytes").empty()); // the rest is untouched
    CHECK(listing(thinned).ends_with("bytes:stat"));

    // Aligned windows, so a second pass at the same interval keeps the same events.
    CHECK(listing(thinned.with_sampling_thinned(10, 1)) == listing(thinned));
}

TEST("record/compaction - thinned sampling drops a stack its thread repeats, with that window's reading")
{
    auto s = stream();
    auto const sample = [&](u64 cycles, u64 frame)
    {
        byte payload[cc::rec::impl::sample_frames_offset + 8] = {};
        auto const thread = u32(1);
        auto const count = u32(1);
        cc::memcpy(payload, &thread, sizeof(thread));
        cc::memcpy(payload + 24, &count, sizeof(count));
        cc::memcpy(payload + cc::rec::impl::sample_frames_offset, &frame, sizeof(frame));
        s.builder.add({.desc = &cc::rec::impl::sample_desc(), .cycles = cycles, .payload = payload});
    };
    auto const reading = [&](u64 cycles)
    {
        byte run[cc::rec::impl::thread_run_payload_size] = {};
        auto const thread = u32(1);
        cc::memcpy(run, &thread, sizeof(thread));
        s.builder.add({.desc = &cc::rec::impl::thread_run_desc(), .cycles = cycles, .payload = run});
    };
    // A thread parked in one wait for three windows, then somewhere else.
    for (auto const [cycles, frame] : {cc::pair<u64, u64>{0, 0x10}, {10, 0x10}, {20, 0x10}, {30, 0x20}})
    {
        reading(cycles);
        sample(cycles + 1, frame);
    }
    auto const thinned = s.take().with_sampling_thinned(10, 1);

    auto samples = cc::vector<u64>();
    auto readings = 0;
    thinned.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.desc == &cc::rec::impl::sample_desc())
                samples.push_back(e.cycles);
            else if (e.desc == &cc::rec::impl::thread_run_desc())
                ++readings;
        });
    REQUIRE(samples.size() == 2);
    CHECK(samples[0] == 1);  // the first time it was seen there
    CHECK(samples[1] == 31); // and once it moved
    CHECK(readings == 2);
}

TEST("record/compaction - a summary survives a file, compressed or not, with its site")
{
    auto s = stream();
    s.begin(inner_begin, 0).end(inner_end, 7).stat(bytes_stat, 8, 3);
    auto const folded = s.take().scopes_collapsed(100);

    auto const bytes = cc::rec::serialize(folded);
    auto const packed = cc::rec::compress_serialized(bytes);
    CHECK(packed.size() < bytes.size());

    for (auto const* const file : {&bytes, &packed})
    {
        auto const loaded = cc::rec::deserialize(*file);
        REQUIRE(loaded.has_value());
        auto const& back = loaded.value().events();
        CHECK(listing(back) == listing(folded));
        auto const inner = summaries(back, "inner");
        REQUIRE(inner.size() == 1);
        CHECK(inner[0].sum == 7);
        CHECK(inner[0].end_cycles == 7);
    }

    // An envelope claiming another size than its frame holds is refused, in either direction.
    for (auto const delta : {u64(1), ~u64(0)})
    {
        auto bad = packed;
        auto size = u64(0);
        cc::memcpy(&size, bad.data() + 8, sizeof(size));
        size += delta; // +1, or -1 by wrapping
        cc::memcpy(bad.data() + 8, &size, sizeof(size));
        CHECK(!cc::rec::deserialize(bad).has_value());
    }
}
