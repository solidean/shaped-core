#include <clean-core/common/macros.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

TEST("file_path - the temp directory is usable as a prefix")
{
    auto const dir = cc::temp_directory_path();
    CHECK(!dir.empty());
    CHECK(dir.back() != '/');
    CHECK(dir.back() != '\\');
}

TEST("file_path - every temp path is distinct and lands under the temp directory")
{
    auto const dir = cc::temp_directory_path();

    auto const a = cc::temp_file_path("cc-path-test", ".bin");
    auto const b = cc::temp_file_path("cc-path-test", ".bin");

    CHECK(a != b); // the counter separates two names asked for by the same process
    CHECK(cc::string_view(a).starts_with(dir));
    CHECK(cc::string_view(a).ends_with(".bin"));
    CHECK(cc::string_view(a).contains("cc-path-test"));

    // Naming a path creates nothing, so a fresh name never exists.
    CHECK(cc::file_read_stream_adapter::open(a).has_error());
}

TEST("file_path - removing reports gone rather than removed")
{
    auto const path = cc::temp_file_path("cc-path-remove", ".bin");

    // Never created, so removal still reports the postcondition: the file is not there.
    CHECK(cc::remove_file(path));

    {
        auto created = cc::file_write_stream_adapter::create(path);
        REQUIRE(created.has_value());
        auto stream = created.value().stream();
        REQUIRE(stream.write(cc::span<byte const>()).has_value());
        REQUIRE(stream.flush().has_value());
    }
    REQUIRE(cc::file_read_stream_adapter::open(path).has_value());

    CHECK(cc::remove_file(path));
    CHECK(cc::file_read_stream_adapter::open(path).has_error());
}

TEST("file_path - the user's config directory is where the platform's convention puts it", nx::config::exclusive("env"))
{
#if defined(CC_OS_WINDOWS)
    auto const app_data = cc::scoped_environment_variable("APPDATA", R"(C:\Users\someone\AppData\Roaming\)");
    CHECK(cc::user_config_directory_path() == cc::string(R"(C:\Users\someone\AppData\Roaming)"));
    auto const none = cc::scoped_environment_variable("APPDATA", "");
    CHECK(!cc::user_config_directory_path().has_value());
#elif defined(CC_OS_EMSCRIPTEN) || defined(CC_OS_WASI) || defined(CC_OS_ANDROID)
    CHECK(!cc::user_config_directory_path().has_value());
#elif defined(CC_OS_MACOS)
    auto const home = cc::scoped_environment_variable("HOME", "/Users/someone/");
    CHECK(cc::user_config_directory_path() == cc::string("/Users/someone/Library/Application Support"));
#else
    auto const home = cc::scoped_environment_variable("HOME", "/home/someone");
    auto const xdg = cc::scoped_environment_variable("XDG_CONFIG_HOME", "/elsewhere/config/");
    CHECK(cc::user_config_directory_path() == cc::string("/elsewhere/config"));
    // A relative XDG_CONFIG_HOME is invalid, and falls back as an unset one does.
    auto const relative = cc::scoped_environment_variable("XDG_CONFIG_HOME", "config");
    CHECK(cc::user_config_directory_path() == cc::string("/home/someone/.config"));
#endif
}
