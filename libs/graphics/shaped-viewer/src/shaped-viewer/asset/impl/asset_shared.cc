#include <babel-serializer/image/image.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <clean-core/string/format.hh>
#include <shaped-viewer/asset/impl/asset_import.hh>

// What more than one importer needs: turning a decoded image into the one pixel shape `texture_data` takes, and giving every light of an asset an id of its own.

namespace sv
{
cc::result<cc::vector<byte>> impl::to_rgba8(babel::image::image const& img)
{
    if (img.is_empty() || img.channels < 1 || img.channels > 4)
        return cc::error("shaped-viewer: image has no usable pixels");

    auto const pixel_count = isize(img.width) * isize(img.height);
    auto out = cc::vector<byte>::create_filled(pixel_count * 4, byte(255));

    auto const sample = [&](isize index) -> byte
    {
        switch (img.comp)
        {
        case babel::image::component::u8:
            return img.pixels[index];
        case babel::image::component::u16:
        {
            // Host-endian u16 pairs; the high byte is the 8-bit value.
            auto const lo = u32(img.pixels[index * 2]);
            auto const hi = u32(img.pixels[index * 2 + 1]);
            return byte((lo | (hi << 8)) >> 8);
        }
        case babel::image::component::f32:
        {
            auto const v = img.samples_f32()[index];
            auto const clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            return byte(u32(clamped * 255.0f + 0.5f));
        }
        }
        return byte(0);
    };

    for (auto p = isize(0); p < pixel_count; ++p)
        for (auto c = 0; c < img.channels; ++c)
        {
            auto const value = sample(p * isize(img.channels) + c);
            // 1 channel is gray and 2 is gray+alpha, so the single value fills rgb rather than r alone.
            if (img.channels <= 2 && c == 0)
            {
                out[p * 4 + 0] = value;
                out[p * 4 + 1] = value;
                out[p * 4 + 2] = value;
            }
            else if (img.channels <= 2)
                out[p * 4 + 3] = value;
            else
                out[p * 4 + c] = value;
        }

    return cc::move(out);
}

void impl::disambiguate_light_ids(cc::vector<asset_light>& lights)
{
    // Counted once rather than compared pairwise, so a file of many lights stays linear.
    auto counts = cc::map<cc::string, isize>();
    for (auto const& l : lights)
        counts[l.id] += 1;
    auto shared = cc::vector<u8>::create_filled(lights.size(), u8(0));
    for (auto i = isize(0); i < lights.size(); ++i)
        shared[i] = counts[lights[i].id] > 1 ? 1 : 0;

    // The names that stay as they are claim their ids first, so a generated one never takes a name the file chose.
    auto taken = cc::set<cc::string>();
    for (auto i = isize(0); i < lights.size(); ++i)
        if (!lights[i].id.empty() && shared[i] == 0)
            taken.insert(lights[i].id);

    for (auto i = isize(0); i < lights.size(); ++i)
    {
        auto& id = lights[i].id;
        if (!id.empty() && shared[i] == 0)
            continue;

        auto const base = id.empty() ? cc::string("light") : cc::string(id);
        auto suffix = i;
        auto candidate = cc::format("{}##{}", base, suffix);
        while (taken.contains(candidate))
            candidate = cc::format("{}##{}", base, ++suffix);
        taken.insert(candidate);
        id = cc::move(candidate);
    }
}
} // namespace sv
