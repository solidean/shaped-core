#include <babel-serializer/geometry/gltf.hh>
#include <babel-serializer/geometry/obj.hh>
#include <babel-serializer/geometry/stl.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/string/format.hh>
#include <shaped-viewer/asset/asset_loader.hh>
#include <shaped-viewer/asset/impl/asset_import.hh>
#include <shaped-viewer/material/material_library.hh>

namespace sv
{
cc::optional<asset_format> asset_format_of_uri(cc::string_view uri)
{
    auto const extension = impl::extension_of(uri);
    if (extension == "gltf" || extension == "glb")
        return asset_format::gltf;
    if (extension == "obj")
        return asset_format::obj;
    if (extension == "stl")
        return asset_format::stl;
    return {};
}

cc::result<material_library*> asset_loader::_library() const
{
    if (_config.materials != nullptr)
        return _config.materials;
    return acquire_material_library();
}

cc::result<asset_data> asset_loader::load(cc::string_view uri) const
{
    auto const format = asset_format_of_uri(uri);
    if (!format.has_value())
        return cc::error(cc::format("shaped-viewer: '{}' names no format asset_loader reads", uri));

    auto bytes = _config.resolve ? _config.resolve(uri) : resolve_uri(uri);
    if (bytes.has_error())
        return cc::error(cc::move(bytes).error());

    // The uri is both the asset's name and what its document's own relative references are joined against.
    return load(cc::move(bytes.value()), format.value(), uri, uri);
}

cc::result<asset_data> asset_loader::load(cc::pinned_data<byte const> bytes,
                                          asset_format format,
                                          cc::string_view name,
                                          cc::string_view base_uri) const
{
    switch (format)
    {
    case asset_format::gltf:
    {
        // A glTF's external buffers and images travel the same seam its own bytes did, joined against where those came
        // from — which is what makes a `.gltf` beside its `.bin` load with no ceremony.
        auto resolve = [&](cc::string_view uri) -> cc::result<cc::pinned_data<byte const>>
        {
            auto const joined = impl::join_uri(base_uri, uri);
            return _config.resolve ? _config.resolve(joined) : resolve_uri(joined);
        };

        auto doc = babel::gltf::read(cc::move(bytes), {.resolve_uri = resolve});
        if (doc.has_error())
            return cc::error(cc::move(doc).error());
        return load(doc.value(), name);
    }

    case asset_format::obj:
    {
        auto doc = babel::obj::read(bytes.span());
        if (doc.has_error())
            return cc::error(cc::move(doc).error());
        return load(doc.value(), name);
    }

    case asset_format::stl:
    {
        auto doc = babel::stl::read(bytes.span());
        if (doc.has_error())
            return cc::error(cc::move(doc).error());
        return load(doc.value(), name);
    }
    }
    return cc::error("shaped-viewer: unknown asset_format");
}

cc::result<asset_data> asset_loader::load(babel::gltf::data const& doc, cc::string_view name) const
{
    auto lib = _library();
    if (lib.has_error())
        return cc::error(cc::move(lib).error());
    return impl::import_gltf(doc, _config, *lib.value(), name);
}

cc::result<asset_data> asset_loader::load(babel::obj::data const& doc, cc::string_view name) const
{
    auto lib = _library();
    if (lib.has_error())
        return cc::error(cc::move(lib).error());
    return impl::import_obj(doc, _config, *lib.value(), name);
}

cc::result<asset_data> asset_loader::load(babel::stl::data const& doc, cc::string_view name) const
{
    auto lib = _library();
    if (lib.has_error())
        return cc::error(cc::move(lib).error());
    return impl::import_stl(doc, _config, *lib.value(), name);
}
} // namespace sv
