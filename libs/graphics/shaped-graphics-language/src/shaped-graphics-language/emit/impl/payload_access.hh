#pragma once

#include <shaped-graphics-language/emit/impl/plan.hh>

namespace sgl::emit::impl
{
/// Which shaders read and write one field of a ray payload (EMIT-137): the caller, then closesthit, miss and anyhit.
struct payload_access
{
    bool reads[4] = {};
    bool writes[4] = {};

    /// The field's mask in DXIL's `!dx.dxrPayloadAnnotations`: four bits a stage in the order above, 1 read and 2 written.
    /// A field no stage reads, or none writes, is still the caller's, as HLSL's text states it too.
    [[nodiscard]] u32 dxil_mask() const;
};

/// Field `member` of payload `type`, as every entry point of the module touches it.
/// Every shader of one SGL file states the same answer, which is what one pipeline's shaders must agree on.
[[nodiscard]] payload_access payload_access_of(plan const& p, check::type_id type, cc::string_view member);

/// Whether `type` is a ray payload of the entry point: the one it is handed, or one it traces with.
[[nodiscard]] bool is_payload(plan const& p, check::type_id type);
} // namespace sgl::emit::impl
