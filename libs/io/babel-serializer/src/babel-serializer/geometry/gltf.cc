#include <babel-serializer/data/base64.hh>
#include <babel-serializer/data/json.hh>
#include <babel-serializer/geometry/gltf.hh>
#include <clean-core/common/utility.hh> // cc::move, cc::unit, cc::memcpy
#include <clean-core/error/optional.hh>
#include <clean-core/string/format.hh>

// The parse is two stages, and the split is what makes the zero-copy promise work:
// the container stage only locates the JSON chunk and the BIN chunk inside the input bytes, then the document
// stage reads the JSON with babel::json and turns every `buffers` entry into a subview of those same bytes.
//
// Order matters in the document stage: buffers resolve first (they depend on nothing), bufferViews next,
// then every remaining array is recorded, then a single pass validates every stored index, and only then are
// the images resolved — an image may sit inside a bufferView, so its bytes need a validated chain.

namespace babel::impl
{
namespace
{
constexpr u32 glb_magic = 0x46546C67;      // 'g','l','T','F' little-endian
constexpr u32 glb_chunk_json = 0x4E4F534A; // 'J','S','O','N'
constexpr u32 glb_chunk_bin = 0x004E4942;  // 'B','I','N',0

/// Read a little-endian u32 at `offset`; the caller guarantees 4 readable bytes.
/// Byte-wise on purpose: the input pin's alignment is not guaranteed, and this costs nothing after inlining.
u32 load_le_u32(cc::span<byte const> bytes, isize offset)
{
    auto const b0 = u32(u8(bytes[offset + 0]));
    auto const b1 = u32(u8(bytes[offset + 1]));
    auto const b2 = u32(u8(bytes[offset + 2]));
    auto const b3 = u32(u8(bytes[offset + 3]));
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

// JSON member readers. All of them are kind-tolerant by design: a required property is checked explicitly at
// its use site, and everything else falls back rather than failing, which is what makes exporter junk harmless.

cc::string string_member(json::ref obj, cc::string_view key)
{
    return cc::string(obj[key].as_string());
}

i64 int_member(json::ref obj, cc::string_view key, i64 fallback = 0)
{
    auto const member = obj[key];
    return member.is_number() ? i64(member.as_double()) : fallback;
}

f32 float_member(json::ref obj, cc::string_view key, f32 fallback)
{
    auto const member = obj[key];
    return member.is_number() ? f32(member.as_double()) : fallback;
}

bool bool_member(json::ref obj, cc::string_view key, bool fallback = false)
{
    auto const member = obj[key];
    return member.is_bool() ? member.as_bool() : fallback;
}

/// A glTF cross-reference: an integer member read as a typed index, `invalid` when absent or not a number.
template <class Index>
Index index_member(json::ref obj, cc::string_view key)
{
    auto const member = obj[key];
    return member.is_number() ? Index(int(member.as_double())) : Index::invalid;
}

tg::vec3f vec3_member(json::ref obj, cc::string_view key, tg::vec3f fallback)
{
    auto const member = obj[key];
    if (!member.is_array() || member.size() != 3)
        return fallback;
    return tg::vec3f(f32(member[0].as_double()), f32(member[1].as_double()), f32(member[2].as_double()));
}

tg::vec4f vec4_member(json::ref obj, cc::string_view key, tg::vec4f fallback)
{
    auto const member = obj[key];
    if (!member.is_array() || member.size() != 4)
        return fallback;
    return tg::vec4f(f32(member[0].as_double()), f32(member[1].as_double()), f32(member[2].as_double()),
                     f32(member[3].as_double()));
}

/// glTF stores a rotation as [x, y, z, w], which is tg::quat's own component order.
tg::quat_f quat_member(json::ref obj, cc::string_view key, tg::quat_f fallback)
{
    auto const member = obj[key];
    if (!member.is_array() || member.size() != 4)
        return fallback;
    return tg::quat_f(f32(member[0].as_double()), f32(member[1].as_double()), f32(member[2].as_double()),
                      f32(member[3].as_double()));
}

void collect_strings(json::ref array, cc::vector<cc::string>& out)
{
    if (!array.is_array())
        return;
    for (auto i = isize(0); i < array.size(); ++i)
        if (array[i].is_string())
            out.push_back(cc::string(array[i].as_string()));
}

// Enum mappers. The ones that decide how bytes are interpreted fail on an unknown value; the cosmetic ones
// below hand back a nullopt instead, because a sampler we do not recognize still leaves the geometry readable.

cc::result<gltf::component_type> component_type_from(i64 value)
{
    switch (value)
    {
    case 5120:
        return gltf::component_type::i8;
    case 5121:
        return gltf::component_type::u8;
    case 5122:
        return gltf::component_type::i16;
    case 5123:
        return gltf::component_type::u16;
    case 5125:
        return gltf::component_type::u32;
    case 5126:
        return gltf::component_type::f32;
    default:
        return cc::error(cc::format("glTF parse error: unknown accessor componentType {}", value));
    }
}

cc::result<gltf::accessor_type> accessor_type_from(cc::string_view spelled)
{
    if (spelled == "SCALAR")
        return gltf::accessor_type::scalar;
    if (spelled == "VEC2")
        return gltf::accessor_type::vec2;
    if (spelled == "VEC3")
        return gltf::accessor_type::vec3;
    if (spelled == "VEC4")
        return gltf::accessor_type::vec4;
    if (spelled == "MAT2")
        return gltf::accessor_type::mat2;
    if (spelled == "MAT3")
        return gltf::accessor_type::mat3;
    if (spelled == "MAT4")
        return gltf::accessor_type::mat4;
    return cc::error(cc::format("glTF parse error: unknown accessor type '{}'", spelled));
}

cc::result<gltf::primitive_mode> primitive_mode_from(i64 value)
{
    if (value < 0 || value > 6)
        return cc::error(cc::format("glTF parse error: unknown primitive mode {}", value));
    return gltf::primitive_mode(u8(value));
}

gltf::buffer_target buffer_target_from(i64 value)
{
    switch (value)
    {
    case 34962:
        return gltf::buffer_target::array_buffer;
    case 34963:
        return gltf::buffer_target::element_array_buffer;
    default:
        return gltf::buffer_target::none;
    }
}

// The cosmetic three hand back a nullopt for an unrecognized value rather than quietly defaulting: the call
// site knows which element it is reading, so it is the only place that can record a useful issue.

cc::optional<gltf::alpha_mode> alpha_mode_from(cc::string_view spelled)
{
    if (spelled == "OPAQUE")
        return gltf::alpha_mode::opaque;
    if (spelled == "MASK")
        return gltf::alpha_mode::mask;
    if (spelled == "BLEND")
        return gltf::alpha_mode::blend;
    return cc::nullopt;
}

cc::optional<gltf::filter> filter_from(i64 value)
{
    switch (value)
    {
    case 9728:
        return gltf::filter::nearest;
    case 9729:
        return gltf::filter::linear;
    case 9984:
        return gltf::filter::nearest_mipmap_nearest;
    case 9985:
        return gltf::filter::linear_mipmap_nearest;
    case 9986:
        return gltf::filter::nearest_mipmap_linear;
    case 9987:
        return gltf::filter::linear_mipmap_linear;
    default:
        return cc::nullopt;
    }
}

cc::optional<gltf::wrap_mode> wrap_mode_from(i64 value)
{
    switch (value)
    {
    case 10497:
        return gltf::wrap_mode::repeat;
    case 33071:
        return gltf::wrap_mode::clamp_to_edge;
    case 33648:
        return gltf::wrap_mode::mirrored_repeat;
    default:
        return cc::nullopt;
    }
}

gltf::texture_ref texture_ref_member(json::ref obj, cc::string_view key)
{
    auto out = gltf::texture_ref();
    auto const member = obj[key];
    if (!member.is_object())
        return out;
    out.texture = index_member<gltf::texture_index>(member, "index");
    out.texcoord = i32(int_member(member, "texCoord"));
    return out;
}

bool is_data_uri(cc::string_view uri)
{
    return uri.starts_with("data:");
}

/// Decode a `data:...;base64,...` URI into an owned pin.
/// glTF 2.0 allows only the base64 form, so anything else is an error rather than a silent skip.
cc::result<cc::pinned_data<byte const>> decode_data_uri(cc::string_view uri)
{
    auto const marker = cc::string_view(";base64,");
    auto const at = uri.find(marker);
    if (at < 0)
        return cc::error("glTF parse error: only base64-encoded data URIs are supported");

    auto payload = base64::decode(uri.subview(at + marker.size()));
    CC_RETURN_IF_ERROR(payload).with_context("while decoding a glTF data URI");

    // make_pinned_data moves the decoded vector into the pin, so the base64 decode is the only copy.
    return cc::pinned_data<byte const>(cc::make_pinned_data(cc::move(payload).value()));
}

struct gltf_parser
{
    cc::pinned_data<byte const> input;
    gltf::read_options opts;

    cc::pinned_data<byte const> bin_chunk; // the GLB BIN chunk; empty for a .gltf
    bool has_bin_chunk = false;

    gltf::data result;

    // issues
public:
    /// Record something skipped, unresolvable or tolerated. Never fails the read — that is what cc::error is for.
    void add_issue(gltf::issue_kind kind, cc::string message)
    {
        result.issues.push_back({.kind = kind, .message = cc::move(message)});
    }

    /// A sampler / material enumerant the reader does not know: fall back and say so.
    /// An absent member is not an issue, which is why these check `is_number` / emptiness first.
    gltf::filter filter_member(json::ref obj, cc::string_view key, isize sampler)
    {
        auto const member = obj[key];
        if (!member.is_number())
            return gltf::filter::none;

        auto const mapped = filter_from(i64(member.as_double()));
        if (!mapped.has_value())
        {
            add_issue(gltf::issue_kind::malformed, cc::format("sampler {}: unknown {} value {}, left unspecified",
                                                              sampler, key, i64(member.as_double())));
            return gltf::filter::none;
        }
        return mapped.value();
    }

    gltf::wrap_mode wrap_member(json::ref obj, cc::string_view key, isize sampler)
    {
        auto const member = obj[key];
        if (!member.is_number())
            return gltf::wrap_mode::repeat;

        auto const mapped = wrap_mode_from(i64(member.as_double()));
        if (!mapped.has_value())
        {
            add_issue(gltf::issue_kind::malformed, cc::format("sampler {}: unknown {} value {}, defaulted to repeat",
                                                              sampler, key, i64(member.as_double())));
            return gltf::wrap_mode::repeat;
        }
        return mapped.value();
    }

    gltf::alpha_mode alpha_mode_member(json::ref obj, isize material)
    {
        auto const member = obj["alphaMode"];
        if (!member.is_string() || member.as_string().empty())
            return gltf::alpha_mode::opaque;

        auto const mapped = alpha_mode_from(member.as_string());
        if (!mapped.has_value())
        {
            add_issue(
                gltf::issue_kind::malformed,
                cc::format("material {}: unknown alphaMode '{}', defaulted to OPAQUE", material, member.as_string()));
            return gltf::alpha_mode::opaque;
        }
        return mapped.value();
    }

    /// Note an array the reader parses over without modelling: `skins`, `animations`, `cameras`.
    void note_unmodelled_array(json::ref root, cc::string_view key, cc::string_view what)
    {
        auto const array = root[key];
        if (array.is_array() && array.size() > 0)
            add_issue(gltf::issue_kind::unsupported,
                      cc::format("{} {} not read: `{}` is not implemented", array.size(), what, key));
    }

    // container
public:
    cc::result<gltf::data> parse()
    {
        auto json_bytes = input.span();
        result.source = gltf::detect_container(json_bytes);

        if (result.source == gltf::container::glb)
        {
            auto chunk = split_glb();
            CC_RETURN_IF_ERROR(chunk);
            json_bytes = chunk.value();
        }

        CC_RETURN_IF_ERROR(parse_document(json_bytes));
        return cc::move(result);
    }

    /// Walk the GLB chunks, returning the JSON chunk and recording the BIN chunk as a subview of the input.
    /// Chunk padding lives inside the chunk length, and an unknown chunk type is an extension we skip.
    cc::result<cc::span<byte const>> split_glb()
    {
        auto const all = input.span();
        if (all.size() < 12)
            return cc::error("GLB parse error: the input is shorter than the 12-byte header");

        if (load_le_u32(all, 0) != glb_magic)
            return cc::error("GLB parse error: bad magic (not a GLB)");

        auto const version = load_le_u32(all, 4);
        if (version != 2)
            return cc::error(cc::format("GLB parse error: unsupported container version {}", version));

        auto const declared = i64(load_le_u32(all, 8));
        if (declared < 12)
            return cc::error(cc::format("GLB parse error: declared length {} does not cover the header", declared));
        if (declared > all.size())
            return cc::error(
                cc::format("GLB parse error: declared length {} exceeds the {} bytes given", declared, all.size()));

        auto json_chunk = cc::span<byte const>();
        auto found_json = false;
        auto offset = i64(12);

        while (offset < declared)
        {
            if (offset + 8 > declared)
                return cc::error(cc::format("GLB parse error at offset {}: the chunk header does not fit", offset));

            auto const length = i64(load_le_u32(all, offset));
            auto const type = load_le_u32(all, offset + 4);
            auto const start = offset + 8;
            if (start + length > declared)
                return cc::error(cc::format(
                    "GLB parse error at offset {}: a chunk of {} bytes runs past the declared length", offset, length));

            if (offset == 12 && type != glb_chunk_json)
                return cc::error("GLB parse error: the first chunk must be the JSON chunk");

            // The spec pads every chunk to 4 bytes from inside its own length. Exporters get this wrong, and it
            // costs us nothing as long as the walk stays in bounds — so it is an issue, not an error.
            if (length % 4 != 0)
                add_issue(gltf::issue_kind::malformed,
                          cc::format("GLB chunk at offset {} has an unpadded length of {} bytes", offset, length));

            if (type == glb_chunk_json)
            {
                if (found_json)
                    return cc::error("GLB parse error: more than one JSON chunk");
                found_json = true;
                json_chunk = all.subspan({.offset = start, .size = length});
            }
            else if (type == glb_chunk_bin)
            {
                if (has_bin_chunk)
                    return cc::error("GLB parse error: more than one BIN chunk");
                has_bin_chunk = true;
                bin_chunk = input.subdata({.offset = start, .size = length});
            }
            else
            {
                // The spec mandates skipping an unknown chunk type — it is an extension we do not implement.
                add_issue(gltf::issue_kind::unsupported, cc::format("GLB chunk of unknown type 0x{:08x} at offset {} "
                                                                    "skipped ({} bytes)",
                                                                    type, offset, length));
            }

            offset = start + length;
        }

        if (!found_json)
            return cc::error("GLB parse error: no JSON chunk");
        return json_chunk;
    }

    // document
public:
    cc::result<cc::unit> parse_document(cc::span<byte const> json_bytes)
    {
        // The span stream babel::json wraps around this is unbuffered, so the parse runs against the input bytes.
        auto parsed = json::read(json_bytes);
        CC_RETURN_IF_ERROR(parsed).with_context("while parsing the glTF JSON");

        auto const document = cc::move(parsed).value();
        auto const root = document.root();
        if (!root.is_object())
            return cc::error("glTF parse error: the document root must be a JSON object");

        CC_RETURN_IF_ERROR(parse_asset(root));

        collect_strings(root["extensionsUsed"], result.extensions_used);
        collect_strings(root["extensionsRequired"], result.extensions_required);
        if (!result.extensions_required.empty())
            return cc::error(cc::format("glTF parse error: the file requires extension '{}', which this reader does "
                                        "not implement",
                                        result.extensions_required[0]));

        // This reader interprets no extension at all, so every one the file uses is one we drop on the floor.
        for (auto const& extension : result.extensions_used)
            add_issue(gltf::issue_kind::unsupported,
                      cc::format("extension '{}' is used by the file but not interpreted", extension));

        note_unmodelled_array(root, "skins", "skins");
        note_unmodelled_array(root, "animations", "animations");
        note_unmodelled_array(root, "cameras", "cameras");

        CC_RETURN_IF_ERROR(parse_buffers(root));
        CC_RETURN_IF_ERROR(parse_buffer_views(root));
        CC_RETURN_IF_ERROR(parse_accessors(root));
        CC_RETURN_IF_ERROR(parse_meshes(root));
        parse_nodes(root);
        parse_scenes(root);
        parse_materials(root);
        parse_textures(root);
        parse_images(root);
        parse_samplers(root);

        CC_RETURN_IF_ERROR(validate_indices());
        CC_RETURN_IF_ERROR(resolve_images());
        return cc::unit{};
    }

    cc::result<cc::unit> parse_asset(json::ref root)
    {
        auto const asset = root["asset"];
        if (!asset.is_object())
            return cc::error("glTF parse error: the required `asset` object is missing");

        auto const version = asset["version"];
        if (!version.is_string())
            return cc::error("glTF parse error: the required `asset.version` is missing");

        auto const spelled = version.as_string();
        result.asset.version = cc::string(spelled);
        result.asset.min_version = string_member(asset, "minVersion");
        result.asset.generator = string_member(asset, "generator");
        result.asset.copyright = string_member(asset, "copyright");

        // Only the major version decides whether this reader can interpret the file at all.
        auto major = 0;
        auto digits = isize(0);
        while (digits < spelled.size() && spelled[digits] >= '0' && spelled[digits] <= '9')
        {
            major = major * 10 + (spelled[digits] - '0');
            ++digits;
        }
        if (digits == 0 || major != 2)
            return cc::error(cc::format("glTF parse error: asset version '{}' is not glTF 2.x", spelled));

        return cc::unit{};
    }

    cc::result<cc::unit> parse_buffers(json::ref root)
    {
        auto const buffers = root["buffers"];
        if (!buffers.is_array())
            return cc::unit{};

        for (auto i = isize(0); i < buffers.size(); ++i)
        {
            auto const entry = buffers[i];
            auto buf = gltf::buffer();
            buf.uri = string_member(entry, "uri");
            buf.byte_length = int_member(entry, "byteLength");
            if (buf.byte_length < 0)
                return cc::error(cc::format("glTF parse error: buffer {} declares a negative byteLength", i));

            if (buf.uri.empty())
            {
                // No URI is legal only for the GLB BIN chunk, which the spec binds to buffer 0.
                if (!has_bin_chunk)
                    return cc::error(
                        cc::format("glTF parse error: buffer {} has no uri and the file has no GLB BIN chunk", i));
                if (i != 0)
                    return cc::error(cc::format(
                        "glTF parse error: buffer {} has no uri, but only buffer 0 may use the BIN chunk", i));
                CC_RETURN_IF_ERROR(adopt_bytes(buf, bin_chunk, "the GLB BIN chunk"));
            }
            else if (is_data_uri(buf.uri))
            {
                auto bytes = decode_data_uri(buf.uri);
                CC_RETURN_IF_ERROR(bytes).with_context(cc::format("while resolving buffer {}", i));
                CC_RETURN_IF_ERROR(adopt_bytes(buf, cc::move(bytes).value(), "the data URI"));
            }
            else if (opts.resolve_uri.is_valid())
            {
                auto bytes = opts.resolve_uri(buf.uri);
                CC_RETURN_IF_ERROR(bytes).with_context(cc::format("while resolving buffer {} from uri '{}'", i, buf.uri));
                CC_RETURN_IF_ERROR(adopt_bytes(buf, cc::move(bytes).value(), "the resolved uri"));
            }
            else
            {
                // An external URI with no resolver: the reference is recorded, the bytes are simply absent.
                add_issue(gltf::issue_kind::unresolved, cc::format("buffer {} not loaded: uri '{}' needs "
                                                                   "read_options::resolve_uri ({} bytes)",
                                                                   i, buf.uri, buf.byte_length));
            }

            result.buffers.push_back(cc::move(buf));
        }
        return cc::unit{};
    }

    /// Take resolved bytes for a buffer: they must cover byteLength, and are trimmed to exactly it.
    /// The trim is what a GLB needs — its BIN chunk may carry up to 3 padding bytes past the buffer's end —
    /// and it is what makes `resolved` imply `data.size() == byte_length`.
    static cc::result<cc::unit> adopt_bytes(gltf::buffer& buf,
                                            cc::pinned_data<byte const> const& bytes,
                                            cc::string_view source)
    {
        if (bytes.size() < buf.byte_length)
            return cc::error(cc::format("glTF parse error: {} holds {} bytes, the buffer declares {}", source,
                                        bytes.size(), buf.byte_length));

        buf.data = bytes.subdata({.offset = 0, .size = buf.byte_length});
        buf.resolved = true;
        return cc::unit{};
    }

    cc::result<cc::unit> parse_buffer_views(json::ref root)
    {
        auto const views = root["bufferViews"];
        if (!views.is_array())
            return cc::unit{};

        for (auto i = isize(0); i < views.size(); ++i)
        {
            auto const entry = views[i];
            auto view = gltf::buffer_view();
            view.buffer = index_member<gltf::buffer_index>(entry, "buffer");
            view.byte_offset = int_member(entry, "byteOffset");
            view.byte_length = int_member(entry, "byteLength");
            view.byte_stride = int_member(entry, "byteStride");
            view.target = buffer_target_from(int_member(entry, "target"));
            view.name = string_member(entry, "name");

            if (view.byte_offset < 0 || view.byte_length < 0 || view.byte_stride < 0)
                return cc::error(cc::format("glTF parse error: bufferView {} has a negative offset, length or stride", i));

            result.buffer_views.push_back(cc::move(view));
        }
        return cc::unit{};
    }

    cc::result<cc::unit> parse_accessors(json::ref root)
    {
        auto const accessors = root["accessors"];
        if (!accessors.is_array())
            return cc::unit{};

        for (auto i = isize(0); i < accessors.size(); ++i)
        {
            auto const entry = accessors[i];

            // A sparse accessor overrides individual elements, so ignoring it hands back the wrong data.
            if (entry.has("sparse"))
                return cc::error(
                    cc::format("glTF parse error: accessor {} is sparse, which this reader does not implement", i));

            auto acc = gltf::accessor();
            acc.buffer_view = index_member<gltf::buffer_view_index>(entry, "bufferView");
            acc.byte_offset = int_member(entry, "byteOffset");

            if (!entry["componentType"].is_number())
                return cc::error(cc::format("glTF parse error: accessor {} is missing its componentType", i));
            auto component = component_type_from(int_member(entry, "componentType"));
            CC_RETURN_IF_ERROR(component);
            acc.component = component.value();

            auto type = accessor_type_from(entry["type"].as_string());
            CC_RETURN_IF_ERROR(type).with_context(cc::format("in accessor {}", i));
            acc.type = type.value();

            acc.count = int_member(entry, "count");
            acc.normalized = bool_member(entry, "normalized");
            if (acc.byte_offset < 0 || acc.count < 0)
                return cc::error(cc::format("glTF parse error: accessor {} has a negative offset or count", i));

            // min / max are only kept when both are present and fully dimensioned — a half-stated bound is no bound.
            auto const mins = entry["min"];
            auto const maxs = entry["max"];
            auto const components = isize(acc.component_count());
            if (mins.is_array() && maxs.is_array() && mins.size() == components && maxs.size() == components)
            {
                acc.first_bound = i32(result.accessor_bounds.size());
                acc.bound_count = i32(2 * components);
                for (auto k = isize(0); k < components; ++k)
                    result.accessor_bounds.push_back(f32(mins[k].as_double()));
                for (auto k = isize(0); k < components; ++k)
                    result.accessor_bounds.push_back(f32(maxs[k].as_double()));
            }
            else if (mins.is_valid() || maxs.is_valid())
            {
                add_issue(gltf::issue_kind::malformed, cc::format("accessor {}: min / max are not both stated with {} "
                                                                  "components, bounds ignored",
                                                                  i, components));
            }

            acc.name = string_member(entry, "name");
            result.accessors.push_back(cc::move(acc));
        }
        return cc::unit{};
    }

    cc::result<cc::unit> parse_meshes(json::ref root)
    {
        auto const meshes = root["meshes"];
        if (!meshes.is_array())
            return cc::unit{};

        for (auto i = isize(0); i < meshes.size(); ++i)
        {
            auto const entry = meshes[i];
            auto msh = gltf::mesh();
            msh.first_primitive = i32(result.primitives.size());

            auto const primitives = entry["primitives"];
            for (auto p = isize(0); primitives.is_array() && p < primitives.size(); ++p)
            {
                auto const prim_entry = primitives[p];
                auto prim = gltf::primitive();
                prim.first_attribute = i32(result.attributes.size());

                auto const attributes = prim_entry["attributes"];
                for (auto a = isize(0); attributes.is_object() && a < attributes.size(); ++a)
                {
                    auto const member = attributes[a];
                    auto attr = gltf::attribute();
                    attr.semantic = cc::string(member.key());
                    attr.accessor = member.is_number() ? gltf::accessor_index(int(member.as_double()))
                                                       : gltf::accessor_index::invalid;
                    result.attributes.push_back(cc::move(attr));
                }
                prim.attribute_count = i32(result.attributes.size()) - prim.first_attribute;

                prim.indices = index_member<gltf::accessor_index>(prim_entry, "indices");
                prim.material = index_member<gltf::material_index>(prim_entry, "material");

                auto mode = primitive_mode_from(int_member(prim_entry, "mode", 4));
                CC_RETURN_IF_ERROR(mode).with_context(cc::format("in mesh {} primitive {}", i, p));
                prim.mode = mode.value();

                // Morph targets are additive: skipping them still renders the base mesh correctly.
                auto const targets = prim_entry["targets"];
                if (targets.is_array() && targets.size() > 0)
                    add_issue(gltf::issue_kind::unsupported, cc::format("mesh {} primitive {}: {} morph target(s) "
                                                                        "skipped, `targets` is not implemented",
                                                                        i, p, targets.size()));

                result.primitives.push_back(prim);
            }
            msh.primitive_count = i32(result.primitives.size()) - msh.first_primitive;

            msh.name = string_member(entry, "name");
            result.meshes.push_back(cc::move(msh));
        }
        return cc::unit{};
    }

    void parse_nodes(json::ref root)
    {
        auto const nodes = root["nodes"];
        if (!nodes.is_array())
            return;

        for (auto i = isize(0); i < nodes.size(); ++i)
        {
            auto const entry = nodes[i];
            auto nod = gltf::node();
            nod.first_child = i32(result.node_children.size());

            auto const children = entry["children"];
            for (auto c = isize(0); children.is_array() && c < children.size(); ++c)
                if (children[c].is_number())
                    result.node_children.push_back(gltf::node_index(int(children[c].as_double())));
            nod.child_count = i32(result.node_children.size()) - nod.first_child;

            nod.mesh = index_member<gltf::mesh_index>(entry, "mesh");

            // `matrix` and TRS are mutually exclusive in the spec; whichever the file used is what we keep.
            auto const matrix = entry["matrix"];
            if (matrix.is_array() && matrix.size() == 16)
            {
                nod.has_matrix = true;
                // glTF lists the matrix column by column, which is tg::mat's own storage order.
                for (auto c = 0; c < 4; ++c)
                    for (auto r = 0; r < 4; ++r)
                        nod.matrix[c, r] = f32(matrix[isize(c) * 4 + isize(r)].as_double());
            }
            else
            {
                nod.translation = vec3_member(entry, "translation", tg::vec3f::zero);
                nod.rotation = quat_member(entry, "rotation", tg::quat_f::identity);
                nod.scale = vec3_member(entry, "scale", tg::vec3f(1, 1, 1));
            }

            nod.name = string_member(entry, "name");
            result.nodes.push_back(cc::move(nod));
        }
    }

    void parse_scenes(json::ref root)
    {
        result.default_scene = index_member<gltf::scene_index>(root, "scene");

        auto const scenes = root["scenes"];
        if (!scenes.is_array())
            return;

        for (auto i = isize(0); i < scenes.size(); ++i)
        {
            auto const entry = scenes[i];
            auto scn = gltf::scene();
            scn.first_node = i32(result.scene_nodes.size());

            auto const nodes = entry["nodes"];
            for (auto n = isize(0); nodes.is_array() && n < nodes.size(); ++n)
                if (nodes[n].is_number())
                    result.scene_nodes.push_back(gltf::node_index(int(nodes[n].as_double())));
            scn.node_count = i32(result.scene_nodes.size()) - scn.first_node;

            scn.name = string_member(entry, "name");
            result.scenes.push_back(cc::move(scn));
        }
    }

    void parse_materials(json::ref root)
    {
        auto const materials = root["materials"];
        if (!materials.is_array())
            return;

        for (auto i = isize(0); i < materials.size(); ++i)
        {
            auto const entry = materials[i];
            auto mat = gltf::material();

            auto const pbr = entry["pbrMetallicRoughness"];
            mat.base_color_factor = vec4_member(pbr, "baseColorFactor", tg::vec4f(1, 1, 1, 1));
            mat.metallic_factor = float_member(pbr, "metallicFactor", 1);
            mat.roughness_factor = float_member(pbr, "roughnessFactor", 1);
            mat.base_color_texture = texture_ref_member(pbr, "baseColorTexture");
            mat.metallic_roughness_texture = texture_ref_member(pbr, "metallicRoughnessTexture");

            mat.normal_texture = texture_ref_member(entry, "normalTexture");
            mat.normal_scale = float_member(entry["normalTexture"], "scale", 1);
            mat.occlusion_texture = texture_ref_member(entry, "occlusionTexture");
            mat.occlusion_strength = float_member(entry["occlusionTexture"], "strength", 1);
            mat.emissive_texture = texture_ref_member(entry, "emissiveTexture");
            mat.emissive_factor = vec3_member(entry, "emissiveFactor", tg::vec3f::zero);

            mat.alpha = alpha_mode_member(entry, i);
            mat.alpha_cutoff = float_member(entry, "alphaCutoff", 0.5f);
            mat.double_sided = bool_member(entry, "doubleSided");

            mat.name = string_member(entry, "name");
            result.materials.push_back(cc::move(mat));
        }
    }

    void parse_textures(json::ref root)
    {
        auto const textures = root["textures"];
        if (!textures.is_array())
            return;

        for (auto i = isize(0); i < textures.size(); ++i)
        {
            auto const entry = textures[i];
            auto tex = gltf::texture();
            tex.sampler = index_member<gltf::sampler_index>(entry, "sampler");
            tex.source = index_member<gltf::image_index>(entry, "source");
            tex.name = string_member(entry, "name");
            result.textures.push_back(cc::move(tex));
        }
    }

    void parse_images(json::ref root)
    {
        auto const images = root["images"];
        if (!images.is_array())
            return;

        for (auto i = isize(0); i < images.size(); ++i)
        {
            auto const entry = images[i];
            auto img = gltf::image();
            img.uri = string_member(entry, "uri");
            img.mime_type = string_member(entry, "mimeType");
            img.buffer_view = index_member<gltf::buffer_view_index>(entry, "bufferView");
            img.name = string_member(entry, "name");
            result.images.push_back(cc::move(img));
        }
    }

    void parse_samplers(json::ref root)
    {
        auto const samplers = root["samplers"];
        if (!samplers.is_array())
            return;

        for (auto i = isize(0); i < samplers.size(); ++i)
        {
            auto const entry = samplers[i];
            auto smp = gltf::sampler();
            smp.mag_filter = filter_member(entry, "magFilter", i);
            smp.min_filter = filter_member(entry, "minFilter", i);
            smp.wrap_s = wrap_member(entry, "wrapS", i);
            smp.wrap_t = wrap_member(entry, "wrapT", i);
            smp.name = string_member(entry, "name");
            result.samplers.push_back(cc::move(smp));
        }
    }

    // validation
public:
    /// Every stored index is either `invalid` or in range once this returns, which is exactly what lets
    /// data::find report nullptr only for "the file left this property out".
    /// The range checks also guard the pinned_data::subdata calls below — subdata asserts, it does not fail.
    cc::result<cc::unit> validate_indices()
    {
        auto const buffer_count = result.buffers.size();
        auto const view_count = result.buffer_views.size();
        auto const accessor_count = result.accessors.size();
        auto const mesh_count = result.meshes.size();
        auto const node_count = result.nodes.size();
        auto const scene_count = result.scenes.size();
        auto const material_count = result.materials.size();
        auto const texture_count = result.textures.size();
        auto const image_count = result.images.size();
        auto const sampler_count = result.samplers.size();

        for (auto i = isize(0); i < view_count; ++i)
        {
            auto const& view = result.buffer_views[i];
            auto const raw = isize(int(view.buffer));
            if (raw < 0)
                return cc::error(cc::format("glTF parse error: bufferView {} is missing its required buffer", i));
            if (raw >= buffer_count)
                return cc::error(cc::format("glTF parse error: bufferView {} references buffer {}, but the file has {}",
                                            i, raw, buffer_count));
            if (view.byte_offset + view.byte_length > result.buffers[raw].byte_length)
                return cc::error(
                    cc::format("glTF parse error: bufferView {} spans [{}, {}) of a buffer that declares {} bytes", i,
                               view.byte_offset, view.byte_offset + view.byte_length, result.buffers[raw].byte_length));
        }

        for (auto i = isize(0); i < accessor_count; ++i)
        {
            auto const& acc = result.accessors[i];
            auto const raw = isize(int(acc.buffer_view));
            if (raw >= view_count)
                return cc::error(cc::format(
                    "glTF parse error: accessor {} references bufferView {}, but the file has {}", i, raw, view_count));
            if (raw < 0)
                continue; // no bufferView: the spec's implicit all-zero data, which has nothing to bound

            auto const& view = result.buffer_views[raw];
            auto const element_size = acc.element_size();
            auto const stride = view.byte_stride > 0 ? view.byte_stride : element_size;
            auto const needed = acc.count > 0 ? (acc.count - 1) * stride + element_size : i64(0);
            if (acc.byte_offset + needed > view.byte_length)
                return cc::error(cc::format("glTF parse error: accessor {} needs {} bytes at offset {} of a bufferView "
                                            "that is {} bytes long",
                                            i, needed, acc.byte_offset, view.byte_length));
        }

        for (auto i = isize(0); i < result.attributes.size(); ++i)
        {
            auto const raw = isize(int(result.attributes[i].accessor));
            if (raw < 0 || raw >= accessor_count)
                return cc::error(cc::format("glTF parse error: attribute '{}' references accessor {}, but the file has "
                                            "{}",
                                            result.attributes[i].semantic, raw, accessor_count));
        }

        for (auto i = isize(0); i < result.primitives.size(); ++i)
        {
            auto const& prim = result.primitives[i];
            if (isize(int(prim.indices)) >= accessor_count)
                return cc::error(cc::format("glTF parse error: primitive {} references index accessor {}, but the file "
                                            "has {}",
                                            i, isize(int(prim.indices)), accessor_count));
            if (isize(int(prim.material)) >= material_count)
                return cc::error(cc::format("glTF parse error: primitive {} references material {}, but the file has "
                                            "{}",
                                            i, isize(int(prim.material)), material_count));
        }

        for (auto i = isize(0); i < node_count; ++i)
        {
            auto const& nod = result.nodes[i];
            if (isize(int(nod.mesh)) >= mesh_count)
                return cc::error(cc::format("glTF parse error: node {} references mesh {}, but the file has {}", i,
                                            isize(int(nod.mesh)), mesh_count));
        }

        for (auto const child : result.node_children)
            if (isize(int(child)) >= node_count)
                return cc::error(cc::format("glTF parse error: a node lists child {}, but the file has {} nodes",
                                            isize(int(child)), node_count));

        for (auto const root_node : result.scene_nodes)
            if (isize(int(root_node)) >= node_count)
                return cc::error(cc::format("glTF parse error: a scene lists node {}, but the file has {}",
                                            isize(int(root_node)), node_count));

        if (isize(int(result.default_scene)) >= scene_count)
            return cc::error(cc::format("glTF parse error: `scene` is {}, but the file has {} scenes",
                                        isize(int(result.default_scene)), scene_count));

        for (auto i = isize(0); i < texture_count; ++i)
        {
            auto const& tex = result.textures[i];
            if (isize(int(tex.source)) >= image_count)
                return cc::error(cc::format("glTF parse error: texture {} references image {}, but the file has {}", i,
                                            isize(int(tex.source)), image_count));
            if (isize(int(tex.sampler)) >= sampler_count)
                return cc::error(cc::format("glTF parse error: texture {} references sampler {}, but the file has {}",
                                            i, isize(int(tex.sampler)), sampler_count));
        }

        for (auto i = isize(0); i < image_count; ++i)
            if (isize(int(result.images[i].buffer_view)) >= view_count)
                return cc::error(cc::format("glTF parse error: image {} references bufferView {}, but the file has {}",
                                            i, isize(int(result.images[i].buffer_view)), view_count));

        CC_RETURN_IF_ERROR(validate_texture_refs());
        return cc::unit{};
    }

    cc::result<cc::unit> validate_texture_refs()
    {
        auto const texture_count = result.textures.size();
        auto const check
            = [texture_count](gltf::texture_ref const& ref) -> bool { return isize(int(ref.texture)) < texture_count; };

        for (auto i = isize(0); i < result.materials.size(); ++i)
        {
            auto const& mat = result.materials[i];
            auto const ok = check(mat.base_color_texture) && check(mat.metallic_roughness_texture)
                         && check(mat.normal_texture) && check(mat.occlusion_texture) && check(mat.emissive_texture);
            if (!ok)
                return cc::error(cc::format(
                    "glTF parse error: material {} references a texture beyond the {} the file has", i, texture_count));
        }
        return cc::unit{};
    }

    /// Images resolve last: a bufferView-backed image needs a validated bufferView and a resolved buffer,
    /// and then its encoded bytes are a plain zero-copy subview of that buffer.
    cc::result<cc::unit> resolve_images()
    {
        for (auto i = isize(0); i < result.images.size(); ++i)
        {
            auto& img = result.images[i];

            if (img.buffer_view != gltf::buffer_view_index::invalid)
            {
                auto const& view = result.buffer_views[isize(int(img.buffer_view))];
                auto const& buf = result.buffers[isize(int(view.buffer))];
                if (!buf.resolved)
                {
                    // The backing buffer was never fetched, so the image stays unresolved with it.
                    add_issue(gltf::issue_kind::unresolved, cc::format("image {} not loaded: its bufferView sits in "
                                                                       "unresolved buffer {}",
                                                                       i, isize(int(view.buffer))));
                    continue;
                }

                img.data = buf.data.subdata({.offset = view.byte_offset, .size = view.byte_length});
                img.resolved = true;
            }
            else if (is_data_uri(img.uri))
            {
                auto bytes = decode_data_uri(img.uri);
                CC_RETURN_IF_ERROR(bytes).with_context(cc::format("while resolving image {}", i));
                img.data = cc::move(bytes).value();
                img.resolved = true;
            }
            else if (!img.uri.empty() && opts.resolve_uri.is_valid())
            {
                auto bytes = opts.resolve_uri(img.uri);
                CC_RETURN_IF_ERROR(bytes).with_context(cc::format("while resolving image {} from uri '{}'", i, img.uri));
                img.data = cc::move(bytes).value();
                img.resolved = true;
            }
            else if (!img.uri.empty())
            {
                add_issue(gltf::issue_kind::unresolved,
                          cc::format("image {} not loaded: uri '{}' needs read_options::resolve_uri", i, img.uri));
            }
        }
        return cc::unit{};
    }
};
} // namespace
} // namespace babel::impl

namespace babel::gltf
{
bool data::has_issue_of(issue_kind kind) const
{
    for (auto const& i : issues)
        if (i.kind == kind)
            return true;
    return false;
}

cc::string data::issue_report() const
{
    auto out = cc::string();
    for (auto const& i : issues)
    {
        if (!out.empty())
            out += "\n";
        switch (i.kind)
        {
        case issue_kind::unsupported:
            out += "unsupported: ";
            break;
        case issue_kind::unresolved:
            out += "unresolved: ";
            break;
        case issue_kind::malformed:
            out += "malformed: ";
            break;
        }
        out += i.message;
    }
    return out;
}

accessor_index data::find_attribute(primitive const& p, cc::string_view semantic) const
{
    for (auto const& attr : attributes_of(p))
        if (attr.semantic == semantic)
            return attr.accessor;
    return accessor_index::invalid;
}

cc::result<accessor_view> data::view_of(accessor const& a) const
{
    auto const* const view = find(a.buffer_view);
    if (view == nullptr)
        return cc::error("gltf: the accessor has no bufferView (its data is implicitly all zero)");

    auto const* const buf = find(view->buffer);
    if (buf == nullptr)
        return cc::error("gltf: the accessor's bufferView has no buffer");
    if (!buf->resolved)
        return cc::error(cc::format("gltf: the accessor's buffer was never resolved (uri '{}')", buf->uri));

    auto const element_size = a.element_size();
    if (element_size <= 0)
        return cc::error("gltf: the accessor has no element size (unknown component or type)");

    // A bufferView without a byteStride is tightly packed, so the element size IS the stride.
    // The accessor's own byte_offset sits on top of the bufferView's.
    auto const stride = view->byte_stride > 0 ? view->byte_stride : element_size;
    auto const start = view->byte_offset + a.byte_offset;
    auto const needed = a.count > 0 ? (a.count - 1) * stride + element_size : i64(0);

    if (!buf->data.span().is_subspan({.offset = start, .size = needed}))
        return cc::error("gltf: the accessor's element range lies outside its buffer");

    return accessor_view{
        .bytes = buf->data.subdata({.offset = start, .size = needed}),
        .stride = stride,
        .count = a.count,
        .element_size = element_size,
        .component = a.component,
        .type = a.type,
        .normalized = a.normalized,
    };
}

cc::result<accessor_view> data::view_of(accessor_index i) const
{
    auto const* const a = find(i);
    if (a == nullptr)
        return cc::error("gltf: invalid accessor index");
    return view_of(*a);
}

cc::result<cc::vector<u32>> data::read_indices(primitive const& p) const
{
    auto const* const a = find(p.indices);
    if (a == nullptr)
        return cc::error("gltf: the primitive is not indexed");
    if (a->type != accessor_type::scalar)
        return cc::error("gltf: an index accessor must be SCALAR");
    if (a->component != component_type::u8 && a->component != component_type::u16 && a->component != component_type::u32)
        return cc::error("gltf: an index accessor must be u8 / u16 / u32");

    auto view = view_of(*a);
    CC_RETURN_IF_ERROR(view);
    auto const& elements = view.value();

    auto const width = a->component_size(); // 1, 2 or 4 after the component check above
    auto indices = cc::vector<u32>();
    indices.resize_to_uninitialized(elements.count);

    for (auto i = i64(0); i < elements.count; ++i)
    {
        // glTF is little-endian, assembled byte-wise so the host's own order never enters into it.
        auto const element = elements.element(i);
        auto value = u32(0);
        for (auto k = 0; k < width; ++k)
            value |= u32(u8(element[k])) << (8 * k);
        indices[isize(i)] = value;
    }

    return cc::move(indices);
}

container detect_container(cc::span<byte const> bytes)
{
    if (bytes.size() >= 4 && babel::impl::load_le_u32(bytes, 0) == babel::impl::glb_magic)
        return container::glb;
    return container::gltf;
}

cc::result<data> read(cc::pinned_data<byte const> bytes, read_options opts)
{
    auto parser = babel::impl::gltf_parser();
    parser.input = cc::move(bytes);
    parser.opts = opts;
    return parser.parse();
}

cc::result<data> read(cc::pinned_data<byte> const& bytes, read_options opts)
{
    return read(cc::pinned_data<byte const>(bytes), opts);
}

cc::result<data> read(cc::read_stream& in, read_options opts)
{
    auto slurped = in.read_all();
    CC_RETURN_IF_ERROR(slurped);

    // make_pinned_data moves the slurped vector into the pin, so the read is the only copy.
    return read(cc::pinned_data<byte const>(cc::make_pinned_data(cc::move(slurped).value())), opts);
}

cc::result<data> read(cc::span<byte const> bytes, read_options opts)
{
    // A span is a borrow, so this pins an owned copy — the returned buffers must not dangle on the caller.
    return read(cc::pinned_data<byte const>(cc::pinned_data<byte>::create_copy_of(bytes)), opts);
}

cc::result<data> read(cc::string_view text, read_options opts)
{
    return read(text.as_bytes(), opts);
}
} // namespace babel::gltf
