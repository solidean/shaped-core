#include <nexus/test.hh>
#include <shaped-shader-library/filesystem/embedded_filesystem.hh>
#include <shaped-shader-library/filesystem/memory_filesystem.hh>
#include <shaped-shader-library/filesystem/mount_table.hh>

#include <memory>

// Mount resolution order is the whole contract: longest prefix first, then most recently mounted first.
// It is easy to write and easy to get backwards, so both halves are pinned here.

namespace
{
std::shared_ptr<slib::memory_filesystem> make_fs(cc::string_view path, cc::string_view text)
{
    auto fs = std::make_shared<slib::memory_filesystem>();
    fs->write(path, text);
    return fs;
}
} // namespace

TEST("slib - mount_table serves a mounted filesystem")
{
    slib::mount_table mounts;
    CHECK(mounts.mount_count() == 0);
    CHECK(!mounts.exists("a.hlsl"));

    mounts.mount("", make_fs("a.hlsl", "root"));

    CHECK(mounts.mount_count() == 1);
    CHECK(mounts.read_text("a.hlsl").value() == "root");
    CHECK(mounts.exists("a.hlsl"));
    CHECK(!mounts.exists("missing.hlsl"));
}

TEST("slib - mount_table rebases a path onto the mount's own root")
{
    slib::mount_table mounts;
    // The filesystem knows nothing of "common" — that prefix is where the table hangs it. This is the
    // point of mounting: a shared library keeps a stable include path wherever its files actually live.
    mounts.mount("common", make_fs("brdf.hlsli", "brdf"));

    CHECK(mounts.read_text("common/brdf.hlsli").value() == "brdf");
    CHECK(!mounts.exists("brdf.hlsli"));
}

TEST("slib - mount_table prefers the longest matching prefix")
{
    slib::mount_table mounts;
    mounts.mount("", make_fs("common/brdf.hlsli", "from root"));
    mounts.mount("common", make_fs("brdf.hlsli", "from common"));

    // Both could answer; the deeper mount wins regardless of the order they were mounted in.
    CHECK(mounts.read_text("common/brdf.hlsli").value() == "from common");

    SECTION("independent of mount order")
    {
        slib::mount_table reversed;
        reversed.mount("common", make_fs("brdf.hlsli", "from common"));
        reversed.mount("", make_fs("common/brdf.hlsli", "from root"));
        CHECK(reversed.read_text("common/brdf.hlsli").value() == "from common");
    }
}

TEST("slib - mount_table lets a later mount shadow an earlier one at the same prefix")
{
    slib::mount_table mounts;
    mounts.mount("", make_fs("a.hlsl", "embedded"));
    mounts.mount("", make_fs("a.hlsl", "source"));

    // This is the dev overlay: mount the embedded copy, then the source folder over it.
    CHECK(mounts.read_text("a.hlsl").value() == "source");
    CHECK(mounts.mount_count() == 2);
}

TEST("slib - mount_table falls through to a shadowed mount for files the top one lacks")
{
    slib::mount_table mounts;

    static constexpr slib::embedded_file k_embedded[] = {
        {.path = "a.hlsl", .text = "embedded a"},
        {.path = "b.hlsl", .text = "embedded b"},
    };
    mounts.mount("", std::make_shared<slib::embedded_filesystem>(k_embedded));
    mounts.mount("", make_fs("a.hlsl", "source a")); // only overrides a.hlsl

    CHECK(mounts.read_text("a.hlsl").value() == "source a");
    CHECK(mounts.read_text("b.hlsl").value() == "embedded b"); // no source copy -> the embedded one
}

TEST("slib - mount_table revisions distinguish which mount answered")
{
    // The trap this guards: a file served by the embedded mount, then shadowed by a source file whose
    // own revision happens to equal the embedded constant. Without folding the mount's identity in, the
    // watcher would see an unchanged revision and never reload.
    slib::mount_table mounts;

    static constexpr slib::embedded_file k_embedded[] = {{.path = "a.hlsl", .text = "embedded"}};
    mounts.mount("", std::make_shared<slib::embedded_filesystem>(k_embedded));
    auto const embedded_revision = mounts.revision("a.hlsl");

    auto source = std::make_shared<slib::memory_filesystem>();
    mounts.mount("", source);
    source->write("a.hlsl", "source"); // the source file appears and takes over

    CHECK(mounts.read_text("a.hlsl").value() == "source");
    CHECK(mounts.revision("a.hlsl") != embedded_revision);
}

TEST("slib - mount_table tracks the revision of whichever mount answers")
{
    slib::mount_table mounts;
    auto fs = std::make_shared<slib::memory_filesystem>();
    mounts.mount("pkg", fs);

    fs->write("a.hlsl", "one");
    auto const first = mounts.revision("pkg/a.hlsl");

    fs->write("a.hlsl", "two");
    CHECK(mounts.revision("pkg/a.hlsl") != first);
    CHECK(mounts.revision("pkg/a.hlsl") != slib::file_revision::none);
}

TEST("slib - mount_table confines lookups to the mount")
{
    slib::mount_table mounts;
    mounts.mount("pkg", make_fs("a.hlsl", "x"));
    mounts.mount("", make_fs("secret.hlsl", "should not be reachable through pkg"));

    CHECK(mounts.read_text("pkg/a.hlsl").value() == "x");

    // "pkg/../secret.hlsl" normalizes to "secret.hlsl" *before* mount selection, so it resolves against
    // the root mount rather than reaching out of pkg's filesystem.
    CHECK(mounts.read_text("pkg/../secret.hlsl").has_value());

    // Climbing past the table's own root resolves to nothing at all.
    CHECK(!mounts.exists("../secret.hlsl"));
    CHECK(mounts.revision("../secret.hlsl") == slib::file_revision::none);
}

TEST("slib - mount_table rejects a null mount")
{
    slib::mount_table mounts;
    CHECK_ASSERTS(mounts.mount("", nullptr));
    CHECK_ASSERTS(mounts.mount("../escape", std::make_shared<slib::memory_filesystem>()));
}
