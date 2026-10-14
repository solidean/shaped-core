#include <clean-core/common/macros.hh>
#include <clean-core/platform/native.hh>
#include <clean-core/platform/source_location.hh>
#include <clean-core/platform/symbolize.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>

// A name that cannot be a mangling comes back as it went in, without entering the platform demangler.

TEST("cc demangle_symbol - a source_location function name comes back unchanged")
{
    auto const name = cc::string_view(cc::source_location::current().function_name());
    CHECK(cc::demangle_symbol(name) == name);
}

TEST("cc demangle_symbol - readable names come back unchanged")
{
    CHECK(cc::demangle_symbol("int __cdecl foo(int)") == "int __cdecl foo(int)");
    CHECK(cc::demangle_symbol("int foo(int)") == "int foo(int)");
    CHECK(cc::demangle_symbol("class cc::string") == "class cc::string");
    CHECK(cc::demangle_symbol("") == "");
}

#ifdef CC_OS_WINDOWS
TEST("cc demangle_symbol - a readable name never takes the DbgHelp lock")
{
    // The lock is not recursive, so taking it here would deadlock, which the watchdog reports as a hang.
    auto result = cc::string();
    cc::impl::with_dbghelp([&] { result = cc::demangle_symbol("int __cdecl foo(int)"); });
    CHECK(result == "int __cdecl foo(int)");
}
#endif

TEST("cc demangle_symbol - a mangled name is still demangled")
{
#ifdef CC_OS_WINDOWS
    CHECK(cc::demangle_symbol("?f@@YAXXZ") == "void __cdecl f(void)");
#else
    CHECK(cc::demangle_symbol("_Z1fv") == "f()");
    // A type encoding has no "_Z" and is still the demangler's, which is what typeid(T).name() hands back.
    CHECK(cc::demangle_symbol("N2cc6stringE") == "cc::string");
#endif
}
