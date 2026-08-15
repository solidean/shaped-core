#include "op_builder.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/container/set.hh>
#include <versioned-document/value_builder.hh>

#include <algorithm>

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

vdoc::op_builder& vdoc::op_builder::set_raw(property_path const& path, value v)
{
    for (auto const& w : _writes)
        CC_ASSERT(!(w.path == path), "the same property path was staged twice in one op");

    _writes.push_back(pending_write{.path = path, .v = cc::move(v)});
    return *this;
}

vdoc::op_builder& vdoc::op_builder::set_raw(entity_id entity, component_type_id component, property_id property, value v)
{
    return set_raw(property_path{.entity = entity, .component = component, .property = property}, cc::move(v));
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
    auto parents = _parents;
    std::sort(parents.begin(), parents.end(), op_id::by_bytes{});

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

    auto const current = graph.materialize_entities(unique_parents, touched);

    auto entries = cc::vector<assignment>();
    for (auto const& w : _writes)
    {
        auto const* const existing = current.try_get(w.path);

        // A multi-valued path always differs: it is two independent writes rather than a value, and this op is what
        // resolves it back to one.
        // Only a single writer holding these exact bytes means there is nothing to say.
        if (existing != nullptr && !existing->is_multi_valued() && existing->single() == w.v.view())
            continue;

        entries.push_back(assignment{.path = w.path, .value = w.v});
    }

    std::sort(entries.begin(), entries.end(),
              [](assignment const& a, assignment const& b) { return a.path.compare_bytes(b.path) < 0; });

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
