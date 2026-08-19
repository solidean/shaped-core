#include <clean-core/algorithm/sort.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <versioned-document/ids.hh>

#include <type_traits>

using namespace cc::primitive_defines;

using vdoc::component_type_id;
using vdoc::entity_id;
using vdoc::property_id;
using vdoc::property_path;

TEST("vdoc - an id round-trips its canonical bytes")
{
    auto const e = entity_id::of("wall-17");

    CHECK(e.as_string_view() == "wall-17");
    CHECK(e.size() == 7);
    CHECK(!e.empty());

    // interning is by content, so the same name is the same id whoever asked for it
    CHECK(e == entity_id::of("wall-17"));
    CHECK(e != entity_id::of("wall-18"));
}

TEST("vdoc - a default-constructed id is the empty id, and is valid")
{
    auto const e = entity_id();

    CHECK(e.empty());
    CHECK(e.size() == 0);
    CHECK(e.as_string_view() == "");

    // the empty name is a name like any other: storage attaches no meaning to it
    CHECK(e == entity_id::of(""));
    CHECK(std::is_lt(e.compare_bytes(entity_id::of("a"))));
}

TEST("vdoc - ids order by canonical bytes")
{
    auto const a = property_id::of("alpha");
    auto const b = property_id::of("beta");

    CHECK(std::is_lt(a.compare_bytes(b)));
    CHECK(std::is_gt(b.compare_bytes(a)));
    CHECK(std::is_eq(a.compare_bytes(property_id::of("alpha"))));

    CHECK(property_id::by_bytes{}(a, b));
    CHECK(!property_id::by_bytes{}(b, a));
    CHECK(!property_id::by_bytes{}(a, a));
}

TEST("vdoc - id ordering is by UNSIGNED bytes")
{
    // A non-ASCII entity name is ordinary, since ids are arbitrary application strings.
    // Comparing char directly is signed on our platforms, which would sort this before every ASCII name — and that
    // order reaches the op hash through the assignment sort.
    auto const ascii = entity_id::of("z");
    auto const high = entity_id::of("\xC3\xA4"); // 'a-umlaut', UTF-8

    CHECK(std::is_lt(ascii.compare_bytes(high)));
    CHECK(entity_id::by_bytes{}(ascii, high));
}

TEST("vdoc - id ordering does not depend on intern order")
{
    // The order must be a property of the bytes, not of who interned first.
    // If it leaked the process-local identity, a document would materialize differently on two machines.
    cc::vector<cc::string_view> const names = {"delta", "alpha", "charlie", "bravo"};

    cc::vector<entity_id> forward;
    cc::vector<entity_id> backward;
    for (isize i = 0; i < names.size(); ++i)
    {
        forward.push_back(entity_id::of(names[i]));
        backward.push_back(entity_id::of(names[names.size() - 1 - i]));
    }

    cc::sort(forward, entity_id::by_bytes{});
    cc::sort(backward, entity_id::by_bytes{});

    for (isize i = 0; i < forward.size(); ++i)
        CHECK(forward[i] == backward[i]);

    for (isize i = 1; i < forward.size(); ++i)
        CHECK(std::is_lt(forward[i - 1].compare_bytes(forward[i])));
}

TEST("vdoc - an id hashes by its bytes")
{
    auto const e = entity_id::of("wall-17");

    CHECK(hash(e) == hash(entity_id::of("wall-17")));
    CHECK(hash(entity_id()) == hash(entity_id::of("")));
}

TEST("vdoc - a property path orders by entity, then component, then property")
{
    auto const path = [](cc::string_view e, cc::string_view c, cc::string_view p)
    {
        return property_path{.entity = entity_id::of(e),
                             .component = component_type_id::of(c),
                             .property = property_id::of(p)};
    };

    auto const base = path("e1", "Transform", "position");

    // each level breaks the tie only when everything above it is equal
    CHECK(std::is_lt(base.compare_bytes(path("e2", "Aaa", "aaa"))));
    CHECK(std::is_lt(base.compare_bytes(path("e1", "Zzz", "aaa"))));
    CHECK(std::is_lt(base.compare_bytes(path("e1", "Transform", "rotation"))));
    CHECK(std::is_eq(base.compare_bytes(path("e1", "Transform", "position"))));

    CHECK(base == path("e1", "Transform", "position"));
    CHECK(base != path("e1", "Transform", "rotation"));
    CHECK(hash(base) == hash(path("e1", "Transform", "position")));
}

TEST("vdoc - the three id types cannot be interchanged")
{
    // The three inherit from different bases, so nothing converts between them and no call site can swap two.
    // The extra parens keep the template comma out of CHECK's argument list.
    CHECK((!std::is_convertible_v<entity_id, component_type_id>));
    CHECK((!std::is_convertible_v<component_type_id, property_id>));
    CHECK((!std::is_convertible_v<property_id, entity_id>));

    // and an id is still just the handle inside it, so passing one by value stays a pointer copy
    CHECK(sizeof(entity_id) == sizeof(cc::interned_string));
    CHECK(std::is_trivially_copyable_v<entity_id>);
}
