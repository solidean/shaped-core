#include <nexus/test.hh>
#include <shaped-shader-library/filesystem/embedded_filesystem.hh>
#include <shaped-shader-library/filesystem/memory_filesystem.hh>

// The two filesystems that need no disk. real_filesystem is covered where a real directory exists
// (see the reload tests); everything else in slib goes through this interface.

TEST("slib - memory_filesystem reads what was written")
{
    slib::memory_filesystem fs;

    CHECK(!fs.exists("a.hlsl"));
    CHECK(!fs.read_text("a.hlsl").has_value());
    CHECK(fs.revision("a.hlsl") == slib::file_revision::none);

    fs.write("a.hlsl", "void main() {}");

    CHECK(fs.exists("a.hlsl"));
    CHECK(fs.read_text("a.hlsl").value() == "void main() {}");
    CHECK(fs.revision("a.hlsl") != slib::file_revision::none);
}

TEST("slib - memory_filesystem bumps the revision on every write")
{
    slib::memory_filesystem fs;

    fs.write("a.hlsl", "one");
    auto const first = fs.revision("a.hlsl");

    fs.write("a.hlsl", "two");
    auto const second = fs.revision("a.hlsl");

    CHECK(fs.read_text("a.hlsl").value() == "two");
    CHECK(second != first); // this is the signal the reload watcher polls

    // Even rewriting identical content bumps it: the filesystem reports "may have changed", and the
    // compile cache is what makes an unchanged shader cheap.
    fs.write("a.hlsl", "two");
    CHECK(fs.revision("a.hlsl") != second);
}

TEST("slib - memory_filesystem removes files")
{
    slib::memory_filesystem fs;
    fs.write("a.hlsl", "x");

    CHECK(fs.remove("a.hlsl"));
    CHECK(!fs.exists("a.hlsl"));
    CHECK(fs.revision("a.hlsl") == slib::file_revision::none);
    CHECK(!fs.remove("a.hlsl")); // already gone
}

TEST("slib - memory_filesystem normalizes paths")
{
    slib::memory_filesystem fs;
    fs.write("dir/a.hlsl", "x");

    CHECK(fs.exists("dir/a.hlsl"));
    CHECK(fs.exists("./dir/a.hlsl"));
    CHECK(fs.exists("dir/./a.hlsl"));
    CHECK(fs.exists("dir/sub/../a.hlsl"));
    CHECK(fs.exists("/dir/a.hlsl"));
    CHECK(fs.exists("dir\\a.hlsl"));

    // A path escaping the root resolves to nothing rather than reaching out.
    CHECK(!fs.exists("../dir/a.hlsl"));
    CHECK_ASSERTS(fs.write("../escape.hlsl", "x"));
}

TEST("slib - embedded_filesystem serves baked files at a constant revision")
{
    // Shaped like what the package generator emits: static storage, normalized paths.
    static constexpr slib::embedded_file k_files[] = {
        {.path = "a.hlsl", .text = "void main() {}"},
        {.path = "dir/b.hlsli", .text = "#define X 1"},
    };
    slib::embedded_filesystem fs{k_files};

    CHECK(fs.read_text("a.hlsl").value() == "void main() {}");
    CHECK(fs.read_text("dir/b.hlsli").value() == "#define X 1");
    CHECK(!fs.read_text("missing.hlsl").has_value());

    CHECK(fs.revision("a.hlsl") != slib::file_revision::none);
    CHECK(fs.revision("missing.hlsl") == slib::file_revision::none);

    // Baked content cannot change, so the revision never moves — nothing to reload.
    CHECK(fs.revision("a.hlsl") == fs.revision("a.hlsl"));
    CHECK(fs.revision("a.hlsl") == fs.revision("dir/b.hlsli"));

    CHECK(fs.exists("./dir/../a.hlsl")); // normalized like every other filesystem
    CHECK(!fs.exists("../a.hlsl"));
}

TEST("slib - embedded_filesystem is empty without files")
{
    slib::embedded_filesystem fs{{}};
    CHECK(!fs.exists("a.hlsl"));
}
