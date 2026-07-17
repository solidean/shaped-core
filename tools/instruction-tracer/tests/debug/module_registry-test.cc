#include <instruction-tracer/debug/module_registry.hh>
#include <nexus/test.hh>

using namespace itrace;

namespace
{
module_info make(u64 base, u64 size, cc::string_view path)
{
    module_info m;
    m.base = base;
    m.size = size;
    m.path = path;
    m.name = path_file_name(path);
    return m;
}
} // namespace

TEST("path_file_name - both separators")
{
    CHECK(path_file_name("C:\\dir\\mymodule.exe") == "mymodule.exe");
    CHECK(path_file_name("/usr/lib/libfoo.so") == "libfoo.so");
    CHECK(path_file_name("C:/mixed\\sep/mod.dll") == "mod.dll");
    CHECK(path_file_name("bare.exe") == "bare.exe");
    CHECK(path_file_name("") == "");
}

TEST("module_registry - find by address")
{
    module_registry r;
    r.add(make(0x1000, 0x1000, "a.exe"));
    r.add(make(0x8000, 0x2000, "b.dll"));

    REQUIRE(r.find_by_address(0x1000) != nullptr);
    CHECK(r.find_by_address(0x1000)->name == "a.exe");
    CHECK(r.find_by_address(0x1fff)->name == "a.exe"); // last byte is inside
    CHECK(r.find_by_address(0x2000) == nullptr);       // one past the end is not
    CHECK(r.find_by_address(0x8fff)->name == "b.dll");
    CHECK(r.find_by_address(0x0fff) == nullptr);
}

TEST("module_registry - find by name is case-insensitive")
{
    module_registry r;
    r.add(make(0x1000, 0x1000, "C:\\dir\\MyModule.exe"));

    CHECK(r.find_by_name("MyModule.exe") != nullptr);
    CHECK(r.find_by_name("mymodule.exe") != nullptr);
    CHECK(r.find_by_name("MYMODULE.EXE") != nullptr);
    CHECK(r.find_by_name("other.exe") == nullptr);
}

TEST("module_registry - a bare stem matches, which is what people type")
{
    module_registry r;
    r.add(make(0x1000, 0x1000, "mymodule.exe"));

    CHECK(r.find_by_name("mymodule") != nullptr);
    CHECK(r.find_by_name("MyModule") != nullptr);
}

TEST("module_registry - an exact name beats a stem match")
{
    // Both are plausible answers for "foo"; the full name must win.
    module_registry r;
    r.add(make(0x1000, 0x1000, "foo.exe"));
    r.add(make(0x8000, 0x1000, "foo"));

    REQUIRE(r.find_by_name("foo") != nullptr);
    CHECK(r.find_by_name("foo")->base == 0x8000);
}

TEST("module_registry - remove")
{
    module_registry r;
    r.add(make(0x1000, 0x1000, "a.exe"));
    r.remove(0x1000);

    CHECK(r.find_by_address(0x1000) == nullptr);
    CHECK(r.find_by_name("a.exe") == nullptr);
    CHECK(r.all().empty());

    r.remove(0x1000); // removing what is not there is fine
}

TEST("module_registry - a re-used base replaces, never duplicates")
{
    // The loader hands the same base back after an unload; the registry is a map, not a log.
    module_registry r;
    r.add(make(0x1000, 0x1000, "old.dll"));
    r.add(make(0x1000, 0x1000, "new.dll"));

    CHECK(r.all().size() == 1);
    REQUIRE(r.find_by_address(0x1000) != nullptr);
    CHECK(r.find_by_address(0x1000)->name == "new.dll");
    CHECK(r.find_by_name("old.dll") == nullptr);
}
