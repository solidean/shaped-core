#include <clean-core/container/span.hh>
#include <clean-core/math/random.hh>
#include <nexus/fuzz/test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/buffer.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/types.hh>


// Backend-agnostic inline buffer transfer: upload / download over the public sg API, run against every
// available backend (see tests/context/context-test.cc for the invocable/alias mechanism).

INVOCABLE_TEST("sg - upload download fuzz test", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto test = nx::fuzz::test::create();

    struct trace
    {
        std::unique_ptr<sg::command_list> cmd;
        sg::buffer_handle buffer;
        cc::vector<cc::u32> data;

        void ensure_open_cmd(sg::context_handle const& ctx)
        {
            if (!cmd)
                cmd = ctx->create_command_list().value();
        }

        void ensure_submitted_cmd(sg::context_handle const& ctx)
        {
            if (cmd)
                ctx->submit_command_list(cc::move(cmd));
            cmd = nullptr;
        }
    };

    test->add_op(
            "mk_trace",
            [&]
            {
                cc::random rng;

                trace t;
                t.buffer
                    = ctx->persistent.create_buffer(4096, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst).value();
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

    test->add_op("open cmd", [&](trace& t) { t.ensure_open_cmd(ctx); });
    test->add_op("submit cmd", [&](trace& t) { t.ensure_submitted_cmd(ctx); });
    test->add_op("advance epoch", [&]() { ctx->advance_epoch(cc::nullopt); });
    test->add_op("advance epoch + wait", [&]() { ctx->advance_epoch_and_wait_for_idle(); });

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

                     t.ensure_open_cmd(ctx);
                     t.cmd->upload.data_to_buffer(t.buffer, data, start * sizeof(cc::u32));
                 });

    test->add_op("download + check",
                 [&](cc::random& rng, trace& t)
                 {
                     auto v0 = rng.uniform(0, int(t.data.size() - 1));
                     auto v1 = rng.uniform(0, int(t.data.size() - 1));
                     auto start = cc::min(v0, v1);
                     auto end = cc::max(v0, v1);

                     auto ref_data = cc::span<cc::u32>(t.data).subspan({.start = start, .end = end});

                     t.ensure_open_cmd(ctx);
                     auto dl = t.cmd->download.data_from_buffer<cc::u32>(t.buffer, start * sizeof(cc::u32), end - start);
                     t.ensure_submitted_cmd(ctx);

                     ctx->advance_epoch_and_wait_for_idle();
                     REQUIRE(dl.is_ready());
                     auto dl_data = dl.wait_get_data().value();

                     CHECK(ref_data.size() == dl_data.size());
                     for (auto i = 0; i < end - start; ++i)
                         CHECK(ref_data[i] == dl_data[i]);
                 });

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
        cc::vector<sg::buffer_handle> buffers;
        cc::vector<sg::command_list> cmd_lists;
    };

    test->add_value("trace", trace{});
    test->add_op("add1", [](int a) { return a + 1; });

    SECTION("fuzz")
    {
        CHECK(test->execute_fuzz_test());
    }
}*/
