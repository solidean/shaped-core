#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-rendering/fwd.hh>

/// The tensor archive Open Image Denoise stores its trained networks in.
///
/// One blob holds every weight and bias of one network, each as a named tensor — `enc_conv0.weight`,
/// `enc_conv0.bias`, and so on — so the network's SHAPE is data rather than something a reader hardcodes.
/// `sr::impl::oidn_network` reads the layer widths straight out of it for that reason.
///
/// The format is small and fixed: a magic, a version, an offset to a table, and one record per tensor naming its
/// dimensions, its layout and its element type.
/// A reader of it is a few dozen lines, which is why this is here rather than in babel — nothing else in the repo
/// reads one, and it is a detail of the denoise member rather than an image format anyone loads.
namespace sr::impl
{
/// A tensor's element type, which OIDN writes as one character.
enum class tza_element : u8
{
    float32,
    float16,
};

/// One named tensor, as a view into the blob it was read from.
///
/// `data` aliases the caller's bytes and does not own them, so the blob has to outlive every tensor read out of it.
/// `dims` is in the order the layout names: `oihw` for a convolution's weights — output channels, input channels,
/// height, width — and one dimension for a bias.
struct tza_tensor
{
    cc::string name;
    cc::vector<i32> dims;
    cc::string layout;
    tza_element element = tza_element::float16;
    cc::span<std::byte const> data;

    /// The product of `dims`, which is how many elements `data` holds.
    [[nodiscard]] i64 element_count() const;
};

/// Every tensor in `blob`, or an empty vector when it is not a tensor archive this reader understands.
///
/// Bounds-checked throughout, because the blob is a file: a truncated or hostile one has to come back empty rather
/// than reading past its end.
/// The tensors alias `blob`, which must outlive them.
[[nodiscard]] cc::vector<tza_tensor> read_tza(cc::span<std::byte const> blob);

/// The tensor of that name, or null.
[[nodiscard]] tza_tensor const* find_tza(cc::span<tza_tensor const> tensors, cc::string_view name);
} // namespace sr::impl
