#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/barrier/resource_access.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/subresource.hh>

/// The subresources of one texture its async transfers in flight hold in the transfer's layout, `general`.
/// Pure logic, held by a backend's texture under a lock, so it is unit-testable without a device.
///
/// A command list touching a texture with a transfer in flight waits for the transfer and so runs after it on the GPU,
/// but the validation layers read submit-call order, and that list's entry barrier is submitted before the copy
/// of a transfer whose actor has not got to it yet: moving the copied subresource out of `general` there would make
/// the copy name a layout the resource has left.
/// So an access to a pinned subresource stays in `general`, where the access allows it: sampling and copies.
///
/// What it must not pin is everything else.
/// `general` is COMMON on dx12, which takes no render-target, depth or storage access, and vulkan's render pass names
/// an attachment layout of its own: a list writing a texture's other levels while one uploads, as a mip chain filled
/// on the device from an uploaded level 0 does, recorded an invalid barrier there.
/// Such an access keeps its own layout even on a pinned subresource, which the GPU still orders after the transfer.
struct sg::impl::transfer_pin
{
    /// One box holding every subresource a transfer in flight covers; none once they all completed.
    cc::optional<sg::subresource_range> range;

    /// A transfer of `r` is being enqueued, `others_pending` saying whether one before it is still in flight.
    /// The box restarts at `r` when none is, so a long-finished transfer pins nothing.
    void note(sg::subresource_range const& r, bool others_pending)
    {
        if (!others_pending || !range.has_value())
        {
            range = r;
            return;
        }
        auto const join = [](cc::start_end a, cc::start_end b)
        { return cc::start_end{.start = cc::min(a.start, b.start), .end = cc::max(a.end, b.end)}; };
        auto& box = range.value();
        box = sg::subresource_range(join(box.mip_range, r.mip_range), join(box.array_range, r.array_range),
                                    join(box.aspect_range, r.aspect_range));
    }

    /// The layout a list's access to `r` takes, `requested` being the one the access asks for,
    /// and `pending` whether any transfer of the texture is still in flight.
    [[nodiscard]] sg::texture_layout layout_for(sg::subresource_range const& r,
                                                sg::texture_layout requested,
                                                bool pending) const
    {
        if (!pending || !range.has_value() || !overlaps(range.value(), r))
            return requested;
        switch (requested)
        {
        case sg::texture_layout::shader_texture:
        case sg::texture_layout::copy_src:
        case sg::texture_layout::copy_dst:
        case sg::texture_layout::general:
            return sg::texture_layout::general;
        default:
            return requested;
        }
    }

    [[nodiscard]] static bool overlaps(sg::subresource_range const& a, sg::subresource_range const& b)
    {
        auto const meet = [](cc::start_end x, cc::start_end y) { return x.start < y.end && y.start < x.end; };
        return meet(a.mip_range, b.mip_range) && meet(a.array_range, b.array_range)
            && meet(a.aspect_range, b.aspect_range);
    }
};
