#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/compare.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
struct entry
{
    i32 group = 0;
    i32 score = 0;
    cc::string name;
};

/// The order the fields appear in, as a plain string, so a whole ordering fits in one CHECK.
cc::string names_of(cc::vector<entry> const& entries)
{
    cc::string r;
    for (auto const& e : entries)
    {
        if (!r.empty())
            r += ' ';
        r += e.name;
    }
    return r;
}

cc::vector<entry> sample_entries()
{
    auto r = cc::vector<entry>();
    r.push_back({.group = 1, .score = 10, .name = "a"});
    r.push_back({.group = 0, .score = 30, .name = "b"});
    r.push_back({.group = 1, .score = 30, .name = "c"});
    r.push_back({.group = 0, .score = 10, .name = "d"});
    r.push_back({.group = 1, .score = 20, .name = "e"});
    return r;
}
} // namespace

TEST("compare_by - a single projection is just sort_by")
{
    auto values = sample_entries();
    cc::sort(values, cc::compare_by(&entry::score));

    CHECK(names_of(values) == "a d e b c");
}

TEST("compare_by - falls through to the next projection on a tie")
{
    auto values = sample_entries();
    cc::sort(values, cc::compare_by(&entry::group, &entry::score));

    CHECK(names_of(values) == "d b a e c");
}

TEST("compare_by - descending reverses just that projection")
{
    auto values = sample_entries();
    cc::sort(values, cc::compare_by(&entry::group, cc::descending(&entry::score)));

    CHECK(names_of(values) == "b d c e a");
}

TEST("compare_by - every projection can be descending")
{
    auto values = sample_entries();
    cc::sort(values, cc::compare_by(cc::descending(&entry::group), cc::descending(&entry::score)));

    CHECK(names_of(values) == "c e a b d");
}

TEST("compare_by - a lambda projection, and a computed key")
{
    auto values = cc::vector<cc::string>{"ccc", "a", "bb", "dd"};

    // by length, then alphabetically — the computed key is a temporary that must survive the comparison
    cc::sort(values, cc::compare_by([](cc::string const& s) { return s.size(); },
                                    [](cc::string const& s) { return cc::string_view(s); }));

    CHECK(values[0] == "a");
    CHECK(values[1] == "bb");
    CHECK(values[2] == "dd");
    CHECK(values[3] == "ccc");
}

TEST("compare_by - three projections, the deepest one deciding")
{
    struct location
    {
        cc::string file;
        i32 line = 0;
        cc::string name;
    };

    auto values = cc::vector<location>();
    values.push_back({.file = "b.cc", .line = 1, .name = "x"});
    values.push_back({.file = "a.cc", .line = 7, .name = "x"});
    values.push_back({.file = "a.cc", .line = 2, .name = "x"});
    values.push_back({.file = "a.cc", .line = 2, .name = "w"});

    cc::sort(values, cc::compare_by(&location::name, &location::file, &location::line));

    CHECK(values[0].name == "w");
    CHECK(values[1].line == 2);
    CHECK(values[2].line == 7);
    CHECK(values[3].file == "b.cc");
}

TEST("compare_by - the comparator is a strict weak ordering")
{
    auto const compare = cc::compare_by(&entry::group, &entry::score);

    auto const a = entry{.group = 1, .score = 10, .name = "a"};
    auto const b = entry{.group = 1, .score = 20, .name = "b"};
    auto const equivalent = entry{.group = 1, .score = 10, .name = "z"};

    CHECK(compare(a, b));
    CHECK(!compare(b, a));
    CHECK(!compare(a, a));

    // equivalent under every projection means neither comes first, which is what the sorts require
    CHECK(!compare(a, equivalent));
    CHECK(!compare(equivalent, a));
}
