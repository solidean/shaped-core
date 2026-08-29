#include <clean-core/platform/system_identifier.hh>
#include <clean-core/string/char_predicates.hh>
#include <nexus/test.hh>

// Nothing here asserts a value: a hostname is whatever this machine is called.
// What it pins is that a field is gathered only when it was asked for, which is the whole point of the type.

TEST("cc system_identifier - an empty request gathers nothing")
{
    auto const id = cc::query_system_identifier({});

    CHECK(!id.hostname.has_value());
    CHECK(!id.username.has_value());
    CHECK(!id.machine_id.has_value());
    CHECK(id.mac_addresses.empty());
    CHECK(id.disk_serials.empty());
}

TEST("cc system_identifier - one field asked for leaves the others alone")
{
    auto const id = cc::query_system_identifier(cc::identity_field::hostname);

    CHECK(!id.username.has_value());
    CHECK(!id.machine_id.has_value());
    CHECK(id.mac_addresses.empty());
    CHECK(id.disk_serials.empty());

    // The hostname itself may be absent — a container without one is normal — but it must not be empty text.
    if (id.hostname.has_value())
        CHECK(!id.hostname.value().empty());
}

TEST("cc system_identifier - a gathered field is either absent or non-empty")
{
    auto const which = cc::identity_field::hostname | cc::identity_field::username | cc::identity_field::machine_id
                     | cc::identity_field::mac_addresses | cc::identity_field::disk_serials;
    auto const id = cc::query_system_identifier(which);

    for (auto const* field : {&id.hostname, &id.username, &id.machine_id})
        if (field->has_value())
            CHECK(!field->value().empty());

    for (auto const& mac : id.mac_addresses)
    {
        // "00:1a:2b:3c:4d:5e" — six pairs and five separators.
        CHECK(mac.size() == 17);
        for (auto const c : cc::string_view(mac))
            CHECK((cc::is_hex_digit(c) || c == ':')); // extra parens: CHECK decomposes the expression, and || breaks it
    }

    for (auto const& serial : id.disk_serials)
        CHECK(!serial.empty());

    // A machine that can answer none of this is a valid machine, so the test still pins something there: the query
    // describes the machine rather than the call, and asking twice sees the same fields present.
    auto const again = cc::query_system_identifier(which);
    CHECK(again.hostname.has_value() == id.hostname.has_value());
    CHECK(again.username.has_value() == id.username.has_value());
    CHECK(again.machine_id.has_value() == id.machine_id.has_value());
    CHECK(again.mac_addresses.size() == id.mac_addresses.size());
    CHECK(again.disk_serials.size() == id.disk_serials.size());
}

TEST("cc system_identifier - the query is not memoized")
{
    // Two calls asking for nothing must both gather nothing, rather than the second inheriting the first's answer.
    CHECK(!cc::query_system_identifier(cc::identity_field::hostname).username.has_value());
    CHECK(!cc::query_system_identifier({}).hostname.has_value());
}
