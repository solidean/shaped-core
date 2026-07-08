#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/math/random.hh>
#include <nexus/fuzz/test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/raw_buffer.hh>
#include <shaped-graphics/types.hh>


// Backend-agnostic inline buffer transfer: upload / download over the public sg API, run against every
// available backend (see tests/context/context-test.cc for the invocable/alias mechanism).
//
// This is an nx::fuzz API-sequence fuzz test — see libs/base/nexus/docs/fuzz-testing.md, especially
// "Fuzzing over external, shared state": the `trace` below drops its open command list in its destructor
// and move-assignment, so the engine's discarded replays never leak a list onto the shared context.
//
// TODO: investigate fuzz runtime. Per-op execution counts are capped below to keep it reasonable, and each
// download + epoch-wait genuinely stalls on the GPU — but it still feels slower than the op mix should cost.
// Profile where the time actually goes (GPU round-trips vs. per-op host overhead vs. the fuzz engine itself)
// before raising the caps back up.

INVOCABLE_TEST("sg - upload download fuzz test", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto test = nx::fuzz::test::create();

    // Fuzz state threaded through the ops. Holds the context so it can DROP a still-open list explicitly
    // when the engine discards a partial state — a command list must be submitted or dropped, never just
    // let leak (see command_list's lifecycle contract). This keeps the fuzz a good citizen: no reliance
    // on the destructor's auto-drop safety net (which would flood the output with warnings).
    struct trace
    {
        sg::context_handle ctx;
        std::unique_ptr<sg::command_list> cmd;
        sg::raw_buffer_handle buffer;
        cc::vector<cc::u32> data;

        trace() = default;
        trace(trace&&) = default;
        trace& operator=(trace&& o) noexcept
        {
            drop_open_cmd(); // never leak our own open list when overwritten
            ctx = cc::move(o.ctx);
            cmd = cc::move(o.cmd);
            buffer = cc::move(o.buffer);
            data = cc::move(o.data);
            return *this;
        }
        ~trace() { drop_open_cmd(); }

        void drop_open_cmd()
        {
            if (cmd)
                ctx->drop_command_list(cc::move(cmd));
        }

        void ensure_open_cmd()
        {
            if (!cmd)
                cmd = ctx->create_command_list().value();
        }

        void ensure_submitted_cmd()
        {
            if (cmd)
                ctx->submit_command_list(cc::move(cmd));
            cmd = nullptr;
        }
    };

    test->add_op("mk_trace",
                 [&]
                 {
                     cc::random rng;

                     trace t;
                     t.ctx = ctx;
                     t.buffer = ctx->persistent
                                    .create_raw_buffer(4096, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst)
                                    .value();
                     t.data = cc::vector<cc::u32>::create_uninitialized(t.buffer->size_in_bytes() / sizeof(cc::u32));

                     // initial random data fill
                     for (auto& d : t.data)
                         d = rng.next_u32();
                     auto cmd = ctx->create_command_list().value();
                     cmd->upload.data_to_buffer(t.buffer, t.data);
                     ctx->submit_command_list(cc::move(cmd));

                     return t;
                 })
        ->execute_once();

    // Explicit open/submit just exercises small/empty command lists; every other op auto-opens as needed, so
    // a few reps suffice (the default ~50 adds nothing but runtime).
    test->add_op("open cmd", [&](trace& t) { t.ensure_open_cmd(); })->execute_at_least(3);
    test->add_op("submit cmd", [&](trace& t) { t.ensure_submitted_cmd(); })->execute_at_least(3);
    // Epoch advances (especially the wait-for-idle variant) drain the GPU, so they dominate the fuzz runtime;
    // a handful covers the epoch-transition interactions without the default's 50 GPU stalls.
    test->add_op("advance epoch",
                 [&](trace& t)
                 {
                     t.ensure_submitted_cmd(); // no open cmdlist
                     ctx->advance_epoch(cc::nullopt);
                 })
        ->execute_at_least(5);
    test->add_op("advance epoch + wait",
                 [&](trace& t)
                 {
                     t.ensure_submitted_cmd(); // no open cmdlist
                     ctx->advance_epoch_and_wait_for_idle();
                 })
        ->execute_at_least(5);

    test->add_op("upload",
                 [&](cc::random& rng, trace& t)
                 {
                     auto v0 = rng.uniform(0, int(t.data.size() - 1));
                     auto v1 = rng.uniform(0, int(t.data.size() - 1));
                     auto start = cc::min(v0, v1);
                     auto end = cc::max(v0, v1);

                     auto cnt = end - start;
                     auto data = cc::vector<cc::u32>::create_uninitialized(cnt);
                     for (auto& d : data)
                         d = rng.next_u32();

                     for (auto i = 0; i < cnt; ++i)
                         t.data[start + i] = data[i];

                     t.ensure_open_cmd();
                     t.cmd->upload.data_to_buffer(t.buffer, data, start);
                 });

    // Async-upload op: mirror of "upload" onto the copy queue. Pick a random region, model it in t.data,
    // submit any open list first so recorded GPU order matches the model, then ctx->upload.data_to_buffer.
    // (The copy actor's window-level reverse-sync deadlock this once tripped is fixed in dx12_upload_async;
    // upload-async-test.cc pins the exact shape as a deterministic regression test.)
    test->add_op("async upload",
                 [&](cc::random& rng, trace& t)
                 {
                     auto v0 = rng.uniform(0, int(t.data.size() - 1));
                     auto v1 = rng.uniform(0, int(t.data.size() - 1));
                     auto start = cc::min(v0, v1);
                     auto end = cc::max(v0, v1);

                     auto cnt = end - start;
                     auto data = cc::vector<cc::u32>::create_uninitialized(cnt);
                     for (auto& d : data)
                         d = rng.next_u32();

                     for (auto i = 0; i < cnt; ++i)
                         t.data[start + i] = data[i];

                     // Async upload streams on the copy queue, ordered after already-submitted work. Submit
                     // any open list first so the recorded GPU order matches our reference model — an open
                     // list's inline writes would race the async copy otherwise.
                     t.ensure_submitted_cmd();
                     ctx->upload.data_to_buffer<cc::u32>(t.buffer, cc::make_pinned_data(cc::move(data)), start);
                 });

    test->add_op(
        "copy region",
        [&](cc::random& rng, trace& t)
        {
            // Pick two equal-length, non-overlapping regions. Cap the length at half the buffer so
            // two blocks always fit, then lay them out with the leftover space split into random
            // gaps (before / between) — a direct construction, no rejection sampling.
            auto n = int(t.data.size());
            auto cnt = rng.uniform(1, n / 2);
            auto slack = n - 2 * cnt;             // free space to distribute around the two blocks
            auto g0 = rng.uniform(0, slack);      // gap before the first block
            auto g1 = rng.uniform(0, slack - g0); // gap between the two blocks
            auto lo = g0;                         // first block start
            auto hi = g0 + cnt + g1;              // second block start (>= lo + cnt, so disjoint)

            // Randomly pick which block is source vs destination.
            auto src_start = lo;
            auto dst_start = hi;
            if (rng.uniform(0, 1) == 1)
            {
                src_start = hi;
                dst_start = lo;
            }

            // Non-overlapping, so a straight element copy models the GPU copy exactly.
            for (auto i = 0; i < cnt; ++i)
                t.data[dst_start + i] = t.data[src_start + i];

            t.ensure_open_cmd();
            t.cmd->copy.buffer_data_region<cc::u32>(
                {.src = t.buffer, .dst = t.buffer, .count = cnt, .src_offset = src_start, .dst_offset = dst_start});
        });

    // Each check blocks on a full wait_for readback round-trip, so this op dominates fuzz runtime; a bounded
    // count still verifies the model against the GPU across varied op histories without the default's 50 stalls.
    test->add_op("download + check",
                 [&](cc::random& rng, trace& t)
                 {
                     auto v0 = rng.uniform(0, int(t.data.size() - 1));
                     auto v1 = rng.uniform(0, int(t.data.size() - 1));
                     auto start = cc::min(v0, v1);
                     auto end = cc::max(v0, v1);

                     auto ref_data = cc::span<cc::u32>(t.data).subspan({.start = start, .end = end});

                     t.ensure_open_cmd();
                     auto dl = t.cmd->download.data_from_buffer<cc::u32>(t.buffer, start, end - start);
                     t.ensure_submitted_cmd();

                     auto dl_data = ctx->wait_for(dl).value();

                     CHECK(ref_data.size() == dl_data.size());
                     for (auto i = 0; i < end - start; ++i)
                         CHECK(ref_data[i] == dl_data[i]);
                 })
        ->execute_at_least(10);

    // Async-download + check: the copy-queue mirror of "download + check" (ctx->download vs cmd->download).
    // Exercises the reverse cross-queue sync (a later direct-queue write waits on the in-flight read) and the
    // forward wait vs a pending async upload — paths a purely-inline download never touches, and both clean
    // GPU cross-queue waits now that async upload and download own separate copy queues (see
    // libs/graphics/shaped-graphics/docs/concepts/download.async.md; a shared queue FIFO-deadlocked here).
    // Submit any open list first so the read is
    // ordered after the recorded writes it must observe; the read auto-waits on the last submitted writer
    // (and on any pending async upload), so the issue-time snapshot of t.data is the correct reference.
    test->add_op("async download + check",
                 [&](cc::random& rng, trace& t)
                 {
                     auto v0 = rng.uniform(0, int(t.data.size() - 1));
                     auto v1 = rng.uniform(0, int(t.data.size() - 1));
                     auto start = cc::min(v0, v1);
                     auto end = cc::max(v0, v1);
                     auto cnt = end - start;

                     t.ensure_submitted_cmd();

                     auto ref = cc::vector<cc::u32>::create_uninitialized(cnt);
                     for (auto i = 0; i < cnt; ++i)
                         ref[i] = t.data[start + i];

                     auto dl = ctx->download.data_from_buffer<cc::u32>(t.buffer, start, cnt);
                     auto dl_data = ctx->wait_for(dl).value();

                     CHECK(cc::isize(cnt) == dl_data.size());
                     for (auto i = 0; i < cnt; ++i)
                         CHECK(ref[i] == dl_data[i]);
                 })
        ->execute_at_least(10);

    SECTION("fuzz")
    {
        CHECK(test->execute_fuzz_test());
    }
}

/*
INVOCABLE_TEST("sg - upload download fuzz test", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    
    auto test = nx::fuzz::test::create();

    enum class buffer_idx : uint32_t;
    enum class cmd_list_idx : uint32_t;

    struct trace {
        cc::vector<sg::raw_buffer_handle> buffers;
        cc::vector<sg::command_list> cmd_lists;
    };

    test->add_value("trace", trace{});
    test->add_op("add1", [](int a) { return a + 1; });

    SECTION("fuzz")
    {
        CHECK(test->execute_fuzz_test());
    }
}*/
