#pragma once

#include <clean-core/error/optional.hh>
#include <versioned-document/parse_report.hh>
#include <versioned-document/raw_document.hh>

/// How a raw document is interpreted, and the one place the multi-value rules are implemented.
///
/// `property_reader` is what a component's parse is handed, and `parse_policy` is what it defers the hard cases to.
/// Splitting them this way is what keeps "how to interpret" out of the parser entirely.
///
/// The design is [the concept](../../docs/concept.md#interpretation) and [multi-values](../../docs/concept.md#multi-values).

/// Reads one component's properties, applying the multi-value rules exactly once.
///
/// **This is the only supported way to read a property.**
/// One writer wins outright; several that agree byte-wise collapse silently into the report's agreed multi-values;
/// several that disagree go to the policy.
/// Reimplementing that anywhere else is how the rules drift.
///
/// **Every returned value_view borrows the op's payload bytes**, so a component that keeps bytes must copy them —
/// the raw document is gone as soon as the parse returns.
class vdoc::property_reader
{
    // queries
public:
    /// The resolved value, or empty when nothing wrote the property or the policy dropped it.
    ///
    /// Const, and it still appends to the report: a reader is a read *of the document*, and recording what it had to
    /// resolve on the way is part of reading it.
    [[nodiscard]] cc::optional<value_view> try_get(property_id property) const;
    [[nodiscard]] cc::optional<value_view> try_get(cc::string_view property) const
    {
        return try_get(property_id::of(property));
    }

    /// The `$schema_version` this component was stored at, already resolved and already checked against the current one.
    /// 0 means nothing stamped it, which is a document written through set_raw rather than an unknown version.
    ///
    /// A parse migrates forward from whatever it finds here.
    [[nodiscard]] i32 schema_version() const { return _schema_version; }

    [[nodiscard]] entity_id entity() const { return _entity; }
    [[nodiscard]] component_type_id component() const { return _component; }

    /// The untyped properties, for the rare component that must iterate what it was not told about.
    /// Coming through here means owning the multi-value rules yourself, which is exactly what try_get exists to avoid.
    [[nodiscard]] raw_component const& raw() const { return *_raw; }

    [[nodiscard]] parse_policy const& policy() const { return *_policy; }
    [[nodiscard]] parse_report& report() const { return *_report; }

    // construction
public:
    property_reader() = default;

    /// The parser is what normally builds one; this exists so a test can drive a parse in isolation.
    /// `raw`, `policy` and `report` must all outlive the reader.
    [[nodiscard]] static property_reader create_for(raw_component const& raw,
                                                    parse_policy const& policy,
                                                    parse_report& report,
                                                    entity_id entity,
                                                    component_type_id component,
                                                    i32 schema_version);

private:
    raw_component const* _raw = nullptr;
    parse_policy const* _policy = nullptr;
    parse_report* _report = nullptr;
    entity_id _entity;
    component_type_id _component;
    i32 _schema_version = 0;
};

/// How to interpret a raw document: which component types are known, which entities exist, how genuine conflicts
/// resolve.
///
/// The whole "it depends on the application" surface is concentrated here, on purpose, so that storage carries none
/// of it and the parser carries none of it either.
/// `default_parse_policy` bakes in the library's conventions.
class vdoc::parse_policy
{
    // interpretation
public:
    /// The schema for a component type, or null when this policy does not understand it.
    [[nodiscard]] virtual component_schema const* query_component_schema(component_type_id type) const = 0;

    /// Whether this entity exists at all; false suppresses it and every component on it.
    ///
    /// The `$entity` / `$alive` convention lives in default_parse_policy rather than in the parser, so a policy is
    /// free to suppress entities on grounds of its own.
    [[nodiscard]] virtual bool should_instantiate_entity(entity_id entity, raw_entity const& raw, parse_report& report) const
        = 0;

    /// Picks one of several disagreeing writers, or empty to drop the property.
    ///
    /// `candidates` is sorted by writer op id bytes and holds at least two entries that are not all byte-equal —
    /// agreeing writers never reach here.
    /// Must be total and reproducible: the same inputs give the same document on every machine.
    [[nodiscard]] virtual cc::optional<value_view> resolve_multi_value(property_path const& path,
                                                                       cc::span<property_value const> candidates,
                                                                       parse_report& report) const = 0;

    // lifetime
public:
    virtual ~parse_policy() = default;

protected:
    parse_policy() = default;
    parse_policy(parse_policy const&) = default;
    parse_policy(parse_policy&&) = default;
    parse_policy& operator=(parse_policy const&) = default;
    parse_policy& operator=(parse_policy&&) = default;
};
