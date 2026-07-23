#include <clean-core/common/asserts.hh>
#include <shaped-graphics/context.hh>
#include <shaped-rendering/impl/imgui_draw_math.hh>
#include <shaped-rendering/impl/imgui_texture_registry.hh>

namespace sr::impl
{
namespace
{
/// imgui hands out `slot index + 1`, keeping 0 as ImTextureID_Invalid.
[[nodiscard]] ImTextureID id_of_slot(isize slot_index)
{
    return ImTextureID(slot_index + 1);
}
[[nodiscard]] isize slot_of_id(ImTextureID id)
{
    return isize(id) - 1;
}
} // namespace

void imgui_texture_registry::service_requests(sg::context& ctx, ImDrawData* draw_data)
{
    CC_ASSERT(draw_data != nullptr, "draw data must not be null");

    // Textures is a pointer into the platform IO and may legitimately be null when a caller drives texture updates itself.
    if (draw_data->Textures == nullptr)
        return;

    for (ImTextureData* const tex : *draw_data->Textures)
    {
        switch (tex->Status)
        {
        case ImTextureStatus_WantCreate:
            create_texture(ctx, tex);
            break;
        case ImTextureStatus_WantUpdates:
            update_texture(ctx, tex);
            break;
        case ImTextureStatus_WantDestroy:
            // UnusedFrames counts frames since imgui last referenced it.
            // Destroying at 0 would free a texture a command list still in flight is sampling.
            if (tex->UnusedFrames > 0)
                destroy_texture(tex);
            break;
        case ImTextureStatus_OK:
        case ImTextureStatus_Destroyed:
            break;
        }
    }
}

void imgui_texture_registry::create_texture(sg::context& ctx, ImTextureData* tex)
{
    // imgui.hlsl samples .rgba unconditionally, so an Alpha8 atlas would render as red-on-transparent.
    // imgui_context pins TexDesiredFormat to RGBA32; this catches anyone changing it.
    CC_ASSERT(tex->Format == ImTextureFormat_RGBA32, "imgui atlas must be RGBA32 — see io.Fonts->TexDesiredFormat in "
                                                     "sr::imgui_context");
    CC_ASSERT(tex->Width > 0 && tex->Height > 0, "imgui requested a degenerate texture");

    auto texture
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                            .width = tex->Width,
                                            .height = tex->Height,
                                            .usage = sg::texture_usage::readonly_texture | sg::texture_usage::copy_dst});

    // A freshly built atlas is already tightly packed, so the whole-texture upload needs no repacking —
    // but ctx.upload is fire-and-forget and holds the pin until the copy runs,
    // so the bytes must be owned rather than borrowed from imgui, which may free them once the status goes OK.
    auto const pixels = pack_texture_rect(reinterpret_cast<cc::byte const*>(tex->GetPixels()), tex->GetPitch(),
                                          tex->BytesPerPixel, tg::pos2i(0, 0), tg::vec2i(tex->Width, tex->Height));
    ctx.upload.bytes_to_texture(texture.raw(), pixels);

    auto const slot_index = [&]
    {
        if (!_free_slots.empty())
        {
            return _free_slots.pop_back();
        }
        _slots.emplace_back();
        return _slots.size() - 1;
    }();

    _slots[slot_index] = cc::move(texture);
    tex->SetTexID(id_of_slot(slot_index));
    tex->SetStatus(ImTextureStatus_OK);
}

void imgui_texture_registry::update_texture(sg::context& ctx, ImTextureData* tex)
{
    auto const slot_index = slot_of_id(tex->GetTexID());
    CC_ASSERT(slot_index >= 0 && slot_index < _slots.size() && _slots[slot_index].raw() != nullptr,
              "imgui asked to update a texture we never created");
    auto const& texture = _slots[slot_index];

    // Updates[] is the precise list; UpdateRect is its bounding box.
    // Preferring the list keeps a few scattered glyph patches from re-uploading everything between them.
    auto const upload_rect = [&](ImTextureRect const& r)
    {
        if (r.w == 0 || r.h == 0)
            return;

        CC_ASSERT(int(r.x) + int(r.w) <= texture.width() && int(r.y) + int(r.h) <= texture.height(),
                  "imgui update rect runs outside the texture");

        // The upload wants the region tightly packed, but imgui's rect is strided by the whole atlas pitch —
        // so this repacks into the pinned buffer the upload takes ownership of.
        auto const pixels
            = pack_texture_rect(reinterpret_cast<cc::byte const*>(tex->GetPixels()), tex->GetPitch(),
                                tex->BytesPerPixel, tg::pos2i(int(r.x), int(r.y)), tg::vec2i(int(r.w), int(r.h)));
        ctx.upload.bytes_to_texture(
            texture.raw(), pixels, {},
            sg::texture_region{.offset = tg::pos3i(int(r.x), int(r.y), 0), .size = tg::vec3i(int(r.w), int(r.h), 1)});
    };

    if (!tex->Updates.empty())
        for (auto const& r : tex->Updates)
            upload_rect(r);
    else
        upload_rect(tex->UpdateRect);

    tex->SetStatus(ImTextureStatus_OK);
}

void imgui_texture_registry::destroy_texture(ImTextureData* tex)
{
    auto const slot_index = slot_of_id(tex->GetTexID());
    CC_ASSERT(slot_index >= 0 && slot_index < _slots.size(), "imgui asked to destroy a texture we never created");

    _slots[slot_index] = {}; // drops the handle; the context defers the actual release past in-flight work
    _free_slots.push_back(slot_index);

    tex->SetTexID(ImTextureID_Invalid);
    tex->SetStatus(ImTextureStatus_Destroyed);
}

cc::result<sg::texture_2d> imgui_texture_registry::try_texture_of(ImTextureID id) const
{
    auto const slot_index = slot_of_id(id);
    if (slot_index < 0 || slot_index >= _slots.size() || _slots[slot_index].raw() == nullptr)
        return cc::error("imgui named a texture id the registry never created");

    return _slots[slot_index];
}
} // namespace sr::impl
