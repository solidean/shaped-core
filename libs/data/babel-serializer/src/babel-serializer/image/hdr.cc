#include <babel-serializer/image/hdr.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>
#include <clean-core/string/string_view.hh>
#include <typed-geometry/scalar/scalar.hh>

namespace babel::hdr
{
namespace
{
// A scanline wider than this cannot use the adaptive RLE: its length would be indistinguishable from a pixel,
// which is why the format excludes those widths rather than us.
constexpr int k_min_rle_width = 8;
constexpr int k_max_rle_width = 0x7FFF;

// Radiance drops a pixel to black below this, and so do we — it is the floor the exponent split assumes.
constexpr float k_black_cutoff = 1e-32f;

// 2^127, the ceiling: at or above it the biased exponent byte would overflow, and infinity is on the wrong side of it too.
constexpr float k_saturation_cutoff = 1.70141183e38f;

// A file claiming more than this many pixels is rejected before anything is allocated.
constexpr isize k_max_pixels = isize(1) << 32;

// --- RGBE <-> f32 ----------------------------------------------------------------------------------------
// The whole conversion is base-two arithmetic on a shared exponent, so it goes through tg::pow2 / tg::exponent_of
// rather than a multiply: every scale below is a power of two, and none of them costs precision.

void rgbe_to_floats(byte const* rgbe, float* out)
{
    auto const exponent = int(u8(rgbe[3]));
    if (exponent == 0)
    {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
        return;
    }

    // The mantissas are NOT offset by half a step on the way back: the encoder below rounds rather than
    // truncating, so the pair is already centered, and an exactly-zero channel stays exactly zero.
    auto const scale = tg::pow2<double>(exponent - (128 + 8));
    out[0] = float(double(int(u8(rgbe[0]))) * scale);
    out[1] = float(double(int(u8(rgbe[1]))) * scale);
    out[2] = float(double(int(u8(rgbe[2]))) * scale);
}

byte quantize_mantissa(float value, double scale)
{
    if (!(value > 0.0f)) // also catches NaN
        return byte(0);

    auto const scaled = double(value) * scale + 0.5;
    return byte(u8(scaled >= 255.0 ? 255 : int(scaled)));
}

void floats_to_rgbe(float const* rgb, byte* out)
{
    auto const largest = cc::max(cc::max(rgb[0], rgb[1]), rgb[2]);

    if (!(largest > k_black_cutoff)) // black, and every NaN or non-positive pixel with it
    {
        out[0] = byte(0);
        out[1] = byte(0);
        out[2] = byte(0);
        out[3] = byte(0);
        return;
    }

    // Ahead of tg::exponent_of, which requires a finite scalar, and not merely for its own sake.
    if (!(largest < k_saturation_cutoff)) // infinity, or a float the shared exponent cannot hold: saturate
    {
        out[0] = byte(255);
        out[1] = byte(255);
        out[2] = byte(255);
        out[3] = byte(255);
        return;
    }

    // Radiance biases the exponent by 128 over a mantissa in [0.5, 1), one below tg's [1, 2) — hence the extra 1.
    // The mantissa scale then falls out as a pure power of two, since `largest` divided by its own exponent is the mantissa.
    auto const exponent = tg::exponent_of(largest) + 1 + 128;
    auto const scale = tg::pow2<double>((128 + 8) - exponent);

    out[0] = quantize_mantissa(rgb[0], scale);
    out[1] = quantize_mantissa(rgb[1], scale);
    out[2] = quantize_mantissa(rgb[2], scale);
    out[3] = byte(u8(exponent));
}

// --- header ----------------------------------------------------------------------------------------------

/// One header line without its terminator, plus where the line after it starts.
struct line_view
{
    cc::string_view text = {};
    isize next = 0;
};

line_view next_line(cc::span<byte const> bytes, isize pos)
{
    auto end = pos;
    while (end < bytes.size() && bytes[end] != byte('\n'))
        ++end;

    auto text = cc::string_view(reinterpret_cast<char const*>(bytes.data()) + pos, end - pos);
    if (text.ends_with('\r'))
        text.remove_suffix(1);

    return {.text = text, .next = end < bytes.size() ? end + 1 : end};
}

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

cc::string_view trimmed(cc::string_view s)
{
    while (!s.empty() && is_space(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && is_space(s.back()))
        s.remove_suffix(1);
    return s;
}

/// Split a line on whitespace runs, appending to `out`.
void split_tokens(cc::string_view line, cc::vector<cc::string_view>& out)
{
    auto pos = isize(0);
    while (pos < line.size())
    {
        while (pos < line.size() && is_space(line[pos]))
            ++pos;
        auto const start = pos;
        while (pos < line.size() && !is_space(line[pos]))
            ++pos;
        if (pos > start)
            out.push_back(line.subview({.start = start, .end = pos}));
    }
}

// --- scanlines -------------------------------------------------------------------------------------------

/// Decode one adaptive-RLE scanline: four component planes, each run-length encoded on its own.
/// The four marker bytes have already been consumed by the caller.
cc::result<cc::unit> decode_rle_scanline(cc::span<byte const> bytes, isize& pos, int width, byte* row)
{
    for (auto component = 0; component < 4; ++component)
    {
        auto x = 0;
        while (x < width)
        {
            if (pos >= bytes.size())
                return cc::error("hdr: scanline data ends mid-run");

            auto const count = int(u8(bytes[pos]));
            ++pos;

            if (count > 128) // a run of one repeated value
            {
                auto const run = count - 128;
                if (x + run > width)
                    return cc::error("hdr: run overruns the scanline");
                if (pos >= bytes.size())
                    return cc::error("hdr: scanline data ends before a run's value");

                auto const value = bytes[pos];
                ++pos;
                for (auto i = 0; i < run; ++i)
                    row[(x + i) * 4 + component] = value;
                x += run;
            }
            else // a literal of `count` values
            {
                if (count == 0)
                    return cc::error("hdr: zero-length literal in a scanline");
                if (x + count > width)
                    return cc::error("hdr: literal overruns the scanline");
                if (pos + count > bytes.size())
                    return cc::error("hdr: scanline data ends mid-literal");

                for (auto i = 0; i < count; ++i)
                    row[(x + i) * 4 + component] = bytes[pos + i];
                pos += count;
                x += count;
            }
        }
    }
    return cc::unit{};
}

/// Decode one flat scanline, honoring the old `1 1 1 n` repeat marker.
/// Consecutive markers shift by 8 more bits each, which is how the old encoding spelled runs past 255.
cc::result<cc::unit> decode_flat_scanline(cc::span<byte const> bytes, isize& pos, int width, byte* row)
{
    auto shift = 0;
    auto x = 0;
    while (x < width)
    {
        if (pos + 4 > bytes.size())
            return cc::error("hdr: scanline data ends mid-pixel");

        auto const is_repeat = u8(bytes[pos]) == 1 && u8(bytes[pos + 1]) == 1 && u8(bytes[pos + 2]) == 1;
        if (!is_repeat)
        {
            for (auto i = 0; i < 4; ++i)
                row[x * 4 + i] = bytes[pos + i];
            pos += 4;
            shift = 0;
            ++x;
            continue;
        }

        if (x == 0)
            return cc::error("hdr: repeat marker at the start of a scanline has nothing to repeat");

        auto const run = int(u8(bytes[pos + 3])) << shift;
        pos += 4;
        if (x + run > width)
            return cc::error("hdr: repeat run overruns the scanline");

        auto const* const previous = row + (x - 1) * 4;
        for (auto i = 0; i < run; ++i)
            cc::memcpy(row + (x + i) * 4, previous, 4);
        shift += 8;
        x += run;
    }
    return cc::unit{};
}

// --- encoding --------------------------------------------------------------------------------------------

void append_text(cc::vector<byte>& out, cc::string_view text)
{
    auto const old = out.size();
    out.resize_to_uninitialized(old + text.size());
    cc::memcpy(out.data() + old, text.data(), size_t(text.size()));
}

/// Emit one component plane of a scanline as runs and literals.
void encode_rle_plane(cc::vector<byte>& out, byte const* row, int width, int component)
{
    auto const at = [&](int x) { return row[x * 4 + component]; };

    auto x = 0;
    while (x < width)
    {
        auto run = 1;
        while (x + run < width && at(x + run) == at(x) && run < 127)
            ++run;

        if (run >= 4)
        {
            out.push_back(byte(u8(128 + run)));
            out.push_back(at(x));
            x += run;
            continue;
        }

        // Literals up to the next run of four, capped at the 128 a count byte can spell.
        auto end = x;
        while (end < width && end - x < 128)
        {
            auto ahead = 1;
            while (end + ahead < width && at(end + ahead) == at(end) && ahead < 4)
                ++ahead;
            if (ahead >= 4)
                break;
            ++end;
        }

        out.push_back(byte(u8(end - x)));
        for (auto i = x; i < end; ++i)
            out.push_back(at(i));
        x = end;
    }
}
} // namespace

cc::span<float const> data::samples_f32() const
{
    auto const floats = cc::span<byte const>(pixels).try_reinterpret_as<float const>();
    return floats.has_value() ? floats.value() : cc::span<float const>();
}

cc::result<data> read(cc::span<byte const> bytes)
{
    CC_RECORD_SCOPE("hdr.read");

    if (bytes.size() < 2 || bytes[0] != byte('#') || bytes[1] != byte('?'))
        return cc::error("hdr: bad magic (a Radiance file opens with '#?')");

    auto result = data{};
    auto pos = next_line(bytes, 0).next; // the magic line itself carries nothing we keep

    // Header variables, up to the blank line that ends the header.
    auto format_seen = false;
    while (true)
    {
        if (pos >= bytes.size())
            return cc::error("hdr: header is not terminated by a blank line");

        auto const line = next_line(bytes, pos);
        pos = line.next;

        if (line.text.empty())
            break;
        if (line.text.starts_with('#'))
            continue;

        auto const equals = line.text.find('=');
        if (equals < 0)
            continue; // Radiance tolerates a stray line here, and so do we

        auto const key = trimmed(line.text.subview({.start = 0, .end = equals}));
        auto const value = trimmed(line.text.subview(equals + 1));
        result.variables.push_back({.key = cc::string(key), .value = cc::string(value)});

        if (key == "FORMAT")
        {
            if (value == "32-bit_rle_rgbe")
                result.format = pixel_format::rgbe;
            else if (value == "32-bit_rle_xyze")
                result.format = pixel_format::xyze;
            else
                return cc::error(cc::format("hdr: unsupported FORMAT '{}'", value));
            format_seen = true;
        }
        else if (key == "EXPOSURE")
        {
            // Radiance defines repeated EXPOSURE lines as cumulative, so they multiply.
            if (auto const parsed = cc::from_string<double>(value); parsed.has_value())
                result.exposure = result.exposure.value_or(1.0) * parsed.value();
        }
        else if (key == "PIXASPECT")
        {
            if (auto const parsed = cc::from_string<double>(value); parsed.has_value())
                result.pixel_aspect = parsed.value();
        }
        else if (key == "SOFTWARE")
            result.software = cc::string(value);
    }

    if (!format_seen)
        return cc::error("hdr: header carries no FORMAT= line");

    // The resolution line, which is also what says whether the first scanline is the top or the bottom row.
    auto const resolution = next_line(bytes, pos);
    pos = resolution.next;

    auto tokens = cc::vector<cc::string_view>();
    split_tokens(resolution.text, tokens);
    if (tokens.size() != 4 || tokens[2] != "+X")
        return cc::error(cc::format("hdr: unsupported resolution line '{}' (expected '-Y h +X w')", resolution.text));
    if (tokens[0] != "-Y" && tokens[0] != "+Y")
        return cc::error(cc::format("hdr: unsupported resolution line '{}' (column-major files are not read)", //
                                    resolution.text));

    auto const height = cc::from_string<int>(tokens[1]);
    auto const width = cc::from_string<int>(tokens[3]);
    if (!height.has_value() || !width.has_value() || height.value() <= 0 || width.value() <= 0)
        return cc::error(cc::format("hdr: bad resolution line '{}'", resolution.text));

    result.width = width.value();
    result.height = height.value();
    result.stored_bottom_up = tokens[0] == "+Y";

    auto const pixel_count = isize(result.width) * isize(result.height);
    if (pixel_count > k_max_pixels)
        return cc::error(cc::format("hdr: implausible image size {}x{}", result.width, result.height));
    if (bytes.size() - pos < isize(result.height) * 4)
        return cc::error("hdr: file is too short to hold that many scanlines");

    // Scanlines, decoded into their packed RGBE form first; the float conversion is one pass over that.
    auto rgbe = cc::vector<byte>();
    rgbe.resize_to_uninitialized(pixel_count * 4);

    for (auto y = 0; y < result.height; ++y)
    {
        auto* const row = rgbe.data() + isize(y) * result.width * 4;

        auto const has_rle_marker = pos + 4 <= bytes.size() && u8(bytes[pos]) == 2 && u8(bytes[pos + 1]) == 2
                                 && (int(u8(bytes[pos + 2])) << 8 | int(u8(bytes[pos + 3]))) == result.width;
        auto const rle_width = result.width >= k_min_rle_width && result.width <= k_max_rle_width;

        if (has_rle_marker && rle_width)
        {
            pos += 4;
            CC_RETURN_IF_ERROR(decode_rle_scanline(bytes, pos, result.width, row));
            result.run_length_encoded = true;
        }
        else
            CC_RETURN_IF_ERROR(decode_flat_scanline(bytes, pos, result.width, row));
    }

    result.pixels.resize_to_uninitialized(pixel_count * 3 * isize(sizeof(float)));
    auto* const out = reinterpret_cast<float*>(result.pixels.data());
    for (auto y = 0; y < result.height; ++y)
    {
        auto const target_y = result.stored_bottom_up ? result.height - 1 - y : y;
        auto const* const src = rgbe.data() + isize(y) * result.width * 4;
        auto* const dst = out + isize(target_y) * result.width * 3;
        for (auto x = 0; x < result.width; ++x)
            rgbe_to_floats(src + x * 4, dst + x * 3);
    }

    return cc::move(result);
}

cc::result<data> read(cc::read_stream& in)
{
    auto bytes = in.read_all();
    CC_RETURN_IF_ERROR(bytes);
    return read(bytes.value());
}

cc::result<cc::vector<byte>> encode(data const& img, write_options opts)
{
    if (img.is_empty())
        return cc::error("hdr encode: empty image");
    if (img.channels != 3)
        return cc::error(cc::format("hdr encode: RGBE stores 3 channels, got {}", img.channels));

    auto const samples = img.samples_f32();
    auto const needed = isize(img.width) * isize(img.height) * 3;
    if (samples.size() < needed)
        return cc::error(cc::format("hdr encode: pixel buffer too small ({} < {} floats)", samples.size(), needed));
    if (img.software.contains('\n'))
        return cc::error("hdr encode: SOFTWARE must not contain a newline");

    auto out = cc::vector<byte>();

    append_text(out, "#?RADIANCE\n");
    append_text(out, img.format == pixel_format::xyze ? "FORMAT=32-bit_rle_xyze\n" : "FORMAT=32-bit_rle_rgbe\n");
    if (img.exposure.has_value())
        append_text(out, cc::format("EXPOSURE={}\n", img.exposure.value()));
    if (img.pixel_aspect.has_value())
        append_text(out, cc::format("PIXASPECT={}\n", img.pixel_aspect.value()));
    if (!img.software.empty())
        append_text(out, cc::format("SOFTWARE={}\n", img.software));
    append_text(out, "\n");
    append_text(out, cc::format("-Y {} +X {}\n", img.height, img.width));

    auto const use_rle = opts.run_length_encode && img.width >= k_min_rle_width && img.width <= k_max_rle_width;

    auto row = cc::vector<byte>();
    row.resize_to_uninitialized(isize(img.width) * 4);

    for (auto y = 0; y < img.height; ++y)
    {
        auto const* const src = samples.data() + isize(y) * img.width * 3;
        for (auto x = 0; x < img.width; ++x)
            floats_to_rgbe(src + x * 3, row.data() + x * 4);

        if (!use_rle)
        {
            auto const old = out.size();
            out.resize_to_uninitialized(old + row.size());
            cc::memcpy(out.data() + old, row.data(), size_t(row.size()));
            continue;
        }

        out.push_back(byte(2));
        out.push_back(byte(2));
        out.push_back(byte(u8((img.width >> 8) & 0xFF)));
        out.push_back(byte(u8(img.width & 0xFF)));
        for (auto component = 0; component < 4; ++component)
            encode_rle_plane(out, row.data(), img.width, component);
    }

    return cc::move(out);
}

cc::result<cc::unit> write(cc::write_stream& out, data const& img, write_options opts)
{
    auto encoded = encode(img, opts);
    CC_RETURN_IF_ERROR(encoded);
    CC_RETURN_IF_ERROR(out.write(encoded.value()));
    return cc::unit{};
}
} // namespace babel::hdr
