#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/metal/metal_barrier.hh>

// Barrier translation is pure logic with no device in it, so these run on any machine rather than only where a Metal 4
// GPU exists — which on a platform with no software adapter is the difference between covered and skipped.
// None of them creates a context, and none of them can SKIP.

namespace mtl = sg::backend::metal;

TEST("sg metal - every sg pipeline stage maps onto MTLStages")
{
    CHECK(mtl::stages_of(sg::pipeline_stage_flag::vertex) == MTL::StageVertex);
    CHECK(mtl::stages_of(sg::pipeline_stage_flag::fragment) == MTL::StageFragment);
    CHECK(mtl::stages_of(sg::pipeline_stage_flag::compute) == MTL::StageDispatch);
    CHECK(mtl::stages_of(sg::pipeline_stage_flag::copy) == MTL::StageBlit);
    CHECK(mtl::stages_of(sg::pipeline_stage_flag::accel_build) == MTL::StageAccelerationStructure);

    // Metal folds several of sg's stages together, and the folds are the part worth pinning — each is a place the
    // mapping is onto rather than one-to-one, so a reader cannot infer it from the enum.
    CHECK(mtl::stages_of(sg::pipeline_stage_flag::render_target) == MTL::StageFragment);
    CHECK(mtl::stages_of(sg::pipeline_stage_flag::depth_stencil_target) == MTL::StageFragment);
    CHECK(mtl::stages_of(sg::pipeline_stage_flag::raytracing) == MTL::StageDispatch);
    CHECK(mtl::stages_of(sg::pipeline_stage_flag::draw_indirect) == (MTL::StageVertex | MTL::StageDispatch));
}

TEST("sg metal - an empty stage set is every stage, not none")
{
    // "Not known" rather than "no stage": a barrier against nothing orders nothing, so the conservative answer is the
    // only safe one.
    // The same convention sg::binding's empty visibility follows.
    CHECK(mtl::stages_of(sg::pipeline_stage_flags{}) == MTL::StageAll);
}

TEST("sg metal - a stage set folds into one mask")
{
    auto const set = sg::pipeline_stage_flag::vertex | sg::pipeline_stage_flag::fragment | sg::pipeline_stage_flag::copy;
    CHECK(mtl::stages_of(set) == (MTL::StageVertex | MTL::StageFragment | MTL::StageBlit));
}

TEST("sg metal - only a write needs caches flushed")
{
    // A read-after-read orders execution and makes nothing newly visible, which is the cheaper barrier — and on a
    // tiler the difference is a real flush rather than bookkeeping.
    CHECK(mtl::visibility_for(sg::access_flag::shader_read, sg::access_flag::shader_read) == MTL4::VisibilityOptionNone);

    CHECK(mtl::visibility_for(sg::access_flag::shader_write, sg::access_flag::shader_read)
          == MTL4::VisibilityOptionDevice);
    CHECK(mtl::visibility_for(sg::access_flag::copy_write, sg::access_flag::vertex_read) == MTL4::VisibilityOptionDevice);

    // A render-target write is ROP-ordered rather than unordered, so sg does not call it an unordered write — but its
    // results still have to become visible to a later reader.
    CHECK(mtl::visibility_for(sg::access_flag::color_write, sg::access_flag::shader_read) == MTL4::VisibilityOptionDevice);
    CHECK(mtl::visibility_for(sg::access_flag::depth_write, sg::access_flag::shader_read) == MTL4::VisibilityOptionDevice);
}

TEST("sg metal - a barrier that is not needed translates to nothing")
{
    CHECK(!mtl::translate_barrier({.needed = false}).needed);
}

TEST("sg metal - a pure layout transition translates to nothing")
{
    // The sharpest divergence from the other two backends: a Metal texture has no layout at all, so a barrier whose
    // only content is a transition has nothing left to emit once the layouts are dropped.
    // Pinned so the drop reads as deliberate rather than as an omission.
    auto const layout_only = sg::access_barrier{
        .needed = true,
        .src_layout = sg::texture_layout::copy_dst,
        .dst_layout = sg::texture_layout::shader_readonly,
    };
    CHECK(!mtl::translate_barrier(layout_only).needed);
}

TEST("sg metal - a write-to-read barrier carries both stage masks")
{
    auto const barrier = sg::access_barrier{
        .needed = true,
        .src_stages = sg::pipeline_stage_flag::copy,
        .dst_stages = sg::pipeline_stage_flag::compute,
        .src_access = sg::access_flag::copy_write,
        .dst_access = sg::access_flag::shader_read,
    };

    auto const translated = mtl::translate_barrier(barrier);
    CHECK(translated.needed);
    CHECK(translated.after_stages == MTL::StageBlit);
    CHECK(translated.before_stages == MTL::StageDispatch);
    CHECK(translated.visibility == MTL4::VisibilityOptionDevice);
}

TEST("sg metal - an encoder barrier is clamped to what a compute encoder accepts")
{
    // `barrierAfterEncoderStages` refuses any stage the encoder cannot encode work for, and with validation armed it
    // aborts rather than warning — which is how this was found.
    CHECK(mtl::clamp_to_compute_encoder(MTL::StageBlit) == MTL::StageBlit);
    CHECK(mtl::clamp_to_compute_encoder(MTL::StageDispatch | MTL::StageBlit) == (MTL::StageDispatch | MTL::StageBlit));

    // A stage this encoder cannot name is dropped rather than passed through.
    CHECK(mtl::clamp_to_compute_encoder(MTL::StageBlit | MTL::StageVertex) == MTL::StageBlit);

    // Clamping to nothing yields the whole set rather than an empty mask, which would order nothing at all.
    CHECK(mtl::clamp_to_compute_encoder(MTL::StageVertex) == mtl::k_compute_encoder_stages);
    CHECK(mtl::clamp_to_compute_encoder(MTL::StageAll) == mtl::k_compute_encoder_stages);
}
