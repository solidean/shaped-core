#include <babel-serializer/image/impl/spng_backend.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>

// babel links the vendored `spng` target PRIVATE, so this include never leaves this TU.
#include <spng.h>

namespace babel::impl
{
namespace
{
/// Owns the spng context for one call — every path below returns early, so the destructor is the only free.
struct context_guard
{
    spng_ctx* ctx = nullptr;

    explicit context_guard(int flags) : ctx(spng_ctx_new(flags)) {}
    ~context_guard()
    {
        if (ctx != nullptr)
            spng_ctx_free(ctx);
    }

    context_guard(context_guard const&) = delete;
    context_guard& operator=(context_guard const&) = delete;
};

cc::string spng_message(char const* what, int err)
{
    return cc::format("png: {} failed: {}", what, spng_strerror(err));
}

/// libspng's chunk setters take `char*` and only ever read through it — spng_set_text and spng_set_iccp
/// store the pointer as-is rather than copying, which is also why the pointee must outlive the encode.
char* as_spng_string(char const* p)
{
    return const_cast<char*>(p);
}

cc::result<babel::png::color_type> map_color_type(u8 v)
{
    switch (v)
    {
    case SPNG_COLOR_TYPE_GRAYSCALE:
        return babel::png::color_type::grey;
    case SPNG_COLOR_TYPE_TRUECOLOR:
        return babel::png::color_type::rgb;
    case SPNG_COLOR_TYPE_INDEXED:
        return babel::png::color_type::palette;
    case SPNG_COLOR_TYPE_GRAYSCALE_ALPHA:
        return babel::png::color_type::grey_alpha;
    case SPNG_COLOR_TYPE_TRUECOLOR_ALPHA:
        return babel::png::color_type::rgba;
    default:
        return cc::error(cc::format("png: invalid IHDR color type {}", int(v)));
    }
}

/// Samples per pixel in the file itself, before any conversion.
/// Indexed is the one that does not survive as-is, so it never reaches here.
int native_channels(u8 color_type)
{
    switch (color_type)
    {
    case SPNG_COLOR_TYPE_GRAYSCALE:
        return 1;
    case SPNG_COLOR_TYPE_GRAYSCALE_ALPHA:
        return 2;
    case SPNG_COLOR_TYPE_TRUECOLOR:
        return 3;
    default:
        return 4;
    }
}

/// Which libspng output to ask for, and what it means once we have it.
///
/// `channels` is the count the PNG's own color type implies — 1 grey, 2 grey+alpha, 3 rgb, 4 rgba, plus one
/// for a tRNS chunk that turns into an alpha channel.
/// Every file maps to a format that produces exactly that count, so nothing is decoded and then thrown away.
struct decode_plan
{
    int fmt = SPNG_FMT_RGBA8;
    int channels = 4;
    babel::png::component decoded = babel::png::component::u8;
};

/// SPNG_FMT_PNG is "no conversion": native channel count, native depth, de-interlaced, and byte-swapped to host
/// order for a 16-bit file.
/// It is the right ask wherever the file's own layout is already what we want, which needs two things to hold —
/// the depth must be 8 or 16, since it leaves 1/2/4-bit samples packed, and there must be no tRNS chunk to
/// expand, since it applies none.
/// Everywhere else a converting format does the work: G8 / GA8 unpack sub-byte grey, RGB8 / RGBA8 de-palettize,
/// and the GA16 / RGBA16 pair is what turns tRNS into alpha at 16 bits.
decode_plan plan_decode(spng_ihdr const& ihdr, bool has_trns)
{
    auto const wide = ihdr.bit_depth == 16;
    auto const decoded = wide ? babel::png::component::u16 : babel::png::component::u8;

    // Only color types 0, 2 and 3 may carry tRNS, so alpha is always the channel being added here.
    if (has_trns)
    {
        if (ihdr.color_type == SPNG_COLOR_TYPE_GRAYSCALE)
            return {.fmt = wide ? SPNG_FMT_GA16 : SPNG_FMT_GA8, .channels = 2, .decoded = decoded};
        return {.fmt = wide ? SPNG_FMT_RGBA16 : SPNG_FMT_RGBA8, .channels = 4, .decoded = decoded};
    }

    // Indexed is always 8-bit on disk and never stays indexed, so it is the one case with no native layout to keep.
    if (ihdr.color_type == SPNG_COLOR_TYPE_INDEXED)
        return {.fmt = SPNG_FMT_RGB8, .channels = 3, .decoded = babel::png::component::u8};

    // Grey below 8 bits is packed, and G8 is the format that unpacks it.
    if (ihdr.color_type == SPNG_COLOR_TYPE_GRAYSCALE && ihdr.bit_depth < 8)
        return {.fmt = SPNG_FMT_G8, .channels = 1, .decoded = babel::png::component::u8};

    return {.fmt = SPNG_FMT_PNG, .channels = native_channels(ihdr.color_type), .decoded = decoded};
}

/// Read every ancillary chunk png::data models.
/// A getter returning SPNG_ECHUNKAVAIL means the chunk is absent, which is not an error — the field stays empty.
void read_metadata(spng_ctx* ctx, babel::png::data& out)
{
    auto gamma = 0.0;
    if (spng_get_gama(ctx, &gamma) == 0)
        out.gamma = gamma;

    auto intent = u8(0);
    if (spng_get_srgb(ctx, &intent) == 0)
        out.srgb_intent = int(intent);

    auto iccp = spng_iccp{};
    if (spng_get_iccp(ctx, &iccp) == 0)
    {
        out.icc_profile_name = cc::string(iccp.profile_name); // char[80], always NUL-terminated by libspng
        out.icc_profile.resize_to_uninitialized(isize(iccp.profile_len));
        cc::memcpy(out.icc_profile.data(), iccp.profile, iccp.profile_len);
    }

    auto phys = spng_phys{};
    if (spng_get_phys(ctx, &phys) == 0)
        out.physical = babel::png::physical_dimensions{.ppu_x = int(phys.ppu_x), //
                                                       .ppu_y = int(phys.ppu_y),
                                                       .unit_is_meter = phys.unit_specifier == 1};

    // Two calls by design: a null `text` asks only for the count.
    auto count = u32(0);
    if (spng_get_text(ctx, nullptr, &count) != 0 || count == 0)
        return;

    auto entries = cc::vector<spng_text>();
    entries.resize_to_filled(isize(count), spng_text{});
    if (spng_get_text(ctx, entries.data(), &count) != 0)
        return;

    out.texts.reserve(isize(count));
    for (auto i = u32(0); i < count; ++i)
    {
        auto const& t = entries[i];
        out.texts.push_back({
            .keyword = cc::string(t.keyword),
            .text = cc::string(t.text, isize(t.length)),
            .language = t.language_tag != nullptr ? cc::string(t.language_tag) : cc::string(),
            .translated_keyword = t.translated_keyword != nullptr ? cc::string(t.translated_keyword) : cc::string(),
            .compressed = t.type == SPNG_ZTXT || (t.type == SPNG_ITXT && t.compression_flag != 0),
        });
    }
}

/// spng_rw_fn sink: append `length` encoded bytes into the cc::vector behind `user`.
int append_to_vector(spng_ctx*, void* user, void* src, size_t length)
{
    auto& out = *static_cast<cc::vector<byte>*>(user);
    auto const old = out.size();
    out.resize_to_uninitialized(old + isize(length));
    cc::memcpy(out.data() + old, src, length);
    return 0;
}

/// The ancillary chunks an encode emits, and the storage libspng needs still alive when it writes them.
///
/// spng_set_text and spng_set_iccp keep the caller's pointers rather than copying, so both this object and the
/// `img` it was applied to must outlive spng_encode_image.
struct encode_metadata
{
    cc::vector<spng_text> entries;
    cc::vector<cc::string> c_strings; // NUL-terminated copies of every string libspng reads as a C string

    cc::result<cc::unit> apply(spng_ctx* ctx, babel::png::data const& img)
    {
        if (img.gamma.has_value())
            if (auto const err = spng_set_gama(ctx, img.gamma.value()); err != 0)
                return cc::error(spng_message("set_gama", err));

        if (img.srgb_intent.has_value())
            if (auto const err = spng_set_srgb(ctx, u8(img.srgb_intent.value())); err != 0)
                return cc::error(spng_message("set_srgb", err));

        if (img.physical.has_value())
        {
            auto const& p = img.physical.value();
            auto phys
                = spng_phys{.ppu_x = u32(p.ppu_x), .ppu_y = u32(p.ppu_y), .unit_specifier = u8(p.unit_is_meter ? 1 : 0)};
            if (auto const err = spng_set_phys(ctx, &phys); err != 0)
                return cc::error(spng_message("set_phys", err));
        }

        if (!img.icc_profile.empty())
        {
            auto iccp = spng_iccp{}; // zeroed, so profile_name stays NUL-terminated after the copy below
            auto const name = cc::string_view(img.icc_profile_name);
            if (name.empty() || name.size() >= isize(sizeof(iccp.profile_name)))
                return cc::error(
                    cc::format("png encode: iCCP profile name must be 1..79 characters, got {}", name.size()));

            cc::memcpy(iccp.profile_name, name.data(), size_t(name.size()));
            iccp.profile_len = size_t(img.icc_profile.size());
            iccp.profile = as_spng_string(reinterpret_cast<char const*>(img.icc_profile.data()));
            if (auto const err = spng_set_iccp(ctx, &iccp); err != 0)
                return cc::error(spng_message("set_iccp", err));
        }

        if (img.texts.empty())
            return cc::unit{};

        // Reserved up front, because the pointers handed to libspng below must survive every later push_back.
        entries.reserve(img.texts.size());
        c_strings.reserve(img.texts.size() * 3);

        for (auto const& t : img.texts)
        {
            if (t.keyword.empty() || t.keyword.size() >= 80)
                return cc::error(
                    cc::format("png encode: text keyword must be 1..79 characters, got {}", t.keyword.size()));
            if (t.text.empty())
                return cc::error(cc::format("png encode: text chunk {} carries no text", t.keyword));

            auto entry = spng_text{};
            cc::memcpy(entry.keyword, t.keyword.data(), size_t(t.keyword.size()));

            // The body must be NUL-terminated: libspng's encoder ignores `length` and measures with strlen,
            // so pointing at a cc::string's own bytes reads past its end.
            // `length` is still set, because spng_set_text rejects a zero one.
            c_strings.push_back(cc::string::create_copy_c_str_materialized(t.text));
            entry.text = as_spng_string(c_strings.back().c_str_if_terminated());
            entry.length = size_t(t.text.size());

            // iTXt is the only variant carrying a language, and it is what an entry with one must be written as.
            if (!t.language.empty() || !t.translated_keyword.empty())
            {
                entry.type = SPNG_ITXT;
                entry.compression_flag = t.compressed ? 1 : 0;
                c_strings.push_back(cc::string::create_copy_c_str_materialized(t.language));
                entry.language_tag = as_spng_string(c_strings.back().c_str_if_terminated());
                c_strings.push_back(cc::string::create_copy_c_str_materialized(t.translated_keyword));
                entry.translated_keyword = as_spng_string(c_strings.back().c_str_if_terminated());
            }
            else
                entry.type = t.compressed ? SPNG_ZTXT : SPNG_TEXT;

            entries.push_back(entry);
        }

        if (auto const err = spng_set_text(ctx, entries.data(), u32(entries.size())); err != 0)
            return cc::error(spng_message("set_text", err));

        return cc::unit{};
    }
};

cc::result<int> spng_color_type_for(int channels)
{
    switch (channels)
    {
    case 1:
        return SPNG_COLOR_TYPE_GRAYSCALE;
    case 2:
        return SPNG_COLOR_TYPE_GRAYSCALE_ALPHA;
    case 3:
        return SPNG_COLOR_TYPE_TRUECOLOR;
    case 4:
        return SPNG_COLOR_TYPE_TRUECOLOR_ALPHA;
    default:
        return cc::error(cc::format("png encode: unsupported channel count {}", channels));
    }
}
} // namespace

cc::result<babel::png::data> spng_decode_png(cc::span<byte const> bytes)
{
    if (bytes.empty())
        return cc::error("png decode: empty input");

    auto guard = context_guard(0);
    if (guard.ctx == nullptr)
        return cc::error("png decode: could not create an spng context");

    if (auto const err = spng_set_png_buffer(guard.ctx, bytes.data(), size_t(bytes.size())); err != 0)
        return cc::error(spng_message("set_png_buffer", err));

    auto ihdr = spng_ihdr{};
    if (auto const err = spng_get_ihdr(guard.ctx, &ihdr); err != 0)
        return cc::error(spng_message("get_ihdr", err));

    auto color = map_color_type(ihdr.color_type);
    CC_RETURN_IF_ERROR(color);

    auto trns = spng_trns{};
    auto const plan = plan_decode(ihdr, spng_get_trns(guard.ctx, &trns) == 0);

    auto size = size_t(0);
    if (auto const err = spng_decoded_image_size(guard.ctx, plan.fmt, &size); err != 0)
        return cc::error(spng_message("decoded_image_size", err));

    auto result = babel::png::data{
        .width = int(ihdr.width),
        .height = int(ihdr.height),
        .channels = plan.channels,
        .bit_depth = int(ihdr.bit_depth),
        .color = color.value(),
        .interlace = ihdr.interlace_method == SPNG_INTERLACE_ADAM7 ? babel::png::interlace_method::adam7
                                                                   : babel::png::interlace_method::none,
        .decoded = plan.decoded,
    };

    result.pixels.resize_to_uninitialized(isize(size));
    // SPNG_DECODE_TRNS is what turns a tRNS chunk into the alpha channel `plan` budgeted for.
    if (auto const err = spng_decode_image(guard.ctx, result.pixels.data(), size, plan.fmt, SPNG_DECODE_TRNS); err != 0)
        return cc::error(spng_message("decode_image", err));

    // Chunks stored after IDAT are only reachable once the image is decoded, and a file carrying none is not an error.
    spng_decode_chunks(guard.ctx);
    read_metadata(guard.ctx, result);

    return cc::move(result);
}

cc::result<cc::vector<byte>> spng_encode_png(babel::png::data const& img, int compression_level)
{
    if (img.width <= 0 || img.height <= 0)
        return cc::error(cc::format("png encode: non-positive dimensions {}x{}", img.width, img.height));
    if (compression_level < -1 || compression_level > 9)
        return cc::error(cc::format("png encode: compression level {} out of range -1..9", compression_level));

    auto color = spng_color_type_for(img.channels); // not const: CC_RETURN_IF_ERROR moves the error out
    CC_RETURN_IF_ERROR(color);

    auto const wide = img.decoded == babel::png::component::u16;
    auto const needed = isize(img.width) * isize(img.height) * isize(img.channels) * (wide ? 2 : 1);
    if (img.pixels.size() < needed)
        return cc::error(cc::format("png encode: pixel buffer too small ({} < {})", img.pixels.size(), needed));

    auto guard = context_guard(SPNG_CTX_ENCODER);
    if (guard.ctx == nullptr)
        return cc::error("png encode: could not create an spng context");

    auto out = cc::vector<byte>();
    if (auto const err = spng_set_png_stream(guard.ctx, &append_to_vector, &out); err != 0)
        return cc::error(spng_message("set_png_stream", err));

    auto ihdr = spng_ihdr{
        .width = u32(img.width),
        .height = u32(img.height),
        // From `decoded` rather than `bit_depth`: the latter describes a file that was read, not one being written.
        .bit_depth = u8(wide ? 16 : 8),
        .color_type = u8(color.value()),
        .compression_method = 0,
        .filter_method = 0,
        .interlace_method = SPNG_INTERLACE_NONE};
    if (auto const err = spng_set_ihdr(guard.ctx, &ihdr); err != 0)
        return cc::error(spng_message("set_ihdr", err));

    // -1 leaves libspng on zlib's own default rather than pinning a level of our choosing.
    if (compression_level >= 0)
        if (auto const err = spng_set_option(guard.ctx, SPNG_IMG_COMPRESSION_LEVEL, compression_level); err != 0)
            return cc::error(spng_message("set_option", err));

    // `meta` holds pointers libspng reads during spng_encode_image, so it must outlive that call.
    auto meta = encode_metadata();
    CC_RETURN_IF_ERROR(meta.apply(guard.ctx, img));

    // SPNG_FMT_PNG: the pixels already match the IHDR above, so the only thing libspng touches is byte order,
    // swapping 16-bit samples from host into the big-endian the format stores.
    if (auto const err
        = spng_encode_image(guard.ctx, img.pixels.data(), size_t(needed), SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE);
        err != 0)
        return cc::error(spng_message("encode_image", err));

    return cc::move(out);
}
} // namespace babel::impl
