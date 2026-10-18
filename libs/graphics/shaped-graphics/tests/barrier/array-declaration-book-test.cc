#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-graphics/barrier/array_declarations.hh>
#include <shaped-graphics/barrier/resource_access.hh> // sg::is_unordered_write
#include <shaped-graphics/context/metrics.hh>
#include <shaped-graphics/resource/views.hh>

using namespace cc::primitive_defines;

// sg::impl::array_declaration_book with no device: when one op of a bind point applies the live declarations, and when it skips them.
// The backends feed it their bound groups' arrays and their trackers; these tests feed it fakes and count what it tracked.

namespace
{
struct fake_element
{
    int resource = -1;

    [[nodiscard]] bool is_vacant() const { return resource < 0; }
};

struct fake_array
{
    cc::string name;
    bool is_texture = false;
    isize binding = 0;
    sg::view_class bound_as = sg::view_class::readonly;
    cc::vector<fake_element> elements;
};

[[nodiscard]] fake_array array_of(cc::string_view name, int count, bool is_texture)
{
    auto a = fake_array{.name = cc::string(name),
                        .is_texture = is_texture,
                        .binding = 0,
                        .bound_as = sg::view_class::readonly,
                        .elements = {}};
    for (auto i = 0; i < count; ++i)
        a.elements.push_back({.resource = i});
    return a;
}

[[nodiscard]] fake_array buffers(cc::string_view name, int count)
{
    return array_of(name, count, false);
}

[[nodiscard]] fake_array textures(cc::string_view name, int count)
{
    return array_of(name, count, true);
}

/// One bind point's book and what a list around it keeps: the member-write count, the stats, and the tracked elements.
struct fake_list
{
    sg::impl::array_declaration_book book;
    u64 member_writes = 0;
    sg::impl::stat_counts stats;

    /// Set to make the next tracked element report that its op already held a write.
    bool held_write = false;
    i64 tracked = 0;
    i64 covered = 0;

    /// One op over `groups`, each the array of a group at index `i`, whose identity is its address.
    /// Every tracked element is a member, so a tracked write counts toward `member_writes` as a list's tracker does.
    /// Returns what `apply` does: whether some array of the op was skipped.
    bool op(cc::span<fake_array const* const> groups, sg::pipeline_stage_flags stages = sg::pipeline_stage_flag::compute)
    {
        auto sites = cc::vector<sg::impl::array_site<fake_array>>();
        for (auto i = 0; i < int(groups.size()); ++i)
            sites.push_back({.group = groups[i], .group_index = i, .array = groups[i]});
        auto const track = [&](fake_element const&, sg::pipeline_stage_flags, sg::access_flags access)
        {
            ++tracked;
            auto const writes = sg::is_unordered_write(access);
            if (writes)
                ++member_writes;
            return sg::impl::member_track{.member_write = writes, .held_write = held_write};
        };
        auto const track_texture
            = [&](fake_element const&, sg::pipeline_stage_flags, sg::access_flags access, sg::texture_layout)
        {
            ++tracked;
            auto const writes = sg::is_unordered_write(access);
            if (writes)
                ++member_writes;
            return sg::impl::member_track{.member_write = writes, .held_write = held_write};
        };
        auto const cover = [&](fake_array const&, fake_element const&, sg::impl::array_plan const&) { ++covered; };
        return book.apply(cc::span<sg::impl::array_site<fake_array> const>(sites), nullptr, this, stages, member_writes,
                          stats, track, track_texture, cover);
    }

    /// One op over `arrays`, each the array of a group of its own.
    bool op(cc::span<fake_array const> arrays, sg::pipeline_stage_flags stages = sg::pipeline_stage_flag::compute)
    {
        auto groups = cc::vector<fake_array const*>();
        for (auto const& array : arrays)
            groups.push_back(&array);
        return op(cc::span<fake_array const* const>(groups), stages);
    }

    bool op(fake_array const& array, sg::pipeline_stage_flags stages = sg::pipeline_stage_flag::compute)
    {
        return op(cc::span<fake_array const>(&array, 1), stages);
    }
};

[[nodiscard]] cc::vector<sg::array_buffer_access> reads(int count)
{
    auto out = cc::vector<sg::array_buffer_access>();
    for (auto i = 0; i < count; ++i)
        out.push_back({.index = i, .access = sg::access_flag::shader_read});
    return out;
}

} // namespace

TEST("sg::impl::array_declaration_book - a current read-only application is skipped")
{
    auto list = fake_list();
    auto const table = buffers("tables.buffers", 4);
    list.book.declare_buffers(1, "tables.buffers", reads(3));

    CHECK(!list.op(table));
    CHECK(list.tracked == 3);
    CHECK(list.op(table));
    CHECK(list.op(table));
    CHECK(list.tracked == 3);
    CHECK(list.stats.values[int(sg::stat::array_element_tracks)] == 3);
}

TEST("sg::impl::array_declaration_book - a member write between two ops makes the next one apply again")
{
    auto list = fake_list();
    auto const table = buffers("tables.buffers", 4);
    list.book.declare_buffers(1, "tables.buffers", reads(3));

    list.op(table);
    ++list.member_writes; // what a list counts for a write to an element, from any path
    list.op(table);
    CHECK(list.tracked == 6);
    list.op(table);
    CHECK(list.tracked == 6);
}

TEST("sg::impl::array_declaration_book - an application whose op already held a write is not current")
{
    auto list = fake_list();
    auto const table = buffers("tables.buffers", 4);
    list.book.declare_buffers(1, "tables.buffers", reads(3));

    list.held_write = true;
    list.op(table);
    list.held_write = false;
    list.op(table);
    CHECK(list.tracked == 6);
    list.op(table);
    CHECK(list.tracked == 6);
}

TEST("sg::impl::array_declaration_book - a write is applied at every op")
{
    auto list = fake_list();
    auto const table = buffers("tables.buffers", 2);
    sg::array_buffer_access const writes[] = {{.index = 1, .access = sg::access_flag::shader_write}};
    list.book.declare_buffers(1, "tables.buffers", writes);

    list.op(table);
    list.op(table);
    CHECK(list.tracked == 2);
}

TEST("sg::impl::array_declaration_book - a skipped read applies after all when a later array of its op writes a member")
{
    // One texture in two arrays of one op, read through the gallery and written through the annex.
    // Declaring the gallery again would combine both uses into one layout, so its skip must not stand.
    auto list = fake_list();
    auto const gallery = textures("gallery.images", 1);
    auto const annex = textures("annex.images", 1);
    sg::array_texture_access const read[]
        = {{.index = 0, .access = sg::access_flag::shader_read, .layout = sg::texture_layout::shader_texture}};
    sg::array_texture_access const write[]
        = {{.index = 0, .access = sg::access_flag::shader_write, .layout = sg::texture_layout::shader_image}};
    list.book.declare_textures(1, "gallery.images", read);
    list.book.declare_textures(2, "annex.images", write);

    list.op(gallery);
    CHECK(list.tracked == 1);
    fake_array const* const read_then_write[] = {&gallery, &annex};
    CHECK(!list.op(read_then_write)); // applied after all, so nothing is left for the flush to check
    CHECK(list.tracked == 3);         // the annex's write, then the gallery's read after all

    // Written first, the gallery is not current when its turn comes, and applies once.
    fake_array const* const write_then_read[] = {&annex, &gallery};
    list.op(write_then_read);
    CHECK(list.tracked == 5);

    // Nothing written in its op, the skip stands.
    list.op(gallery);
    CHECK(list.op(gallery));
    CHECK(list.tracked == 5);
    CHECK(list.stats.values[int(sg::stat::array_element_tracks)] == 5);
}

TEST("sg::impl::array_declaration_book - a key covered by what was applied skips, and one outside it applies once")
{
    auto list = fake_list();
    auto const table = buffers("tables.buffers", 4);
    list.book.declare_buffers(1, "tables.buffers", reads(2));

    // The compute and raytracing stages of one bind point, alternating as a trace after a dispatch does.
    list.op(table, sg::pipeline_stage_flag::compute);
    list.op(table, sg::pipeline_stage_flag::raytracing);
    CHECK(list.tracked == 4);
    list.op(table, sg::pipeline_stage_flag::compute);
    list.op(table, sg::pipeline_stage_flag::raytracing);
    CHECK(list.tracked == 4);

    // An invalidation clears the union, so both apply again once more.
    ++list.member_writes;
    list.op(table, sg::pipeline_stage_flag::compute);
    list.op(table, sg::pipeline_stage_flag::raytracing);
    CHECK(list.tracked == 8);
}

TEST("sg::impl::array_declaration_book - a declaration of another name leaves an array current")
{
    auto list = fake_list();
    auto const table = buffers("tables.buffers", 4);
    auto const other = buffers("tables.others", 4);
    list.book.declare_buffers(1, "tables.buffers", reads(3));

    list.op(table);
    list.book.declare_buffers(2, "tables.others", reads(1));
    list.op(other);
    CHECK(list.book.withdraw(2));
    list.op(table);
    CHECK(list.tracked == 4);

    // One of the same name is a new set, which applies.
    list.book.declare_buffers(3, "tables.buffers", reads(1));
    list.op(table);
    CHECK(list.tracked == 8);
    CHECK(list.book.withdraw(3));
}

TEST("sg::impl::array_declaration_book - an op where no group holds the array leaves the declaration dormant")
{
    auto list = fake_list();
    auto const table = buffers("tables.buffers", 4);
    auto const other = buffers("tables.others", 4);
    list.book.declare_buffers(1, "tables.buffers", reads(3));
    list.book.declare_buffers(2, "tables.others", {});

    list.op(other);
    list.op(table);
    list.op(other);
    list.op(table);
    CHECK(list.tracked == 3);
    CHECK(list.book.withdraw(1));
    CHECK(list.book.withdraw(2));
}

TEST("sg::impl::array_declaration_book - each group the declarations meet is checked, the smaller one too")
{
    auto list = fake_list();
    auto const large = buffers("tables.buffers", 4);
    auto const small = buffers("tables.buffers", 2);
    sg::array_buffer_access const last[] = {{.index = 3, .access = sg::access_flag::shader_read}};
    list.book.declare_buffers(1, "tables.buffers", last);

    list.op(large);
    CHECK(list.tracked == 1);

    // Two groups holding the name at once: the index is valid only in the first, and nothing is tracked.
    fake_array const both[] = {large, small};
    CHECK_ASSERTS(list.op(both));
    CHECK(list.tracked == 1);
}

TEST("sg::impl::array_declaration_book - many groups at one name each apply once, then each is current")
{
    // One group per draw under one declaration, as a caller binding a group of its own per draw does.
    auto list = fake_list();
    auto groups = cc::vector<fake_array>();
    for (auto i = 0; i < 200; ++i)
        groups.push_back(buffers("tables.buffers", 3));
    list.book.declare_buffers(1, "tables.buffers", reads(2));

    for (auto const& group : groups)
        list.op(group);
    CHECK(list.tracked == 400);
    for (auto const& group : groups)
        list.op(group);
    CHECK(list.tracked == 400);

    // Every group is checked on its own: one more, with a vacant element, asserts at its first meeting.
    auto vacant = buffers("tables.buffers", 3);
    vacant.elements[1].resource = -1;
    CHECK_ASSERTS(list.op(vacant));
    CHECK(list.tracked == 400);
}

TEST("sg::impl::array_declaration_book - a vacant element asserts at the first meeting with its group")
{
    auto list = fake_list();
    auto const filled = buffers("tables.buffers", 3);
    auto vacant = buffers("tables.buffers", 3);
    vacant.elements[1].resource = -1;
    list.book.declare_buffers(1, "tables.buffers", reads(2));

    list.op(filled);
    CHECK_ASSERTS(list.op(vacant));
}

TEST("sg::impl::array_declaration_book - an undeclared array is covered at every op")
{
    // Logged once per pipeline address for the whole process, so a repeated run may find it said already.
    nx::allow_errors("declared no access for a bound array");
    auto list = fake_list();
    auto table = buffers("tables.buffers", 3);
    table.elements[2].resource = -1;

    list.op(table);
    list.op(table);
    CHECK(list.covered == 4); // the vacant element is skipped
    CHECK(list.tracked == 0);
    CHECK(list.stats.values[int(sg::stat::array_element_tracks)] == 4);
}

TEST("sg::impl::array_declaration_book - a declaration made after its name met a group is checked against that group")
{
    auto list = fake_list();
    auto const table = buffers("tables.buffers", 4);
    list.book.declare_buffers(1, "tables.buffers", reads(2));
    list.op(table);

    sg::array_buffer_access const past_the_end[] = {{.index = 4, .access = sg::access_flag::shader_read}};
    list.book.declare_buffers(2, "tables.buffers", past_the_end);
    CHECK_ASSERTS(list.op(table));
}

TEST("sg::impl::array_declaration_book - end_all ends every declaration, and the next op finds the array undeclared")
{
    // Logged once per pipeline, binding and message, which an earlier test's list at this address may have spent.
    nx::allow_errors("declared no access for a bound array");
    auto list = fake_list();
    auto const table = buffers("tables.buffers", 3);
    list.book.declare_buffers(1, "tables.buffers", reads(1));
    list.book.declare_buffers(2, "tables.buffers", reads(2));
    list.op(table);
    CHECK(list.tracked == 3);
    list.book.end_all();
    CHECK(list.book.is_empty());
    CHECK(!list.book.withdraw(1));
    CHECK(!list.book.withdraw(2));

    list.op(table);
    CHECK(list.tracked == 3);
    CHECK(list.covered == 3);
}

TEST("sg::impl::array_skip_guard - a member written between a skip and its flush asserts at the flush")
{
    auto guard = sg::impl::array_skip_guard();

    // A read tracked between the two leaves the count alone.
    guard.arm(true, 7);
    guard.check(7);

    // Disarmed by the flush, and by an op that skipped nothing, so a later write is not this guard's.
    guard.check(8);
    guard.arm(false, 8);
    guard.check(9);

    guard.arm(true, 9);
    CHECK_ASSERTS(guard.check(10));
}
