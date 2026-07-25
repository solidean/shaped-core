#include <clean-core/string/format.hh>
#include <instruction-tracer/report/source_cache.hh>
#include <instruction-tracer/report/source_view.hh>
#include <nexus/test.hh>

#include <filesystem>
#include <fstream>
#include <string>

using namespace itrace;

namespace
{
recorded_instruction at(cc::string file, u32 line)
{
    recorded_instruction insn;
    insn.file = cc::move(file);
    insn.line = line;
    return insn;
}

trace trace_of(cc::vector<recorded_instruction> instructions)
{
    trace t;
    t.index = 1;
    t.instructions = cc::move(instructions);
    return t;
}

/// Write a 30-line file to a unique temp path; line 10 is indented so indentation-preservation can be
/// checked. Returns the path (caller removes it).
cc::string write_fixture()
{
    auto const path = std::filesystem::temp_directory_path() / "itrace_source_view_test.cc";
    std::ofstream f(path, std::ios::binary);
    for (int i = 1; i <= 30; ++i)
    {
        if (i == 10)
            f << "    int indented = 10;\n";
        else
            f << "line " << i << "\n";
    }
    auto const s = path.string();
    return cc::string(cc::string_view(s.data(), isize(s.size())));
}

/// Write `content` verbatim to a temp path named `name`. Returns the path (caller removes it).
cc::string write_file(char const* name, cc::string_view content)
{
    auto const path = std::filesystem::temp_directory_path() / name;
    std::ofstream f(path, std::ios::binary);
    f.write(content.data(), std::streamsize(content.size()));
    auto const s = path.string();
    return cc::string(cc::string_view(s.data(), isize(s.size())));
}

void remove_file(cc::string_view path)
{
    std::filesystem::remove(std::filesystem::path(std::string(path.data(), size_t(path.size()))));
}
} // namespace

TEST("source cache - a CRLF file yields lines with no carriage return")
{
    // raw_line preserves indentation but must never hand a '\r' on to a source view. The line ending is
    // normalized where the file is read, so nothing downstream has to know which ending it had.
    auto const path = write_file("itrace_source_cache_crlf.cc", "  first\r\nsecond\r\n\r\nlast\r\n");

    source_cache sources;
    CHECK(sources.line_count(path) == 4);
    CHECK(sources.raw_line(path, 1) == "  first");
    CHECK(sources.line(path, 1) == "first");
    CHECK(sources.raw_line(path, 3) == "");
    CHECK(sources.raw_line(path, 4) == "last");

    remove_file(path);
}

TEST("source cache - a final line without a newline still counts")
{
    auto const path = write_file("itrace_source_cache_no_eol.cc", "a\nb");

    source_cache sources;
    CHECK(sources.line_count(path) == 2);
    CHECK(sources.raw_line(path, 2) == "b");

    remove_file(path);
}

TEST("source cache - an unreadable file is empty, not an error")
{
    source_cache sources;
    CHECK(sources.line_count("itrace_no_such_file_anywhere.cc") == 0);
    CHECK(sources.line("itrace_no_such_file_anywhere.cc", 1) == "");
}

TEST("source view - grows, merges near lines, keeps far lines separate")
{
    auto const path = write_fixture();

    // Touched: 2, 10, 12 (windows overlap → one range), and 25 (far → its own range).
    auto t = trace_of({at(path, 2), at(path, 10), at(path, 12), at(path, 25)});

    source_cache sources;
    auto const model = collect_source_view(t, sources, 5);

    REQUIRE(model.files.size() == 1);
    auto const& f = model.files[0];
    CHECK(f.path == path);
    REQUIRE(f.ranges.size() == 2);

    // [1..17] (2's window clamps low to 1; 12's window extends to 17), then [20..30].
    CHECK(f.ranges[0].start == 1);
    CHECK(f.ranges[0].end == 17);
    CHECK(f.ranges[1].start == 20);
    CHECK(f.ranges[1].end == 30);

    CHECK(f.ranges[0].lines.size() == 17);

    remove_file(path);
}

TEST("source view - marks executed lines and preserves indentation")
{
    auto const path = write_fixture();
    auto t = trace_of({at(path, 10), at(path, 12)});

    source_cache sources;
    auto const model = collect_source_view(t, sources, 5);
    REQUIRE(model.files.size() == 1);
    auto const& range = model.files[0].ranges[0];

    for (auto const& line : range.lines)
    {
        bool const should_run = line.number == 10 || line.number == 12;
        CHECK(line.executed == should_run);
        if (line.number == 10)
            CHECK(line.text == "    int indented = 10;"); // indentation preserved, unlike source_cache::line
    }

    remove_file(path);
}

TEST("source view - drops instructions without a source mapping")
{
    // No file / line 0 → nothing to show.
    auto t = trace_of({at("", 0), recorded_instruction{}});
    source_cache sources;
    auto const model = collect_source_view(t, sources, 5);
    CHECK(model.files.empty());
}
