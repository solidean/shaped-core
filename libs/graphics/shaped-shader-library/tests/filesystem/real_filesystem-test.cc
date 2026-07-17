#include <nexus/test.hh>
#include <shaped-shader-library/filesystem/real_filesystem.hh>

#include <filesystem>
#include <fstream>
#include <string>

// real_filesystem is the only part of slib that touches the disk, so it is also the only part whose
// tests need a real directory. Everything above it is covered through memory_filesystem instead.

namespace
{
// A unique directory in the OS temp dir, removed when the guard goes out of scope.
struct temp_dir
{
    std::filesystem::path path;

    explicit temp_dir(char const* name) : path(std::filesystem::temp_directory_path() / name)
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }

    ~temp_dir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    temp_dir(temp_dir const&) = delete;
    temp_dir& operator=(temp_dir const&) = delete;

    void write(char const* relative, char const* text) const
    {
        auto const file = path / relative;
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
    }

    [[nodiscard]] cc::string root() const { return cc::string(path.string().c_str()); }
};
} // namespace

TEST("slib - real_filesystem reads files under its root")
{
    temp_dir dir("slib-real-fs-read");
    dir.write("a.hlsl", "void main() {}");
    dir.write("sub/b.hlsli", "#define X 1");

    slib::real_filesystem fs{dir.root()};

    CHECK(fs.read_text("a.hlsl").value() == "void main() {}");
    CHECK(fs.read_text("sub/b.hlsli").value() == "#define X 1");
    CHECK(fs.exists("a.hlsl"));

    CHECK(!fs.exists("missing.hlsl"));
    CHECK(!fs.read_text("missing.hlsl").has_value());
    CHECK(fs.revision("missing.hlsl") == slib::file_revision::none);
}

TEST("slib - real_filesystem revision moves when a file's content changes")
{
    temp_dir dir("slib-real-fs-revision");
    dir.write("a.hlsl", "one");

    slib::real_filesystem fs{dir.root()};
    auto const first = fs.revision("a.hlsl");
    CHECK(first != slib::file_revision::none);
    CHECK(fs.revision("a.hlsl") == first); // stable while nothing changes

    // Size is folded in alongside mtime precisely so an edit within one filesystem clock tick — which
    // this rewrite almost certainly is — still reads as changed.
    dir.write("a.hlsl", "one much longer body");
    CHECK(fs.read_text("a.hlsl").value() == "one much longer body");
    CHECK(fs.revision("a.hlsl") != first);
}

TEST("slib - real_filesystem confines lookups to its root")
{
    temp_dir dir("slib-real-fs-escape");
    dir.write("sub/a.hlsl", "inside");

    // Rooted at the subdirectory: the file one level up must be unreachable from here.
    slib::real_filesystem fs{cc::string((dir.path / "sub").string().c_str())};
    CHECK(fs.exists("a.hlsl"));

    CHECK(!fs.exists("../a.hlsl"));
    CHECK(!fs.read_text("../../../../../../etc/passwd").has_value());
    CHECK(fs.revision("../a.hlsl") == slib::file_revision::none);

    // Normalizing to something still inside the root is fine.
    CHECK(fs.exists("./x/../a.hlsl"));
}

TEST("slib - real_filesystem over a missing root finds nothing")
{
    // Not an error: this is what makes "mount the source dir over the embedded copy, if it exists"
    // work without a mode flag — a shipped build simply has no source dir.
    slib::real_filesystem fs{cc::string("C:/definitely/not/a/real/shader/dir")};

    CHECK(!fs.exists("a.hlsl"));
    CHECK(!fs.read_text("a.hlsl").has_value());
    CHECK(fs.revision("a.hlsl") == slib::file_revision::none);
}

TEST("slib - real_filesystem does not read a directory as a file")
{
    temp_dir dir("slib-real-fs-dir");
    dir.write("sub/a.hlsl", "inside");

    slib::real_filesystem fs{dir.root()};
    CHECK(!fs.exists("sub")); // a directory is not a file
}
