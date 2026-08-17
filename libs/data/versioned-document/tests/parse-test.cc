#include <clean-core/common/assert.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <nexus/test.hh>
#include <versioned-document/parse_policy.hh>

using namespace cc::primitive_defines;

using vdoc::component_type_id;
using vdoc::diagnostic_kind;
using vdoc::entity_id;
using vdoc::op_id;
using vdoc::parse_report;
using vdoc::property_id;
using vdoc::property_path;
using vdoc::property_reader;
using vdoc::raw_component;
using vdoc::raw_property;
using vdoc::value;
using vdoc::value_view;

namespace
{
/// A distinct op id per seed, for writers that only have to be told apart and ordered.
[[nodiscard]] op_id op_id_of(u8 seed)
{
    byte bytes[op_id::byte_size] = {};
    bytes[0] = byte(seed);
    return op_id::from_bytes(bytes);
}

/// A policy that records what it was asked and answers by a fixed rule, so a test can see which branch ran.
struct recording_policy final : vdoc::parse_policy
{
    /// What resolve_multi_value should return: the largest writer id's value, or nothing.
    bool resolves = true;

    mutable isize resolve_calls = 0;

    [[nodiscard]] vdoc::component_schema const* query_component_schema(component_type_id) const override
    {
        return nullptr;
    }

    [[nodiscard]] bool should_instantiate_entity(entity_id, vdoc::raw_entity const&, parse_report&) const override
    {
        return true;
    }

    [[nodiscard]] cc::optional<value_view> resolve_multi_value(property_path const& path,
                                                               cc::span<vdoc::property_value const> candidates,
                                                               parse_report& report) const override
    {
        ++resolve_calls;
        CC_ASSERT(candidates.size() >= 2, "the reader must not delegate a singly-written property");

        if (!resolves)
            return {};

        report.diagnostics.push_back({.kind = diagnostic_kind::multi_valued_conflict,
                                      .path = path,
                                      .chosen_writer = candidates.back().writer,
                                      .writer_count = candidates.size()});
        return candidates.back().value;
    }
};

/// One component's properties, with the values it views kept alive alongside.
struct raw_fixture
{
    raw_component component;
    cc::vector<cc::unique_ptr<value>> owned;

    /// Adds one property with one writer per value, in the sorted-by-writer-bytes order materialization guarantees.
    /// Properties must be added in ascending property-name order, which is what raw_component's binary search needs.
    template <class... ValueTs>
    void add(cc::string_view property, ValueTs... values)
    {
        auto prop = raw_property();
        auto writer_index = isize(0);
        (
            [&]
            {
                owned.push_back(cc::make_unique<value>(value::of_i64(i64(values))));
                prop.writers.push_back({.writer = op_id_of(u8(++writer_index)), .value = owned.back()->view()});
            }(),
            ...);

        component.properties.push_back({.property = property_id::of(property), .value = cc::move(prop)});
    }
};

[[nodiscard]] property_reader reader_of(raw_fixture const& raw, recording_policy const& policy, parse_report& report)
{
    return property_reader::create_for(raw.component, policy, report, entity_id::of("wall-17"),
                                       component_type_id::of("Transform"), 1);
}
} // namespace

TEST("vdoc - a property nothing wrote reads as empty")
{
    auto const raw = raw_fixture();
    auto const policy = recording_policy();
    auto report = parse_report();

    auto const r = reader_of(raw, policy, report);
    CHECK(!r.try_get("x").has_value());
    CHECK(report.is_empty());
    CHECK(policy.resolve_calls == 0);
}

TEST("vdoc - a singly-written property reads through without touching the report")
{
    auto raw = raw_fixture();
    raw.add("x", 7);

    auto const policy = recording_policy();
    auto report = parse_report();

    auto const r = reader_of(raw, policy, report);
    auto const v = r.try_get("x");
    REQUIRE(v.has_value());
    CHECK(v.value().as_i64() == 7);
    CHECK(report.is_empty());
    CHECK(policy.resolve_calls == 0);
}

TEST("vdoc - writers that agree byte-wise collapse silently into an agreed multi-value")
{
    auto raw = raw_fixture();
    raw.add("x", 7, 7, 7);

    auto const policy = recording_policy();
    auto report = parse_report();

    auto const r = reader_of(raw, policy, report);
    auto const v = r.try_get("x");
    REQUIRE(v.has_value());
    CHECK(v.value().as_i64() == 7);

    // Not a problem, so not a diagnostic — a tidy-up hint for a later write, and nothing more.
    CHECK(report.diagnostics.empty());
    REQUIRE(report.agreed_multi_values.size() == 1);
    CHECK(report.agreed_multi_values[0].writer_count == 3);
    CHECK(report.agreed_multi_values[0].path.property == property_id::of("x"));
    CHECK(policy.resolve_calls == 0);
}

TEST("vdoc - writers that disagree go to the policy, and its choice is what the reader returns")
{
    auto raw = raw_fixture();
    raw.add("x", 7, 9);

    auto const policy = recording_policy();
    auto report = parse_report();

    auto const r = reader_of(raw, policy, report);
    auto const v = r.try_get("x");
    REQUIRE(v.has_value());
    CHECK(v.value().as_i64() == 9);

    CHECK(policy.resolve_calls == 1);
    CHECK(report.agreed_multi_values.empty());
    REQUIRE(report.diagnostics.size() == 1);
    CHECK(report.diagnostics[0].kind == diagnostic_kind::multi_valued_conflict);
    CHECK(report.diagnostics[0].writer_count == 2);
}

TEST("vdoc - a policy that drops a contested property leaves the reader with nothing")
{
    auto raw = raw_fixture();
    raw.add("x", 7, 9);

    auto policy = recording_policy();
    policy.resolves = false;
    auto report = parse_report();

    auto const r = reader_of(raw, policy, report);
    CHECK(!r.try_get("x").has_value());
    CHECK(policy.resolve_calls == 1);
}

TEST("vdoc - one writer of a differing pair is still not a conflict when the others agree with it")
{
    // Three writers, two distinct values: the reader must delegate rather than pick the majority itself.
    auto raw = raw_fixture();
    raw.add("x", 7, 7, 9);

    auto const policy = recording_policy();
    auto report = parse_report();

    auto const r = reader_of(raw, policy, report);
    CHECK(r.try_get("x").value().as_i64() == 9);
    CHECK(policy.resolve_calls == 1);
    CHECK(report.agreed_multi_values.empty());
}

TEST("vdoc - a report accumulates across reads and clears in one call")
{
    auto raw = raw_fixture();
    raw.add("x", 7, 7);
    raw.add("y", 1, 2);

    auto const policy = recording_policy();
    auto report = parse_report();

    auto const r = reader_of(raw, policy, report);
    CHECK(r.try_get("x").has_value());
    CHECK(r.try_get("y").has_value());

    CHECK(report.agreed_multi_values.size() == 1);
    CHECK(report.count_of(diagnostic_kind::multi_valued_conflict) == 1);
    CHECK(report.count_of(diagnostic_kind::contested_alive) == 0);
    CHECK(!report.is_empty());

    report.clear();
    CHECK(report.is_empty());
}
