#include "op_builder.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/container/set.hh>
#include <versioned-document/value_builder.hh>

using namespace cc::primitive_defines;

vdoc::op_builder& vdoc::op_builder::set_parents(cc::span<op_id const> parents)
{
    _parents = cc::vector<op_id>::create_copy_of(parents);
    return *this;
}

vdoc::op_builder& vdoc::op_builder::set_metadata(value metadata)
{
    _metadata = cc::move(metadata);
    return *this;
}

void vdoc::op_builder::impl_assert_unstaged(property_path const& path)
{
    // Free in release, where CC_ASSERT is stripped — but a debug editor staging a bulk edit pays it, and a linear
    // scan per write is quadratic in the size of the op.
    // Past a handful of writes the set is cheaper than the scan and asks the same question.
    if constexpr (CC_ASSERT_ENABLED)
    {
        if (_writes.size() < 16)
        {
            for (auto const& w : _writes)
                CC_ASSERT(!(w.path == path), "the same property path was staged twice in one op");
        }
        else
        {
            if (_staged_paths.size() != _writes.size())
            {
                _staged_paths.clear();
                for (auto const& w : _writes)
                    (void)_staged_paths.insert(w.path);
            }

            CC_ASSERT(_staged_paths.insert(path), "the same property path was staged twice in one op");
        }
    }
}

vdoc::op_builder& vdoc::op_builder::set_raw(property_path const& path, value v)
{
    impl_assert_unstaged(path);
    _writes.push_back(pending_write{.path = path, .v = cc::move(v)});
    return *this;
}

vdoc::op_builder& vdoc::op_builder::set_raw(entity_id entity, component_type_id component, property_id property, value v)
{
    return set_raw(property_path{.entity = entity, .component = component, .property = property}, cc::move(v));
}

vdoc::op_builder& vdoc::op_builder::abstain(property_path const& path)
{
    // Staging a write and a withdrawal of one path in one op is the same caller bug as staging two writes.
    impl_assert_unstaged(path);
    _writes.push_back(pending_write{.path = path, .kind = assignment_kind::abstain});
    return *this;
}

vdoc::op_builder& vdoc::op_builder::abstain(entity_id entity, component_type_id component, property_id property)
{
    return abstain(property_path{.entity = entity, .component = component, .property = property});
}

vdoc::op_builder& vdoc::op_builder::set_alive(entity_id entity, component_type_id component, bool alive)
{
    return set_raw(entity, component, reserved::alive(), value::of(alive));
}

vdoc::op_builder& vdoc::op_builder::set_entity_alive(entity_id entity, bool alive)
{
    return set_alive(entity, reserved::entity(), alive);
}

vdoc::op vdoc::op_builder::build(op_graph const& graph) const
{
    return impl_build(graph, nullptr);
}

vdoc::op vdoc::op_builder::build(op_graph const& graph, snapshot_cache& cache) const
{
    return impl_build(graph, &cache);
}

vdoc::op vdoc::op_builder::impl_build(op_graph const& graph, snapshot_cache* cache) const
{
    // On impl_build rather than build, so the cacheless overload is covered by the same span.
    CC_RECORD_SCOPE("vdoc.op_builder.build");

    auto parents = _parents;
    cc::sort(parents, op_id::by_bytes{});

    // Deduplicate in place: a caller naming one parent twice means one edge, and the hash must see the canonical form.
    auto unique_parents = cc::vector<op_id>();
    for (auto const& p : parents)
        if (unique_parents.empty() || !(unique_parents.back() == p))
            unique_parents.push_back(p);

    // Only the touched entities need materializing, which is what keeps an edit cheap on a large document.
    auto touched = cc::vector<entity_id>();
    auto seen = cc::set<entity_id>();
    for (auto const& w : _writes)
        if (seen.insert(w.path.entity))
            touched.push_back(w.path.entity);

    auto const current = cache == nullptr ? graph.materialize_entities(unique_parents, touched)
                                          : graph.materialize_entities(unique_parents, touched, *cache);

    auto entries = cc::vector<assignment>();
    for (auto const& w : _writes)
    {
        auto const* const existing = current.try_get(w.path);

        if (w.kind == assignment_kind::abstain)
        {
            // The mirror of the write case: withdrawing a contribution to a path nothing writes says nothing at all.
            // That is what lets an override layer re-stage the same withdrawal every frame for free.
            if (existing == nullptr)
                continue;

            entries.push_back(assignment{.path = w.path, .kind = assignment_kind::abstain});
            continue;
        }

        // A multi-valued path always differs: it is two independent writes rather than a value, and this op is what
        // resolves it back to one.
        // Only a single writer holding these exact bytes means there is nothing to say.
        if (existing != nullptr && !existing->is_multi_valued() && existing->single() == w.v.view())
            continue;

        entries.push_back(assignment{.path = w.path, .value = w.v});
    }

    cc::sort(entries, [](assignment const& a, assignment const& b) { return a.path.compare_bytes(b.path) < 0; });

    auto const metadata_bytes = cc::vector<byte>::create_copy_of(_metadata.bytes());
    auto assignment_bytes = encode_assignments(entries);
    auto const id = compute_op_id(unique_parents, metadata_bytes, assignment_bytes);

    auto out
        = op{.id = id,
             .parents = cc::move(unique_parents),
             .payload = op_payload{.metadata_bytes = metadata_bytes, .assignment_bytes = cc::move(assignment_bytes)}};

    CC_ASSERT(verify_op(out) == op_verification::verified, "op_builder stamped an id its own bytes do not hash to");
    return out;
}
