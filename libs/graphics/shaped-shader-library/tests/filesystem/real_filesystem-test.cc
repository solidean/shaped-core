#include <clean-core/thread/atomic.hh>
#include <nexus/test.hh>
#include <shaped-shader-library/filesystem/real_filesystem.hh>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

// real_filesystem is the only part of slib that touches the disk, so it is also the only part whose
// tests need a real directory. Everything above it is covered through memory_filesystem instead.
//
// The same goes double for the watch tests at the bottom: they are the only ones here that wait on an OS
// notification rather than observe a return value, so they are kept few and given a generous bound.

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

/// Waits for `pred` to hold, up to a generous bound. The watch tests below are the only ones in slib that
/// depend on the OS getting round to telling us something, so they are also the only ones that can be slow
/// or flaky — the bound is deliberately far larger than any plausible notification delay.
template <class PredT>
bool wait_until(PredT&& pred)
{
    constexpr int k_timeout_ms = 5000;
    constexpr int k_slice_ms = 5;

    for (int waited = 0; waited < k_timeout_ms; waited += k_slice_ms)
    {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(k_slice_ms));
    }
    return pred();
}
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

TEST("slib - real_filesystem watches its root for changes")
{
    temp_dir dir("slib-real-fs-watch");
    dir.write("a.hlsl", "v1");

    slib::real_filesystem fs{dir.root()};

    cc::atomic<int> fires{0};
    auto const sub = fs.watch("", [&fires] { fires.fetch_add(1); });

    dir.write("a.hlsl", "a much longer v2");

    // Both answers are honest, and the contract binds either way: a platform with a watch backend has to
    // actually fire, and one without has to have said nullopt rather than hand back a watch that stays
    // silent. Today that reads as "Windows with threads, or not"; it tightens by itself once inotify and
    // FSEvents land.
    bool const notified = sub.has_value() && wait_until([&] { return fires.load() > 0; });
    CHECK(notified == sub.has_value());

    if (!sub.has_value())
        return;

    // A file appearing counts too — an editor that saves by writing a temp file and renaming it over the
    // original never produces a plain modify, which is half of why the sink is only ever a hint to rescan.
    auto const before = fires.load();
    dir.write("b.hlsl", "new file");
    CHECK(wait_until([&] { return fires.load() > before; }));
}

TEST("slib - real_filesystem cannot watch a root that does not exist")
{
    temp_dir dir("slib-real-fs-watch-missing");
    slib::real_filesystem fs{cc::string((dir.path / "nope").string().c_str())};

    // nullopt, not a subscription that never fires: there is nothing to watch, so the only truthful answer
    // is "poll me". A shipped build with no source tree lands here.
    CHECK(!fs.watch("", [] {}).has_value());
}

TEST("slib - dropping a real_filesystem watch stops the sink")
{
    temp_dir dir("slib-real-fs-watch-stop");
    dir.write("a.hlsl", "v1");

    slib::real_filesystem fs{dir.root()};

    cc::atomic<int> fires{0};
    {
        auto const sub = fs.watch("", [&fires] { fires.fetch_add(1); });

        dir.write("a.hlsl", "a much longer v2");

        bool const notified = sub.has_value() && wait_until([&] { return fires.load() > 0; });
        CHECK(notified == sub.has_value()); // no backend here, nothing to tear down: see the test above

        if (!sub.has_value())
            return;
    }

    // The subscription is gone, so no notification may follow — the destructor's promise, and the one the
    // OS makes hardest to keep. There is nothing to wait *for* here, so this waits a short fixed while and
    // checks that nothing happened; a broken teardown moves the count on its own.
    auto const after_unsubscribe = fires.load();
    dir.write("a.hlsl", "v3");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(fires.load() == after_unsubscribe);
}

TEST("slib - real_filesystem does not read a directory as a file")
{
    temp_dir dir("slib-real-fs-dir");
    dir.write("sub/a.hlsl", "inside");

    slib::real_filesystem fs{dir.root()};
    CHECK(!fs.exists("sub")); // a directory is not a file
}
