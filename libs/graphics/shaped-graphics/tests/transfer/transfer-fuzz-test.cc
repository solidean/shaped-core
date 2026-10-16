#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/math/random.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/fuzz/async.hh>
#include <nexus/test.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/raw_buffer.hh>
#include <shaped-graphics/types.hh>

using namespace cc::primitive_defines;


// Backend-agnostic buffer transfer: upload / download over the public sg API, fuzzed in each backend's sweep.
// See tests/context/context-test.cc for the invocable mechanism.
//
// This is an nx::fuzz API-sequence fuzz test — see libs/base/nexus/docs/fuzz-testing.md, especially "Fuzzing over external, shared state".
// The `trace` below drops its open command list in its destructor and move-assignment, so the engine's discarded replays never leak a list onto the shared context.
// The ops that wait for the GPU are async ops, awaited by the engine one step at a time, so nothing here blocks.
//
// The full search is a thorough-run cost: on vulkan nearly all of it is synchronization validation, which is the point of running it there.
// A default run narrows the seed count instead of dropping the layer, so every backend still sees random op sequences under its own checks.

ASYNC_INVOCABLE_TEST("sg - upload download fuzz test", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto test = nx::fuzz::test::create();
    test->set_inherit_home(true); // a thread-bound device's sweep homes this body, and every op must run there too

    // Fuzz state threaded through the ops.
    // It holds the context so it can DROP a still-open list explicitly when the engine discards a partial state — a command list must be submitted or dropped, never just leaked.
    // That keeps the fuzz a good citizen, relying on no auto-drop safety net, which would flood the output with warnings.
    struct trace
    {
        sg::context_handle ctx;
        std::unique_ptr<sg::command_list> cmd;
        sg::raw_buffer_handle buffer;
        cc::vector<u32> data;

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
                cmd = ctx->create_command_list();
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
                     t.buffer = ctx->persistent.create_raw_buffer(
                         4096, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
                     t.data = cc::vector<u32>::create_uninitialized(t.buffer->size_in_bytes() / sizeof(u32));

                     // initial random data fill
                     for (auto& d : t.data)
                         d = rng.next_u32();
                     auto cmd = ctx->create_command_list();
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
                     ctx->advance_epoch();
                 })
        ->execute_at_least(5);
    test->add_op("advance epoch + wait",
                 [&](trace& t) -> cc::shared_async<cc::unit>
                 {
                     t.ensure_submitted_cmd(); // no open cmdlist
                     ctx->advance_epoch();
                     (void)co_await ctx->idle_completion();
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
                     auto data = cc::vector<u32>::create_uninitialized(cnt);
                     for (auto& d : data)
                         d = rng.next_u32();

                     for (auto i = 0; i < cnt; ++i)
                         t.data[start + i] = data[i];

                     t.ensure_open_cmd();
                     t.cmd->upload.data_to_buffer(t.buffer, data, start);
                 });

    // Async-upload op: the mirror of "upload" onto the copy queue.
    // Pick a random region, model it in t.data, submit any open list first so recorded GPU order matches the model, then ctx->upload.data_to_buffer.
    // The copy actor's window-level reverse-sync deadlock this once tripped is fixed in dx12_upload_async.
    // upload-async-test.cc pins the exact shape as a deterministic regression test.
    test->add_op("async upload",
                 [&](cc::random& rng, trace& t)
                 {
                     auto v0 = rng.uniform(0, int(t.data.size() - 1));
                     auto v1 = rng.uniform(0, int(t.data.size() - 1));
                     auto start = cc::min(v0, v1);
                     auto end = cc::max(v0, v1);

                     auto cnt = end - start;
                     auto data = cc::vector<u32>::create_uninitialized(cnt);
                     for (auto& d : data)
                         d = rng.next_u32();

                     for (auto i = 0; i < cnt; ++i)
                         t.data[start + i] = data[i];

                     // Async upload streams on the copy queue, ordered after already-submitted work.
                     // Submit any open list first so the recorded GPU order matches our reference model — an open list's inline writes would race the async copy otherwise.
                     t.ensure_submitted_cmd();
                     ctx->upload.data_to_buffer<u32>(t.buffer, cc::make_pinned_data(cc::move(data)), start);
                 });

    test->add_op(
        "copy region",
        [&](cc::random& rng, trace& t)
        {
            // Pick two equal-length, non-overlapping regions.
            // Cap the length at half the buffer so two blocks always fit, then lay them out with the leftover space split into random gaps, before and between.
            // A direct construction, with no rejection sampling.
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
            t.cmd->copy.buffer_data_region<u32>(
                {.src = t.buffer, .dst = t.buffer, .count = cnt, .src_offset = src_start, .dst_offset = dst_start});
        });

    // Each check awaits a full readback round-trip, so this op dominates fuzz runtime; a bounded
    // count still verifies the model against the GPU across varied op histories without the default's 50 stalls.
    test->add_op("download + check",
                 [&](cc::random& rng, trace& t) -> cc::shared_async<cc::unit>
                 {
                     auto v0 = rng.uniform(0, int(t.data.size() - 1));
                     auto v1 = rng.uniform(0, int(t.data.size() - 1));
                     auto start = cc::min(v0, v1);
                     auto end = cc::max(v0, v1);

                     auto ref_data = cc::span<u32>(t.data).subspan({.start = start, .end = end});

                     t.ensure_open_cmd();
                     auto dl = t.cmd->download.data_from_buffer<u32>(t.buffer, start, end - start);
                     t.ensure_submitted_cmd();

                     auto const dl_data = co_await dl.data();

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
                 [&](cc::random& rng, trace& t) -> cc::shared_async<cc::unit>
                 {
                     auto v0 = rng.uniform(0, int(t.data.size() - 1));
                     auto v1 = rng.uniform(0, int(t.data.size() - 1));
                     auto start = cc::min(v0, v1);
                     auto end = cc::max(v0, v1);
                     auto cnt = end - start;

                     t.ensure_submitted_cmd();

                     auto ref = cc::vector<u32>::create_uninitialized(cnt);
                     for (auto i = 0; i < cnt; ++i)
                         ref[i] = t.data[start + i];

                     auto dl = ctx->download.data_from_buffer<u32>(t.buffer, start, cnt);
                     auto const dl_data = co_await dl.data();

                     CHECK(isize(cnt) == dl_data.size());
                     for (auto i = 0; i < cnt; ++i)
                         CHECK(ref[i] == dl_data[i]);
                 })
        ->execute_at_least(10);

    SECTION("fuzz")
    {
        if (!nx::is_thorough())
            test->cap_seed_count(24);

        CHECK(co_await test->execute_fuzz_test_async());
    }

    // The sections below are fuzz findings on metal, where one queue waited on an MTLSharedEvent another queue signals.
    // Such a wait sometimes stalls for four seconds and then lets the work run before the value is reached.
    // See metal_epoch_system::submission_wait_timeline.

    // An async download of bytes a list had just written handed back zeroes.
    SECTION("regression - async download after an inline upload")
    {
        auto n0 = test->eval_op("mk_trace");
        (void)test->eval_op("advance epoch", n0);
        co_await test->eval_op_async("async download + check", cc::random::from_state(9050458225619920008ull), n0);
    }

    // A list read the buffer as it was before an async upload it waited on.
    SECTION("regression - a list reads after an async upload")
    {
        auto n0 = test->eval_op("mk_trace");
        (void)test->eval_op("copy region", cc::random::from_state(15407372961399553199ull), n0);
        (void)test->eval_op("async upload", cc::random::from_state(3286546624697886487ull), n0);
        (void)test->eval_op("advance epoch", n0);
        co_await test->eval_op_async("download + check", cc::random::from_state(8633375846102810339ull), n0);
    }

    // A list missed the middle one of three async uploads.
    SECTION("regression - a list reads after three async uploads")
    {
        auto n0 = test->eval_op("mk_trace");
        (void)test->eval_op("async upload", cc::random::from_state(5122889261944415232ull), n0);
        (void)test->eval_op("async upload", cc::random::from_state(15008876143257452787ull), n0);
        (void)test->eval_op("async upload", cc::random::from_state(10398724905230871989ull), n0);
        (void)test->eval_op("advance epoch", n0);
        co_await test->eval_op_async("download + check", cc::random::from_state(16467782329776526886ull), n0);
    }

    // The finding that started the hunt: an inline upload, an async upload, a second inline upload, an async download.
    SECTION("regression - inline upload, async upload, inline upload, async download")
    {
        auto n0 = test->eval_op("mk_trace");
        (void)test->eval_op("submit cmd", n0);
        (void)test->eval_op("advance epoch", n0);
        (void)test->eval_op("advance epoch", n0);
        (void)test->eval_op("copy region", cc::random::from_state(3699031922499913056ull), n0);
        (void)test->eval_op("advance epoch", n0);
        (void)test->eval_op("upload", cc::random::from_state(1724587761744200817ull), n0);
        (void)test->eval_op("open cmd", n0);
        (void)test->eval_op("async upload", cc::random::from_state(12318124846294869785ull), n0);
        (void)test->eval_op("upload", cc::random::from_state(13562018016512298076ull), n0);
        co_await test->eval_op_async("async download + check", cc::random::from_state(1247115699077984541ull), n0);
    }

    // The ops start async transfers they do not all await, and a test settles what it started: the context's detached work, then the GPU.
    co_await cc::async_settled(ctx->backlog.settled());
    (void)co_await ctx->idle_completion();
}
