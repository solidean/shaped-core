#include "metal_raytracing_shader_table.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>
#include <shaped-graphics/backends/metal/metal_raytracing_pipeline.hh>

namespace sg::backend::metal
{
namespace
{
/// The four resource ids a kernel reads out of the reserved group, in `[[id(n)]]` order.
/// Kept next to the header sentence that states the assignment, because the two must not drift.
constexpr isize k_intersection_slot = 0;
constexpr isize k_miss_slot = 1;
constexpr isize k_closest_hit_slot = 2;
constexpr isize k_callable_slot = 3;
constexpr isize k_table_slot_count = 4;
} // namespace

void release_raygen_binding(metal_residency_set& residency, metal_raytracing_shader_table::raygen_binding const& r)
{
    // **All five residency entries, because all five were added.**
    // A resource left in the set is one the queue keeps resident for the context's whole life, and nothing reports it
    // — the same silence that makes a missing entry read zeroes rather than fail.
    //
    // The pipeline state is borrowed from the pipeline, which outlives this table, so it is not released here.
    auto const give_back = [&](auto* object)
    {
        if (object == nullptr)
            return;
        residency.remove(object);
        object->release();
    };

    give_back(r.intersection);
    give_back(r.miss);
    give_back(r.closest_hit);
    give_back(r.callable);
    give_back(r.arguments);
}

metal_raytracing_shader_table::~metal_raytracing_shader_table()
{
    auto raygens = cc::move(_raygens);
    _raygens.clear();

    // **Deferred here and immediate on the unwind path**, which is why the deferral is at the call site rather than
    // inside the helper: a built table may still be in flight, and a partial build has never been submitted.
    _ctx.epochs().defer(
        [raygens = cc::move(raygens), &residency = _ctx.residency()]
        {
            for (auto const& r : raygens)
                release_raygen_binding(residency, r);
        });
}

cc::result<sg::raytracing_shader_table_handle> metal_context::create_metal_raytracing_shader_table(
    sg::raytracing_shader_table_description const& desc,
    sg::lifetime_scope)
{
    if (desc.pipeline == nullptr)
        return cc::error("raytracing_shader_table: the description names no pipeline");
    if (desc.raygen.empty())
        return cc::error("raytracing_shader_table: at least one raygen entry is required");

    auto const& pipeline = static_cast<metal_raytracing_pipeline const&>(*desc.pipeline);
    auto const scope = autorelease_scope();

    auto raygens = cc::vector<metal_raytracing_shader_table::raygen_binding>();

    // **Every failure past this point owns objects nobody else will free.**
    // Each iteration mints four function tables and an argument buffer, all owned references rather than autoreleased,
    // and registers five residency entries — and the destructor that would release them never runs, because the table
    // object is never constructed.
    // Released immediately rather than deferred: nothing here has been submitted.
    auto const unwind = [&](metal_raytracing_shader_table::raygen_binding const& partial)
    {
        release_raygen_binding(_residency, partial);
        for (auto const& done : raygens)
            release_raygen_binding(_residency, done);
    };

    for (auto const raygen_handle : desc.raygen)
    {
        auto binding = metal_raytracing_shader_table::raygen_binding{};
        binding.state = pipeline.raygen_state(raygen_handle);
        if (binding.state == nullptr)
        {
            unwind(binding);
            return cc::error("raytracing_shader_table: a raygen handle is out of the pipeline's range");
        }

        // Every table is minted from THIS raygen's pipeline state: a function handle is per state, so a table built
        // from one raygen's state cannot be bound to another's.
        auto const visible_table = [&](isize count) -> MTL::VisibleFunctionTable*
        {
            auto* const descriptor = MTL::VisibleFunctionTableDescriptor::alloc()->init();
            descriptor->setFunctionCount(NS::UInteger(count));
            auto* const table = binding.state->newVisibleFunctionTable(descriptor);
            descriptor->release();
            return table;
        };

        binding.miss = visible_table(desc.miss.size());
        binding.closest_hit = visible_table(desc.hit.size());
        binding.callable = visible_table(desc.callable.size());

        auto* const intersection_descriptor = MTL::IntersectionFunctionTableDescriptor::alloc()->init();
        intersection_descriptor->setFunctionCount(NS::UInteger(desc.hit.size()));
        binding.intersection = binding.state->newIntersectionFunctionTable(intersection_descriptor);
        intersection_descriptor->release();

        if (binding.miss == nullptr || binding.closest_hit == nullptr || binding.callable == nullptr
            || binding.intersection == nullptr)
        {
            unwind(binding);
            return cc::error("raytracing_shader_table: the metal device refused a function table");
        }

        // A handle that comes back null is an un-linked or misspelled function, and it is the one mistake that is
        // caught for free at table build — so it is an error here rather than a wrong call at trace time.
        // A group with no shader of this kind leaves the slot empty, which is a valid table entry.
        auto const set_visible = [&](MTL::VisibleFunctionTable* table, isize index, MTL4::BinaryFunction* function) -> bool
        {
            if (function == nullptr)
                return true;
            auto* const handle = binding.state->functionHandle(function);
            if (handle == nullptr)
                return false;
            table->setFunction(handle, NS::UInteger(index));
            return true;
        };

        for (auto i = isize(0); i < desc.miss.size(); ++i)
        {
            auto const handle = u32(desc.miss[i]);
            if (handle >= u32(pipeline.miss_functions().size()))
            {
                unwind(binding);
                return cc::error("raytracing_shader_table: a miss handle is out of the pipeline's range");
            }
            if (!set_visible(binding.miss, i, pipeline.miss_functions()[isize(handle)]))
            {
                unwind(binding);
                return cc::error(cc::format("raytracing_shader_table: the miss function at index {} did not link into "
                                            "this raygen's pipeline",
                                            i));
            }
        }

        for (auto i = isize(0); i < desc.callable.size(); ++i)
        {
            auto const handle = u32(desc.callable[i]);
            if (handle >= u32(pipeline.callable_functions().size()))
            {
                unwind(binding);
                return cc::error("raytracing_shader_table: a callable handle is out of the pipeline's range");
            }
            if (!set_visible(binding.callable, i, pipeline.callable_functions()[isize(handle)]))
            {
                unwind(binding);
                return cc::error(cc::format("raytracing_shader_table: the callable function at index {} did not link "
                                            "into this raygen's pipeline",
                                            i));
            }
        }

        for (auto i = isize(0); i < desc.hit.size(); ++i)
        {
            auto const handle = u32(desc.hit[i]);
            if (handle >= u32(pipeline.hit_groups().size()))
            {
                unwind(binding);
                return cc::error("raytracing_shader_table: a hit handle is out of the pipeline's range");
            }
            auto const& group = pipeline.hit_groups()[isize(handle)];

            if (!set_visible(binding.closest_hit, i, group.closest_hit))
            {
                unwind(binding);
                return cc::error(cc::format("raytracing_shader_table: the closest-hit function of hit group {} did "
                                            "not link into this raygen's pipeline",
                                            i));
            }

            // **Metal runs ONE function during traversal, and a procedural group's is its intersection shader.**
            // DXR runs an any-hit after it; MSL has nowhere to put that, so a procedural group carrying one is
            // refused rather than silently losing it — the intersection function does that work here.
            if (group.is_procedural && group.any_hit != nullptr)
            {
                unwind(binding);
                return cc::error(cc::format("raytracing_shader_table: hit group {} is procedural and carries an "
                                            "any-hit shader, which metal has no traversal slot for — fold the "
                                            "any-hit's work into the intersection function",
                                            i));
            }

            // What runs during traversal: an intersection shader for a procedural group, otherwise the any-hit if
            // there is one, and otherwise Metal's own triangle intersection.
            auto* const traversal = group.is_procedural ? group.intersection : group.any_hit;
            if (traversal != nullptr)
            {
                auto* const handle_for = binding.state->functionHandle(traversal);
                if (handle_for == nullptr)
                {
                    unwind(binding);
                    return cc::error(cc::format("raytracing_shader_table: the traversal function of hit group {} did "
                                                "not link into this raygen's pipeline",
                                                i));
                }
                binding.intersection->setFunction(handle_for, NS::UInteger(i));
            }
            else
            {
                // **The signature must match the table's own MSL declaration**, which `None` does not.
                // A table that holds any triangle group is declared `intersection_function_table<instancing,
                // triangle_data>` — see the readme, which states that as the requirement on a kernel — so the opaque
                // default is asked for with exactly those two.
                binding.intersection->setOpaqueTriangleIntersectionFunction(
                    MTL::IntersectionFunctionSignatureInstancing | MTL::IntersectionFunctionSignatureTriangleData,
                    NS::UInteger(i));
            }
        }

        // The reserved group's argument buffer: four resource ids, in the [[id(n)]] order the header states.
        auto* const arguments
            = _device->newBuffer(size_t(k_table_slot_count) * sizeof(u64), MTL::ResourceStorageModeShared);
        if (arguments == nullptr)
        {
            unwind(binding);
            return cc::error("raytracing_shader_table: the metal device refused an argument buffer");
        }

        auto* const slots = static_cast<u64*>(arguments->contents());
        slots[k_intersection_slot] = binding.intersection->gpuResourceID()._impl;
        slots[k_miss_slot] = binding.miss->gpuResourceID()._impl;
        slots[k_closest_hit_slot] = binding.closest_hit->gpuResourceID()._impl;
        slots[k_callable_slot] = binding.callable->gpuResourceID()._impl;
        arguments->setLabel(ns_string("sg raytracing shader table"));

        binding.arguments = arguments;
        _residency.add(arguments);
        _residency.add(binding.intersection);
        _residency.add(binding.miss);
        _residency.add(binding.closest_hit);
        _residency.add(binding.callable);

        raygens.push_back(binding);
    }

    return sg::raytracing_shader_table_handle(
        std::make_shared<metal_raytracing_shader_table const>(*this, desc.pipeline, cc::move(raygens)));
}
} // namespace sg::backend::metal
