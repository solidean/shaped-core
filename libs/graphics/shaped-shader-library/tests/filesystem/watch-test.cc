#include <nexus/test.hh>
#include <shaped-shader-library/filesystem/embedded_filesystem.hh>
#include <shaped-shader-library/filesystem/memory_filesystem.hh>
#include <shaped-shader-library/filesystem/mount_table.hh>

#include <memory>

// A notification is a hint to rescan, so what these pin is not "the sink knows what changed" but the three
// things a caller actually leans on: it fires when something under the prefix moved, it stops dead once the
// subscription is gone, and a filesystem that cannot notify says so instead of just never firing.
//
// mount_table's composition gets the most attention here. It is the one piece with no OS in it that can
// still be quietly, half-way wrong — watching some of the tree and reporting success.

namespace
{
/// A filesystem that cannot notify: it just leaves watch() at the base's nullopt. Every platform without a
/// watch backend looks like this, which is the fallback mount_table has to notice and honour.
struct unwatchable_filesystem final : slib::filesystem
{
    [[nodiscard]] cc::optional<cc::string> read_text(cc::string_view) const override { return cc::nullopt; }
    [[nodiscard]] slib::file_revision revision(cc::string_view) const override { return slib::file_revision::none; }
};
} // namespace

TEST("slib - memory_filesystem fires a watch on write and remove")
{
    slib::memory_filesystem fs;

    int fires = 0;
    auto const sub = fs.watch("", [&fires] { ++fires; });
    REQUIRE(sub.has_value());

    fs.write("a.hlsl", "v1");
    CHECK(fires == 1);

    fs.write("a.hlsl", "v2");
    CHECK(fires == 2);

    fs.remove("a.hlsl");
    CHECK(fires == 3);

    // Nothing changed, so nothing is worth telling anyone about.
    fs.remove("a.hlsl");
    CHECK(fires == 3);
}

TEST("slib - a sink can read the change it is being told about")
{
    // The sink fires after the write lands and off the state lock. Both halves matter: a sink that ran
    // under the lock could not call back in, and one that ran before the write would rescan the old text.
    slib::memory_filesystem fs;

    cc::string seen;
    auto const sub = fs.watch("", [&] { seen = fs.read_text("a.hlsl").value_or(cc::string()); });
    REQUIRE(sub.has_value());

    fs.write("a.hlsl", "v1");
    CHECK(seen == "v1");
}

TEST("slib - dropping a subscription stops the sink")
{
    slib::memory_filesystem fs;

    int fires = 0;
    {
        auto const sub = fs.watch("", [&fires] { ++fires; });
        REQUIRE(sub.has_value());

        fs.write("a.hlsl", "v1");
        CHECK(fires == 1);
    }

    // The subscription is gone, so the sink must be unreachable. It captured `fires` by reference; in a
    // real watcher that reference is the actor a late notification would reach into after it had died.
    fs.write("a.hlsl", "v2");
    CHECK(fires == 1);
}

TEST("slib - memory_filesystem scopes a watch to its prefix")
{
    slib::memory_filesystem fs;

    int fires = 0;
    auto const sub = fs.watch("shaders", [&fires] { ++fires; });
    REQUIRE(sub.has_value());

    fs.write("shaders/a.hlsl", "x");
    CHECK(fires == 1);

    fs.write("shaders/nested/b.hlsli", "x");
    CHECK(fires == 2);

    // Outside the prefix. The contract allows a filesystem to over-fire, so this is stricter than a caller
    // may assume — but memory_filesystem knows exactly what moved, and a test that cannot tell proves little.
    fs.write("other/c.hlsl", "x");
    CHECK(fires == 2);

    // Segment-wise, so a sibling that merely shares a spelling is not under it.
    fs.write("shaders_old/d.hlsl", "x");
    CHECK(fires == 2);
}

TEST("slib - a watch on a prefix that escapes the root never fires")
{
    slib::memory_filesystem fs;

    int fires = 0;
    auto const sub = fs.watch("../outside", [&fires] { ++fires; });

    // Valid, not nullopt: nothing is reachable out there, so "this will never fire" is the whole truth —
    // the same answer read_text gives by resolving to nothing.
    REQUIRE(sub.has_value());

    fs.write("a.hlsl", "x");
    CHECK(fires == 0);
}

TEST("slib - embedded_filesystem returns a subscription that never fires")
{
    static constexpr slib::embedded_file k_files[] = {{.path = "a.hlsl", .text = "void main() {}"}};
    slib::embedded_filesystem fs{k_files};

    int fires = 0;
    auto const sub = fs.watch("", [&fires] { ++fires; });

    // A valid subscription, deliberately not nullopt: "I will never notify" is a promise, "I cannot notify"
    // is a request to be polled. A shipped build rides on the difference — its watcher does nothing at all,
    // rather than polling an embedded copy that cannot move.
    CHECK(sub.has_value());
    CHECK(fires == 0);
}

TEST("slib - a filesystem that cannot notify returns nullopt")
{
    unwatchable_filesystem fs;
    CHECK(!fs.watch("", [] {}).has_value());
}

TEST("slib - mount_table watches through the mount that contains the prefix")
{
    auto fs = std::make_shared<slib::memory_filesystem>();
    slib::mount_table mounts;
    mounts.mount("pkg", fs);

    int fires = 0;
    auto const sub = mounts.watch("pkg/shaders", [&fires] { ++fires; });
    REQUIRE(sub.has_value());

    // The table's "pkg/shaders" is this mount's own "shaders": the prefix is rebased on the way in, or the
    // mount would be watching a directory it does not have.
    fs->write("shaders/a.hlsl", "x");
    CHECK(fires == 1);

    fs->write("elsewhere/b.hlsl", "x");
    CHECK(fires == 1);

    SECTION("including a mount at the table's root")
    {
        auto root = std::make_shared<slib::memory_filesystem>();
        slib::mount_table rooted;
        rooted.mount("", root);

        int root_fires = 0;
        auto const root_sub = rooted.watch("pkg/shaders", [&root_fires] { ++root_fires; });
        REQUIRE(root_sub.has_value());

        // Nothing to rebase here — the mount's root *is* the table's root, so the prefix passes through.
        root->write("pkg/shaders/a.hlsl", "x");
        CHECK(root_fires == 1);

        root->write("pkg/other/b.hlsl", "x");
        CHECK(root_fires == 1);
    }
}

TEST("slib - mount_table watches every mount inside the prefix")
{
    auto pkg = std::make_shared<slib::memory_filesystem>();
    auto common = std::make_shared<slib::memory_filesystem>();

    slib::mount_table mounts;
    mounts.mount("pkg", pkg);
    mounts.mount("pkg/common", common);

    int fires = 0;
    auto const sub = mounts.watch("pkg", [&fires] { ++fires; });
    REQUIRE(sub.has_value());

    pkg->write("a.hlsl", "x");
    CHECK(fires == 1);

    // The trap this guards: a lookup only ever asks "which mount holds this one file", so a walk written
    // for lookups finds `pkg` and misses `pkg/common` — and the composed watch goes blind to a whole
    // subtree while still reporting success.
    common->write("brdf.hlsli", "x");
    CHECK(fires == 2);
}

TEST("slib - mount_table ignores mounts disjoint from the prefix")
{
    auto pkg = std::make_shared<slib::memory_filesystem>();
    auto other = std::make_shared<slib::memory_filesystem>();

    slib::mount_table mounts;
    mounts.mount("pkg", pkg);
    mounts.mount("other", other);

    int fires = 0;
    auto const sub = mounts.watch("pkg", [&fires] { ++fires; });
    REQUIRE(sub.has_value());

    other->write("a.hlsl", "x");
    CHECK(fires == 0);

    pkg->write("a.hlsl", "x");
    CHECK(fires == 1);
}

TEST("slib - mount_table cannot notify when an intersecting mount cannot")
{
    slib::mount_table mounts;
    mounts.mount("pkg", std::make_shared<slib::memory_filesystem>());
    mounts.mount("pkg/vendor", std::make_shared<unwatchable_filesystem>());

    // All-or-nothing on purpose: one mount that cannot notify sends the caller back to polling everything,
    // rather than leaving it quietly blind to the part of the tree that mount serves.
    CHECK(!mounts.watch("pkg", [] {}).has_value());
    CHECK(!mounts.watch("", [] {}).has_value());

    // A mount that cannot notify and does not intersect is simply not this watch's problem.
    CHECK(mounts.watch("pkg/shaders", [] {}).has_value());
}

TEST("slib - a composed watch fires once per mount that saw the change")
{
    // Two mounts at one prefix — the dev overlay: the embedded copy, then the source folder over it. Both
    // are watched, so a single logical edit can reach the sink more than once. That is allowed (a sink is
    // a hint, not a report) and it is exactly why reload_watcher coalesces before it scans.
    auto embedded = std::make_shared<slib::memory_filesystem>();
    auto source = std::make_shared<slib::memory_filesystem>();

    slib::mount_table mounts;
    mounts.mount("pkg", embedded);
    mounts.mount("pkg", source);

    int fires = 0;
    auto const sub = mounts.watch("pkg", [&fires] { ++fires; });
    REQUIRE(sub.has_value());

    embedded->write("a.hlsl", "x");
    source->write("a.hlsl", "x");
    CHECK(fires == 2);
}

TEST("slib - dropping a composed subscription stops every mount under it")
{
    auto pkg = std::make_shared<slib::memory_filesystem>();
    auto common = std::make_shared<slib::memory_filesystem>();

    slib::mount_table mounts;
    mounts.mount("pkg", pkg);
    mounts.mount("pkg/common", common);

    int fires = 0;
    {
        auto const sub = mounts.watch("pkg", [&fires] { ++fires; });
        REQUIRE(sub.has_value());

        pkg->write("a.hlsl", "x");
        CHECK(fires == 1);
    }

    // One subscription owns every child watch, so dropping it has to silence all of them — the mounts
    // themselves are still very much alive.
    pkg->write("a.hlsl", "y");
    common->write("brdf.hlsli", "y");
    CHECK(fires == 1);
}
