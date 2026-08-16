#include "recovery.hh"

using namespace cc::primitive_defines;

namespace
{
[[nodiscard]] bool same_parents(cc::span<vdoc::op_id const> a, cc::span<vdoc::op_id const> b)
{
    if (a.size() != b.size())
        return false;

    for (isize i = 0; i < a.size(); ++i)
        if (!(a[i] == b[i]))
            return false;

    return true;
}
} // namespace

cc::result<cc::vector<vdoc::op>, vdoc::integration_rejection> vdoc::try_verify_batch(op_graph const& graph,
                                                                                     cc::span<received_op const> batch)
{
    auto verified = cc::vector<op>();
    verified.reserve(batch.size());

    for (auto const& received : batch)
    {
        auto decoded = try_decode_op(received.id, received.parents, received.metadata_bytes, received.assignment_bytes);
        if (decoded.has_error())
            return cc::error(integration_rejection{.op = received.id,
                                                   .reason = integration_error::malformed,
                                                   .decode_error = decoded.error()});

        // The one integrity claim content addressing cannot make for us.
        // A held skeleton's parents came out of storage without a hash over them, so this is the only moment a
        // corrupted parent list is detectable — and it can only ever fire on a skeleton.
        auto const* const held = graph.find(received.id);
        if (held != nullptr && !same_parents(held->parents, decoded.value().parents))
            return cc::error(integration_rejection{.op = received.id, .reason = integration_error::parents_disagree});

        verified.push_back(cc::move(decoded.value()));
    }

    return verified;
}

vdoc::integration_result vdoc::apply_verified_batch(op_graph& graph, cc::vector<op> verified)
{
    auto result = integration_result();

    for (auto& o : verified)
    {
        auto const id = o.id;
        auto const* const existing = graph.find(id);

        if (existing == nullptr)
        {
            ++result.ops_added;
            (void)graph.add(cc::move(o));
        }
        else if (existing->is_skeleton())
        {
            ++result.skeletons_filled;
            (void)graph.fill_payload(id, cc::move(o.payload.value()));
        }
        else
        {
            ++result.already_present;
        }
    }

    return result;
}

cc::result<vdoc::integration_result, vdoc::integration_rejection> vdoc::integrate(op_graph& graph,
                                                                                  cc::span<received_op const> batch)
{
    // Verifying the whole batch before applying any of it is what makes a rejection leave the replica exactly as it
    // was, rather than holding the prefix that happened to verify before the tampered op.
    auto verified = try_verify_batch(graph, batch);
    if (verified.has_error())
        return cc::error(verified.error());

    return apply_verified_batch(graph, cc::move(verified.value()));
}
