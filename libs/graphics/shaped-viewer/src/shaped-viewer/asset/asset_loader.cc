#include <babel-serializer/geometry/gltf.hh>
#include <babel-serializer/geometry/obj.hh>
#include <babel-serializer/geometry/stl.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <shaped-viewer/asset/asset.hh>
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
    auto definitions = cc::vector<impl::asset_material_definition>();
    return _finish(_import(cc::move(bytes), format, name, base_uri, definitions), definitions);
}

cc::result<asset_data> asset_loader::_import(cc::pinned_data<byte const> bytes,
                                             asset_format format,
                                             cc::string_view name,
                                             cc::string_view base_uri,
                                             cc::vector<impl::asset_material_definition>& definitions) const
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
        return impl::import_gltf(doc.value(), _config, name, definitions);
    }

    case asset_format::obj:
    {
        auto doc = babel::obj::read(bytes.span());
        if (doc.has_error())
            return cc::error(cc::move(doc).error());
        return impl::import_obj(doc.value(), _config, name, definitions);
    }

    case asset_format::stl:
    {
        auto doc = babel::stl::read(bytes.span());
        if (doc.has_error())
            return cc::error(cc::move(doc).error());
        return impl::import_stl(doc.value(), _config, name, definitions);
    }
    }
    return cc::error("shaped-viewer: unknown asset_format");
}

sv::asset asset_loader::load_async(cc::string_view uri) const
{
    // Captured by value, so the node needs nothing of this call to survive; the LOADER must, and says so.
    auto owned_uri = cc::string(uri);

    // One frame for the whole worker half — fetch, parse, import — rather than a node per stage.
    //
    // Three nodes would express the stages but buy nothing yet: each stage feeds only the next, so there is no
    // parallelism to expose and nothing else to depend on an intermediate.
    // What splitting them WOULD buy is structure-first loading, and that needs a mesh whose geometry has not been read
    // — see the note on `sv::asset`.
    auto node = cc::make_async_scheduled<impl::imported_asset>(
        [this, owned_uri = cc::move(owned_uri)](cc::async_context<impl::imported_asset>& ctx)
        {
            auto const format = asset_format_of_uri(owned_uri);
            if (!format.has_value())
                return ctx.resolve_to_error(cc::async_error::make_error(
                    cc::any_error(cc::format("shaped-viewer: '{}' names no format asset_loader reads", owned_uri))));

            auto bytes = _config.resolve ? _config.resolve(owned_uri) : resolve_uri(owned_uri);
            if (bytes.has_error())
                return ctx.resolve_to_error(cc::async_error::make_error(cc::move(bytes).error()));

            auto out = impl::imported_asset();
            auto imported = _import(cc::move(bytes.value()), format.value(), owned_uri, owned_uri, out.definitions);
            if (imported.has_error())
                return ctx.resolve_to_error(cc::async_error::make_error(cc::move(imported).error()));

            out.data = cc::move(imported.value());
            return ctx.resolve_to_value(cc::move(out));
        });

    return sv::asset(cc::move(node), this);
}

cc::result<asset_data> asset_loader::_finish(cc::result<asset_data> imported,
                                             cc::vector<impl::asset_material_definition> const& definitions) const
{
    if (imported.has_error())
        return imported;

    auto lib = _library();
    if (lib.has_error())
        return cc::error(cc::move(lib).error());

    impl::acquire_asset_materials(imported.value(), definitions, _config, *lib.value());
    return imported;
}

cc::result<asset_data> asset_loader::load(babel::gltf::data const& doc, cc::string_view name) const
{
    auto definitions = cc::vector<impl::asset_material_definition>();
    return _finish(impl::import_gltf(doc, _config, name, definitions), definitions);
}

cc::result<asset_data> asset_loader::load(babel::obj::data const& doc, cc::string_view name) const
{
    auto definitions = cc::vector<impl::asset_material_definition>();
    return _finish(impl::import_obj(doc, _config, name, definitions), definitions);
}

cc::result<asset_data> asset_loader::load(babel::stl::data const& doc, cc::string_view name) const
{
    auto definitions = cc::vector<impl::asset_material_definition>();
    return _finish(impl::import_stl(doc, _config, name, definitions), definitions);
}
} // namespace sv
