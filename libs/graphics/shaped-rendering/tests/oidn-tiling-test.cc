#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-rendering/impl/oidn_network.hh>

using namespace cc::primitive_defines;

// How the OIDN network cuts an image into tiles, which is integer arithmetic and needs no device.
//
// The expectations are the minima worked out by hand: for each tiled axis, the tile extent at most the cap whose tile
// count times extent is least, with the default overlap of 80.

namespace
{
struct plan_case
{
    tg::vec2i image = tg::vec2i(0, 0);
    int cap = 0;
    tg::vec2i extent = tg::vec2i(0, 0);
    tg::vec2i step = tg::vec2i(0, 0);
    tg::vec2i counts = tg::vec2i(0, 0);
    int overlap = 0;
};

void check_plan(plan_case const& c)
{
    auto const plan = sr::impl::plan_tiles(c.image, c.cap, sr::impl::oidn_network::k_tile_overlap);
    auto const label = cc::format("{}x{} at a cap of {}", c.image[0], c.image[1], c.cap);
    CHECK(plan.extent == c.extent).context(cc::format("{}: extent {}x{}", label, plan.extent[0], plan.extent[1]));
    CHECK(plan.step == c.step).context(cc::format("{}: step {}x{}", label, plan.step[0], plan.step[1]));
    CHECK(plan.counts == c.counts).context(cc::format("{}: counts {}x{}", label, plan.counts[0], plan.counts[1]));
    CHECK(plan.overlap == c.overlap).context(cc::format("{}: overlap {}", label, plan.overlap));

    // Every tiled axis covers the image, and its tile never exceeds the cap.
    for (auto a = 0; a < 2; ++a)
    {
        CHECK(plan.counts[a] * plan.step[a] >= c.image[a]).context(label);
        if (plan.counts[a] > 1)
            CHECK(plan.extent[a] <= c.cap).context(label);
    }
}
} // namespace

TEST("sr - OIDN tile planning: an image that fits is one tile with no overlap")
{
    // Rounded up to the 16-pixel grid four pools need, and nothing else.
    check_plan({.image = tg::vec2i(1, 1),
                .cap = 512,
                .extent = tg::vec2i(16, 16),
                .step = tg::vec2i(16, 16),
                .counts = tg::vec2i(1, 1),
                .overlap = 0});
    check_plan({.image = tg::vec2i(15, 17),
                .cap = 512,
                .extent = tg::vec2i(16, 32),
                .step = tg::vec2i(16, 32),
                .counts = tg::vec2i(1, 1),
                .overlap = 0});
}

TEST("sr - OIDN tile planning: each axis is tiled on its own")
{
    // One pixel past the cap tiles that axis; the other, under the cap, stays one span.
    // 432 is the smallest tile whose 272-pixel interior covers 513 in two tiles.
    check_plan({.image = tg::vec2i(513, 100),
                .cap = 512,
                .extent = tg::vec2i(432, 112),
                .step = tg::vec2i(272, 112),
                .counts = tg::vec2i(2, 1),
                .overlap = 80});

    // An axis exactly at the cap fits, whatever the other one needs.
    check_plan({.image = tg::vec2i(512, 600),
                .cap = 512,
                .extent = tg::vec2i(512, 464),
                .step = tg::vec2i(512, 304),
                .counts = tg::vec2i(1, 2),
                .overlap = 80});
}

TEST("sr - OIDN tile planning: 1080p takes the tile that computes least at every cap")
{
    // 512 chooses 480 on x rather than 512: both take 6 tiles to cover 1920, and 6 of 480 compute 2880 columns
    // where 6 of 512 would compute 3072.
    check_plan({.image = tg::vec2i(1920, 1080),
                .cap = 384,
                .extent = tg::vec2i(384, 384),
                .step = tg::vec2i(224, 224),
                .counts = tg::vec2i(9, 5),
                .overlap = 80});
    check_plan({.image = tg::vec2i(1920, 1080),
                .cap = 448,
                .extent = tg::vec2i(448, 432),
                .step = tg::vec2i(288, 272),
                .counts = tg::vec2i(7, 4),
                .overlap = 80});
    check_plan({.image = tg::vec2i(1920, 1080),
                .cap = 512,
                .extent = tg::vec2i(480, 432),
                .step = tg::vec2i(320, 272),
                .counts = tg::vec2i(6, 4),
                .overlap = 80});
    check_plan({.image = tg::vec2i(1920, 1080),
                .cap = 576,
                .extent = tg::vec2i(544, 528),
                .step = tg::vec2i(384, 368),
                .counts = tg::vec2i(5, 3),
                .overlap = 80});
    check_plan({.image = tg::vec2i(1920, 1080),
                .cap = 640,
                .extent = tg::vec2i(640, 528),
                .step = tg::vec2i(480, 368),
                .counts = tg::vec2i(4, 3),
                .overlap = 80});
    check_plan({.image = tg::vec2i(1920, 1080),
                .cap = 704,
                .extent = tg::vec2i(640, 704),
                .step = tg::vec2i(480, 544),
                .counts = tg::vec2i(4, 2),
                .overlap = 80});
    check_plan({.image = tg::vec2i(1920, 1080),
                .cap = 768,
                .extent = tg::vec2i(640, 704),
                .step = tg::vec2i(480, 544),
                .counts = tg::vec2i(4, 2),
                .overlap = 80});
}
