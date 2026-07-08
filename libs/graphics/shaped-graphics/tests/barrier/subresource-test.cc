#include <nexus/test.hh>
#include <shaped-graphics/backend/subresource.hh>

// Pure unit tests for the subresource covering-partition (designed-in for textures). The load-bearing
// property is the covering invariant: the boxes always exactly tile the whole domain, splitting to keep
// it exact and merging back when uniform.

namespace
{
// Sum of box volumes must equal the domain volume, and boxes must not overlap — i.e. an exact tiling.
bool is_exact_cover(sg::subresource_partition const& p)
{
    auto const e = p.extent();
    int covered = 0;
    // Mark each grid cell exactly once.
    for (int mip = 0; mip < e.mip_count; ++mip)
        for (int arr = 0; arr < e.array_count; ++arr)
            for (int asp = 0; asp < e.aspect_count; ++asp)
            {
                int hits = 0;
                for (auto const& box : p.boxes())
                {
                    auto const& r = box.range;
                    if (mip >= r.mip_range.start && mip < r.mip_range.end && arr >= r.array_range.start
                        && arr < r.array_range.end && asp >= r.aspect_range.start && asp < r.aspect_range.end)
                        ++hits;
                }
                if (hits != 1)
                    return false;
                ++covered;
            }
    return covered == e.total();
}
} // namespace

TEST("sg subresource - starts as a single whole-domain box")
{
    sg::subresource_partition p(sg::subresource_extent{4, 2, 1});
    CHECK(p.box_count() == 1);
    CHECK(is_exact_cover(p));
}

TEST("sg subresource - touching a sub-range splits while preserving the cover")
{
    sg::subresource_partition p(sg::subresource_extent{4, 2, 1});

    // Declare a write on mip 1..2, array slice 0 only.
    int touched = 0;
    p.for_each_in(sg::subresource_range{.mip_range = {1, 2}, .array_range = {0, 1}, .aspect_range = {0, 1}},
                  [&](sg::resource_access_state& s)
                  {
                      s.declare(sg::pipeline_stage_flags::compute, sg::access_flags::shader_write);
                      ++touched;
                  });

    CHECK(touched == 1);      // exactly one grid cell was in range
    CHECK(p.box_count() > 1); // it fragmented
    CHECK(is_exact_cover(p)); // and still tiles the domain exactly
}

TEST("sg subresource - whole-domain touch visits every box")
{
    sg::subresource_extent const e{3, 2, 2};
    sg::subresource_partition p(e);

    // Fresh: one whole-domain box -> exactly one visit (per-box, not per-cell).
    int touched = 0;
    p.for_each_in(sg::subresource_range::whole(e), [&](sg::resource_access_state&) { ++touched; });
    CHECK(touched == 1);
    CHECK(is_exact_cover(p));

    // After fragmenting, a whole-domain touch visits all boxes.
    p.for_each_in(sg::subresource_range{.mip_range = {1, 2}, .array_range = {0, 1}, .aspect_range = {0, 1}},
                  [](sg::resource_access_state&) {});
    touched = 0;
    p.for_each_in(sg::subresource_range::whole(e), [&](sg::resource_access_state&) { ++touched; });
    CHECK(touched == p.box_count());
    CHECK(is_exact_cover(p));
}

TEST("sg subresource - try_merge collapses a uniform partition")
{
    sg::subresource_extent const e{4, 2, 1};
    sg::subresource_partition p(e);

    // Fragment by touching a sub-range, but apply the SAME declare+flush to every cell so all states match.
    p.for_each_in(sg::subresource_range{.mip_range = {1, 2}, .array_range = {0, 1}, .aspect_range = {0, 1}},
                  [](sg::resource_access_state&) {});
    CHECK(p.box_count() > 1);

    // Apply an identical (no-op) transform everywhere: states are all default-equal.
    p.for_each_in(sg::subresource_range::whole(e), [](sg::resource_access_state&) {});
    p.try_merge();
    CHECK(p.box_count() == 1);
    CHECK(is_exact_cover(p));
}

TEST("sg subresource - try_merge keeps distinct states apart")
{
    sg::subresource_extent const e{2, 1, 1};
    sg::subresource_partition p(e);

    // Give mip 0 a different state than mip 1.
    p.for_each_in(sg::subresource_range{.mip_range = {0, 1}, .array_range = {0, 1}, .aspect_range = {0, 1}},
                  [](sg::resource_access_state& s)
                  { s.declare(sg::pipeline_stage_flags::compute, sg::access_flags::shader_write); });

    p.try_merge();
    CHECK(p.box_count() == 2); // differing states must not merge
    CHECK(is_exact_cover(p));
}

TEST("sg subresource - for_each_box_in passes each covered box's range")
{
    sg::subresource_extent const e{4, 1, 1};
    sg::subresource_partition p(e);

    // Touch mip 1..3; the callback must see box ranges contained in [1,3), covering it exactly.
    int covered_mips = 0;
    p.for_each_box_in(sg::subresource_range{.mip_range = {1, 3}, .array_range = {0, 1}, .aspect_range = {0, 1}},
                      [&](sg::subresource_range const& box_range, sg::resource_access_state&)
                      {
                          CHECK(box_range.mip_range.start >= 1);
                          CHECK(box_range.mip_range.end <= 3);
                          covered_mips += box_range.mip_range.end - box_range.mip_range.start;
                      });
    CHECK(covered_mips == 2); // the two mips in [1,3) were each visited exactly once
    CHECK(is_exact_cover(p));
}
