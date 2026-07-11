#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <nexus/fuzz/test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/pixel_format.hh>
#include <shaped-graphics/raw_texture.hh>
#include <shaped-graphics/texture_region.hh>
#include <shaped-graphics/types.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>


// Backend-agnostic 2D-texture transfer fuzz: upload / download of random sub-rectangles over the public sg
// API, run against every available backend (see tests/context/context-test.cc for the invocable/alias
// mechanism). The texture-transfer path (bytes_to_texture / bytes_from_texture, inline + async) landed
// recently and — unlike buffers — has to un-pad the backend's row pitch on readback and place sub-regions by
// footprint; this drives varied op histories against it. The buffer sibling lives in transfer-buffer-fuzz-test.cc.
//
// This is an nx::fuzz API-sequence fuzz test — see libs/base/nexus/docs/fuzz-testing.md, especially
// "Fuzzing over external, shared state": the `trace` below drops its open command list in its destructor and
// move-assignment, so the engine's discarded replays never leak a list onto the shared context.
//
// NOTE: there is no public texture→texture copy op yet (cmd.copy is buffer-only — command_list.copy.hh says
// texture copies "land here later"), so this test has no analogue of the buffer fuzz's "copy region" op.
// Growing command_list_copy_scope with a texture region-copy (+ backend impl) would let it join.

INVOCABLE_TEST("sg - texture 2d transfer fuzz test", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    // A plain, uncompressed "normal" format: one texel = one addressable block = 4 bytes (block_extent 1).
    constexpr sg::pixel_format fmt = sg::pixel_format::rgba8_unorm;
    constexpr int tex_w = 64;
    constexpr int tex_h = 64;
    int const bpt = sg::format_block_size(fmt); // bytes per texel

    auto test = nx::fuzz::test::create();

    // Byte offset of texel (x, y) in a tightly-packed, row-major W×H image.
    auto texel_off = [=](int x, int y) { return cc::isize(y) * tex_w * bpt + cc::isize(x) * bpt; };

    // Fills a byte span with random content, 4 bytes per rng draw (region rows are multiples of bpt=4).
    auto fill_random = [](cc::random& rng, cc::span<cc::byte> dst)
    {
        cc::isize i = 0;
        for (; i + 4 <= dst.size(); i += 4)
        {
            auto const v = rng.next_u32();
            dst[i + 0] = cc::byte(v);
            dst[i + 1] = cc::byte(v >> 8);
            dst[i + 2] = cc::byte(v >> 16);
            dst[i + 3] = cc::byte(v >> 24);
        }
        for (; i < dst.size(); ++i)
            dst[i] = cc::byte(rng.next_u32());
    };

    // A random sub-rectangle in texels: two corners in [0, extent] per axis, so width/height run [0, extent]
    // (an empty rect — w or h == 0 — is a valid no-op the transfer APIs accept).
    struct rect
    {
        int x, y, w, h;
    };
    auto rand_rect = [](cc::random& rng)
    {
        auto const x0 = rng.uniform(0, tex_w);
        auto const x1 = rng.uniform(0, tex_w);
        auto const y0 = rng.uniform(0, tex_h);
        auto const y1 = rng.uniform(0, tex_h);
        return rect{cc::min(x0, x1), cc::min(y0, y1), cc::max(x0, x1) - cc::min(x0, x1),
                    cc::max(y0, y1) - cc::min(y0, y1)};
    };
    auto region_of = [](rect const& r)
    { return sg::texture_region{.offset = tg::pos3i(r.x, r.y, 0), .size = tg::vec3i(r.w, r.h, 1)}; };

    // Fuzz state threaded through the ops. Holds the context so it can DROP a still-open list explicitly when
    // the engine discards a partial state — a command list must be submitted or dropped, never just let leak
    // (see command_list's lifecycle contract). This keeps the fuzz a good citizen: no reliance on the
    // destructor's auto-drop safety net (which would flood the output with warnings).
    struct trace
    {
        sg::context_handle ctx;
        std::unique_ptr<sg::command_list> cmd;
        sg::raw_texture_handle tex;
        cc::vector<cc::byte> data; // tightly-packed, row-major reference model of the whole subresource

        trace() = default;
        trace(trace&&) = default;
        trace& operator=(trace&& o) noexcept
        {
            drop_open_cmd(); // never leak our own open list when overwritten
            ctx = cc::move(o.ctx);
            cmd = cc::move(o.cmd);
            tex = cc::move(o.tex);
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

                     sg::texture_description desc;
                     desc.format = fmt;
                     desc.dimension = sg::texture_dimension::d2;
                     desc.width = tex_w;
                     desc.height = tex_h;
                     desc.usage = sg::texture_usage::copy_src | sg::texture_usage::copy_dst;
                     t.tex = ctx->persistent.create_raw_texture(desc);

                     // initial random data fill of the whole subresource
                     t.data = cc::vector<cc::byte>::create_uninitialized(cc::isize(tex_w) * tex_h * bpt);
                     fill_random(rng, t.data);
                     auto cmd = ctx->create_command_list();
                     cmd->upload.bytes_to_texture(t.tex, cc::span<cc::byte const>(t.data)); // no region = whole subresource
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

    // Inline upload of a random sub-rect: fill fresh random pixels, mirror them into the model row by row, then
    // record the copy on an open list. The host source is tightly packed (row bytes = w × bpt).
    test->add_op("upload",
                 [&](cc::random& rng, trace& t)
                 {
                     auto const r = rand_rect(rng);
                     auto const row_bytes = cc::isize(r.w) * bpt;

                     auto pixels = cc::vector<cc::byte>::create_uninitialized(row_bytes * r.h);
                     fill_random(rng, pixels);
                     for (int j = 0; j < r.h; ++j)
                         for (cc::isize b = 0; b < row_bytes; ++b)
                             t.data[texel_off(r.x, r.y + j) + b] = pixels[cc::isize(j) * row_bytes + b];

                     t.ensure_open_cmd();
                     t.cmd->upload.bytes_to_texture(t.tex, pixels, {}, region_of(r));
                 });

    // Async-upload op: mirror of "upload" onto the copy queue. Fill a random rect, model it, submit any open
    // list first so recorded GPU order matches the model, then ctx->upload.bytes_to_texture with a pin.
    //
    // DISABLED (#if 0): the async texture ops are gated off because they trip a real dx12 cross-queue texture
    // layout bug this fuzz uncovered — an inline copy op leaves the texture in COPY_DEST/COPY_SOURCE, but the
    // async copy queue requires COMMON, so interleaving inline + async floods the D3D12 debug layer with
    // barrier-layout errors (data still round-trips). See the standalone repro + full write-up in
    // transfer-tex2d-test.cc. Re-enable both async ops here once that bug is fixed (planned follow-up).
#if 0
    test->add_op("async upload",
                 [&](cc::random& rng, trace& t)
                 {
                     auto const r = rand_rect(rng);
                     auto const row_bytes = cc::isize(r.w) * bpt;

                     auto pixels = cc::vector<cc::byte>::create_uninitialized(row_bytes * r.h);
                     fill_random(rng, pixels);
                     for (int j = 0; j < r.h; ++j)
                         for (cc::isize b = 0; b < row_bytes; ++b)
                             t.data[texel_off(r.x, r.y + j) + b] = pixels[cc::isize(j) * row_bytes + b];

                     // Async upload streams on the copy queue, ordered after already-submitted work. Submit any
                     // open list first so the recorded GPU order matches our reference model — an open list's
                     // inline writes would race the async copy otherwise.
                     t.ensure_submitted_cmd();
                     ctx->upload.bytes_to_texture(t.tex, cc::make_pinned_data(cc::move(pixels)), {}, region_of(r));
                 });
#endif

    // Each check blocks on a full wait_for readback round-trip, so this op dominates fuzz runtime; a bounded
    // count still verifies the model against the GPU across varied op histories without the default's 50 stalls.
    test->add_op("download + check",
                 [&](cc::random& rng, trace& t)
                 {
                     auto const r = rand_rect(rng);
                     auto const row_bytes = cc::isize(r.w) * bpt;

                     t.ensure_open_cmd();
                     auto dl = t.cmd->download.bytes_from_texture(t.tex, {}, region_of(r));
                     t.ensure_submitted_cmd();

                     auto dl_data = ctx->wait_for(dl).value();

                     CHECK(dl_data.size() == row_bytes * r.h);
                     bool matches = true;
                     for (int j = 0; j < r.h; ++j)
                         for (cc::isize b = 0; b < row_bytes; ++b)
                             if (dl_data[cc::isize(j) * row_bytes + b] != t.data[texel_off(r.x, r.y + j) + b])
                                 matches = false;
                     CHECK(matches);
                 })
        ->execute_at_least(10);

    // Async-download + check: the copy-queue mirror of "download + check" (ctx->download vs cmd->download).
    // Exercises the reverse cross-queue sync (a later direct-queue write waits on the in-flight read) and the
    // forward wait vs a pending async upload — paths a purely-inline download never touches. Submit any open
    // list first so the read is ordered after the recorded writes it must observe; the read auto-waits on the
    // last submitted writer (and on any pending async upload), so the issue-time snapshot of t.data is correct.
    //
    // DISABLED (#if 0): same dx12 cross-queue texture layout bug as "async upload" above — see transfer-tex2d-test.cc.
#if 0
    test->add_op("async download + check",
                 [&](cc::random& rng, trace& t)
                 {
                     auto const r = rand_rect(rng);
                     auto const row_bytes = cc::isize(r.w) * bpt;

                     t.ensure_submitted_cmd();

                     auto ref = cc::vector<cc::byte>::create_uninitialized(row_bytes * r.h);
                     for (int j = 0; j < r.h; ++j)
                         for (cc::isize b = 0; b < row_bytes; ++b)
                             ref[cc::isize(j) * row_bytes + b] = t.data[texel_off(r.x, r.y + j) + b];

                     auto dl = ctx->download.bytes_from_texture(t.tex, {}, region_of(r));
                     auto dl_data = ctx->wait_for(dl).value();

                     CHECK(dl_data.size() == ref.size());
                     bool matches = true;
                     for (cc::isize i = 0; i < ref.size(); ++i)
                         if (dl_data[i] != ref[i])
                             matches = false;
                     CHECK(matches);
                 })
        ->execute_at_least(10);
#endif

    SECTION("fuzz")
    {
        CHECK(test->execute_fuzz_test());
    }
}
