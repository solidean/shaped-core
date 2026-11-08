#include <clean-core/bytes/hash128.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <nexus/run.hh>
#include <shaped-graphics-language/driver/prelude.hh>

using namespace cc::primitive_defines;

// `sgl`, the command line of SGL's toolchain: every job is a COMMAND of this one nexus binary.
// `sgl <command> …` runs the command named first with the rest of the line, and `sgl` alone lists what it holds.
// A new tool of the toolchain - a formatter, a linter, the language server - is one more file with one more COMMAND.
int main(int argc, char** argv)
{
    // Checked layers, one file per slot for every later run: a build of another compiler overwrites its slot rather than
    // adding a file, and the key is inside, so a file of another build or one cut short is a miss that checks instead.
    auto const path_of = [](cc::string_view name)
    {
        return cc::format("{}/sgl-{:016x}.layer", cc::temp_directory_path(), cc::hash128::create(name.as_bytes(), 0).low);
    };
    sgl::set_artifact_store({
        .load = [path_of](cc::string_view name) -> cc::optional<cc::vector<byte>>
        {
            auto opened = cc::file_read_stream_adapter::open(path_of(name));
            if (!opened.has_value())
                return {};
            cc::read_stream stream = opened.value().stream();
            auto bytes = stream.read_all();
            if (!bytes.has_value())
                return {};
            return cc::move(bytes.value());
        },
        .save =
            [path_of](cc::string_view name, cc::span<byte const> bytes)
        {
            auto opened = cc::file_write_stream_adapter::create(path_of(name));
            if (!opened.has_value())
                return;
            cc::write_stream stream = opened.value().stream();
            if (stream.write(bytes).has_value())
                (void)stream.flush();
        },
    });
    return nx::run(argc, argv);
}
