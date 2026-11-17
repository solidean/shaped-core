#pragma once

#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/driver/describe.hh>

namespace sgl::driver::impl
{
/// What `sgl describe` says of one binding: its members, their slots and facts, and its block.
/// `s` must be a binding that passed `emit::impl::validate_binding`.
[[nodiscard]] described_binding describe_binding(check::checked_module const& m, check::symbol const& s);

/// What `sgl describe` says of one file-scope sampler, which must have checked.
[[nodiscard]] described_file_sampler describe_file_sampler(check::checked_module const& m, check::symbol_id id);

/// The file-scope samplers the code of `legal` reaches, in index order.
[[nodiscard]] cc::vector<check::symbol_id> file_samplers_of(check::flat_entry_point const& legal);

/// What one entry point's signature hands a pipeline beside its bindings, which a host that builds that pipeline in
/// its own code reads (EMIT-268, EMIT-269, EMIT-270).
/// `describe` and `compile_to_text` both report it from here, so the two cannot disagree.
struct entry_interface
{
    /// A vertex entry point's `@vertex struct`, `m.name` for another module's; empty for every other entry point.
    cc::string vertex_input;
    /// A ray-tracing stage's payload, or a callable's parameter: its name, its structural hash as 32 hex digits and its
    /// bytes; empty and 0 for any other stage and for a stage without one.
    cc::string payload;
    cc::string payload_shape;
    i32 payload_size = 0;
    /// A procedural closest or any hit's attributes, or what an intersection reports: its name and its structural hash
    /// as 32 hex digits; empty for a triangle hit and for every other stage.
    cc::string attributes;
    cc::string attributes_shape;
    /// The hit attributes' bytes: 8 for a triangle's barycentrics, the attributes' own for a procedural hit and for an
    /// intersection, 0 for every other stage.
    i32 attribute_size = 0;
};

/// The interface of `e`, an entry point as the check pass flattened it, legalized or not.
[[nodiscard]] entry_interface entry_interface_of(check::checked_module const& m, check::flat_entry_point const& e);
} // namespace sgl::driver::impl
