#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/utility.hh> // cc::move, cc::memcmp
#include <clean-core/container/byte_stream_builder.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh> // including it is what makes recorded_once_settled a coroutine
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh> // sgl::compiler_fingerprint
#include <shaped-graphics/binding/impl/shader_codec.hh>
#include <shaped-shader-library/impl/sgl_emit_cache.hh> // slib::impl::sgl_edge_fingerprint
#include <shaped-shader-library/impl/shader_bundle.hh>
#include <shaped-shader-library/shader_bundle.hh>

using namespace cc::primitive_defines;

// A bundle is a header, then records until the end: the key's low and high halves, the encoded shader's size, and its bytes.
// Every number is little-endian, as sg's shader codec writes its own.
// The same key may appear more than once, since a recording appends each compile it sees; the first one wins.

namespace
{
constexpr char k_magic[8] = {'s', 'l', 'i', 'b', 'b', 'n', 'd', 'l'};
constexpr u32 k_bundle_version = 1;
constexpr isize k_header_size = 8 + 4;
constexpr isize k_record_head_size = 8 + 8 + 4;

struct bundle_registry
{
    cc::vector<cc::span<byte const>> pending;
    cc::map<cc::hash128, cc::span<byte const>> shaders;
};

cc::mutex<bundle_registry>& registry()
{
    static cc::mutex<bundle_registry> instance = cc::mutex<bundle_registry>(bundle_registry());
    return instance;
}

u32 read_u32(byte const* p)
{
    return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24;
}

u64 read_u64(byte const* p)
{
    return u64(read_u32(p)) | u64(read_u32(p + 4)) << 32;
}

void push_u32(cc::vector<byte>& out, u32 v)
{
    for (auto i = 0; i < 4; ++i)
        out.push_back(byte(u8(v >> (8 * i))));
}

void push_u64(cc::vector<byte>& out, u64 v)
{
    push_u32(out, u32(v));
    push_u32(out, u32(v >> 32));
}

/// Files the records of `bytes` into `shaders`; false where the bundle is not one this build reads.
bool parse_bundle(cc::span<byte const> bytes, cc::map<cc::hash128, cc::span<byte const>>& shaders)
{
    if (bytes.size() < k_header_size || cc::memcmp(bytes.data(), k_magic, sizeof(k_magic)) != 0
        || read_u32(bytes.data() + 8) != k_bundle_version)
        return false;

    auto at = k_header_size;
    while (at < bytes.size())
    {
        if (bytes.size() - at < k_record_head_size)
            return false;
        auto const key = cc::hash128{.low = read_u64(bytes.data() + at), .high = read_u64(bytes.data() + at + 8)};
        auto const size = isize(read_u32(bytes.data() + at + 16));
        at += k_record_head_size;
        if (bytes.size() - at < size)
            return false;
        if (!shaders.contains(key))
            shaders[key] = bytes.subspan({.offset = at, .size = size});
        at += size;
    }
    return true;
}

/// The open recording, created on the first shader this process writes to it.
struct recording
{
    cc::string path;
    cc::unique_ptr<cc::file_write_stream_adapter> file;
    /// The one stream every record goes through: a view taken afresh starts at the beginning of the file again.
    cc::optional<cc::seekable_write_stream> stream;
    /// What was written already: two requests for one shader can both compile it, and a bundle needs it once.
    cc::set<cc::hash128> written;
    bool failed = false;
};

cc::vector<byte> bundle_header()
{
    auto header = cc::vector<byte>();
    for (auto const c : k_magic)
        header.push_back(byte(c));
    push_u32(header, k_bundle_version);
    return header;
}

cc::vector<byte> bundle_record(cc::hash128 key, sg::compiled_shader const& shader)
{
    auto const encoded = sg::impl::encode_compiled_shader(shader);
    auto record = cc::vector<byte>();
    push_u64(record, key.low);
    push_u64(record, key.high);
    push_u32(record, u32(encoded.size()));
    record.push_back_range(encoded);
    return record;
}

cc::mutex<recording>& the_recording()
{
    static cc::mutex<recording> instance = cc::mutex<recording>(recording{
        .path = cc::environment_variable("SC_SHADER_RECORD").value_or(cc::string()),
    });
    return instance;
}

void write_record(cc::hash128 key, sg::compiled_shader const& shader)
{
    the_recording().lock(
        [&](recording& r)
        {
            if (r.path.empty() || r.failed || !r.written.insert(key))
                return;
            if (r.file == nullptr)
            {
                // Created rather than appended to: a recording is what one run compiled, never a leftover of the last.
                auto created = cc::file_write_stream_adapter::create(r.path);
                if (created.has_error())
                {
                    CC_LOG_ERROR("cannot record shaders into '{}': {}", r.path, created.error().to_string());
                    r.failed = true;
                    return;
                }
                r.file = cc::make_unique<cc::file_write_stream_adapter>(cc::move(created.value()));
                r.stream = cc::seekable_write_stream(r.file->stream());
                (void)r.stream.value().write(bundle_header());
            }

            // Flushed per record, so a run that ends abruptly still leaves everything it compiled behind.
            auto& stream = r.stream.value();
            if (stream.write(bundle_record(key, shader)).has_error() || stream.flush().has_error())
            {
                CC_LOG_ERROR("recording a shader into '{}' failed; the recording stops here", r.path);
                r.failed = true;
            }
        });
}

sg::async_compiled_shader recorded_once_settled(cc::hash128 key, sg::async_compiled_shader built)
{
    auto shader = co_await built;
    write_record(key, shader);
    co_return shader;
}

/// `text` as a key holds it: without carriage returns.
void add_text(cc::byte_stream_builder& b, cc::string_view text)
{
    if (!text.contains('\r'))
    {
        b.add_string(text);
        return;
    }
    auto stripped = cc::string(text);
    stripped.replace_all("\r", "");
    b.add_string(stripped);
}
} // namespace

void slib::register_shader_bundle(cc::span<byte const> bytes)
{
    // Parsed on the first lookup rather than here, since this runs before main, where logging a broken bundle is not yet safe.
    if (!bytes.empty())
        registry().lock([&](bundle_registry& r) { r.pending.push_back(bytes); });
}

cc::hash128 slib::impl::bundle_key(shader_language language,
                                   sg::shader_format format,
                                   sg::shader_stage stage,
                                   cc::string_view entry_point,
                                   cc::span<shader_option const> options,
                                   cc::string_view host_namespace,
                                   cc::string_view source,
                                   cc::span<module_source const> modules)
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();

    // The compilers themselves, so a bundle from another commit misses rather than serving what they used to write.
    b.add_string(sgl::compiler_fingerprint());
    b.add_string(sgl_edge_fingerprint());

    b.add_pod(language);
    b.add_pod(format);
    b.add_pod(stage);
    b.add_string(entry_point);
    b.add_pod(u64(options.size()));
    for (auto const& o : options)
    {
        b.add_string(o.name);
        b.add_string(o.value);
    }
    b.add_string(host_namespace);
    add_text(b, source);

    // By path, since the disk and the embedded copy may list a directory in different orders.
    auto sorted = cc::vector<module_source>();
    sorted.push_back_range(modules);
    cc::sort_by(sorted, &module_source::path);
    b.add_pod(u64(sorted.size()));
    for (auto const& m : sorted)
    {
        b.add_string(m.path);
        add_text(b, m.text);
    }

    return cc::hash128::create(b.written_bytes(), 0);
}

cc::optional<sg::compiled_shader> slib::impl::find_bundled_shader(cc::hash128 key)
{
    auto const bytes = registry().lock(
        [&](bundle_registry& r) -> cc::optional<cc::span<byte const>>
        {
            for (auto const pending : r.pending)
                if (!parse_bundle(pending, r.shaders))
                    CC_LOG_ERROR("an embedded shader bundle does not parse, so the shaders it holds are compiled "
                                 "instead");
            r.pending.clear();

            if (auto const* found = r.shaders.get_ptr(key))
                return *found;
            return {};
        });
    if (!bytes.has_value())
        return {};

    // A record that does not decode is a miss: the codec refuses whatever it doubts, so it never serves a guess.
    return sg::impl::decode_compiled_shader(bytes.value());
}

bool slib::impl::is_recording_shaders()
{
    return the_recording().lock([](recording const& r) { return !r.path.empty() && !r.failed; });
}

void slib::impl::record_shader(cc::hash128 key, sg::compiled_shader const& shader)
{
    write_record(key, shader);
}

sg::async_compiled_shader slib::impl::recorded_shader(cc::hash128 key, sg::async_compiled_shader built)
{
    return recorded_once_settled(key, cc::move(built));
}

cc::vector<byte> slib::impl::make_bundle(cc::hash128 key, sg::compiled_shader const& shader)
{
    auto bytes = bundle_header();
    bytes.push_back_range(bundle_record(key, shader));
    return bytes;
}
