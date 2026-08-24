#include "viewer_test_env.hh"

#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-viewer/all.hh>
#include <shaped-viewer/resources/impl/mip_layout.hh>

using namespace cc::primitive_defines;

// sv::gpu_resource_manager: the epoch tick every window may call, and the lock the staging group's owner holds
// while a snapshot is bound.
// The layout it is built over is covered without a device in bindless-tables-test.cc.

namespace
{
/// `table` at `count`, plus the two tables the manager requires whatever else is configured.
/// textures_2d is what the texture manager pins into and buffers is what attributes and parameter blocks pin into, so a config
/// omitting either has no manager to build — see gpu_resource_manager::create.
[[nodiscard]] sv::bindless_config only(sv::bindless_table table, u32 count)
{
    auto tables = cc::vector<sv::bindless_table_budget>{{.table = sv::bindless_table::textures_2d, .count = 2},
                                                        {.table = sv::bindless_table::buffers, .count = 2}};
    for (auto& t : tables)
        if (t.table == table)
        {
            t.count = count;
            return {.tables = cc::move(tables)};
        }
    tables.push_back({.table = table, .count = count});
    return {.tables = cc::move(tables)};
}

[[nodiscard]] sg::texture_2d make_texture(sg::context& ctx)
{
    return ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba8_unorm, .width = 8, .height = 8, .usage = sg::texture_usage::readonly_texture});
}
} // namespace

TEST("sv - the resource manager's epoch tick is idempotent")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto m = sv::gpu_resource_manager::create(ctx);
    m.advance_to(ctx.current_epoch());

    // Several windows drawing into one epoch each call it, and only the first one does anything — which is what
    // lets none of them own the tick.
    auto const e = m.current_epoch();
    m.advance_to(ctx.current_epoch());
    m.advance_to(ctx.current_epoch());
    CHECK(m.current_epoch() == e);

    ctx.advance_epoch_and_wait_for_idle();
    m.advance_to(ctx.current_epoch());
    CHECK(m.current_epoch() != e);

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv - the resource manager declares only its configured tables")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto m = sv::gpu_resource_manager::create(ctx, {.bindless = only(sv::bindless_table::textures_2d, 4)});
    CHECK(m.has_table(sv::bindless_table::textures_2d));
    CHECK(m.table_capacity(sv::bindless_table::textures_2d) == 4);

    CHECK(!m.has_table(sv::bindless_table::textures_cube));
    CHECK(m.table_capacity(sv::bindless_table::textures_cube) == 0);
    CHECK_ASSERTS((void)m.acquire_texture(sv::bindless_table::textures_cube, make_texture(ctx).as_readonly_view()));

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv - the resource manager refuses acquires while frozen")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto m = sv::gpu_resource_manager::create(ctx);
    m.advance_to(ctx.current_epoch());

    auto const tex = make_texture(ctx);
    (void)m.acquire_texture(sv::bindless_table::textures_2d, tex.as_readonly_view());

    // The manual pair, which freeze is the RAII form of.
    m.lock();
    CHECK(m.is_locked());
    CHECK_ASSERTS((void)m.acquire_texture(sv::bindless_table::textures_2d, make_texture(ctx).as_readonly_view()));
    CHECK_ASSERTS(m.lock());
    m.unlock();
    CHECK(!m.is_locked());
    CHECK_ASSERTS(m.unlock());

    {
        auto const bound = m.freeze();
        CHECK(m.is_locked());
        CHECK(bound.group() != nullptr);
        CHECK_ASSERTS((void)m.acquire_texture(sv::bindless_table::textures_2d, make_texture(ctx).as_readonly_view()));

        // Advancing under a live snapshot would invalidate the very indices it was taken for.
        CHECK_ASSERTS(m.advance_to(sg::epoch(u64(ctx.current_epoch()) + 1)));
    }
    CHECK(!m.is_locked());

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv - two freezes in one epoch keep the first's indices")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    // The multi-window invariant: window A records against its snapshot, then window B acquires more in the same epoch.
    // B's mints must not disturb what A already handed the GPU, which is what sg's "reclaim only what was NOT
    // acquired this epoch" rule buys.
    auto m = sv::gpu_resource_manager::create(ctx, {.bindless = only(sv::bindless_table::textures_2d, 4)});
    m.advance_to(ctx.current_epoch());

    auto const a = make_texture(ctx);
    auto const a_index = u32(m.acquire_texture(sv::bindless_table::textures_2d, a.as_readonly_view()));
    {
        auto const bound = m.freeze();
        CHECK(bound.elements(sv::bindless_table::textures_2d).size() == 1);
        CHECK(bound.elements(sv::bindless_table::textures_2d)[0] == a_index);
    }

    auto const b = make_texture(ctx);
    auto const b_index = u32(m.acquire_texture(sv::bindless_table::textures_2d, b.as_readonly_view()));
    CHECK(b_index != a_index);
    CHECK(u32(m.acquire_texture(sv::bindless_table::textures_2d, a.as_readonly_view())) == a_index);

    {
        auto const bound = m.freeze();

        // Re-acquiring a resident view is deduplicated: an access declaration naming one element twice is not
        // what a dispatch expects.
        CHECK(bound.elements(sv::bindless_table::textures_2d).size() == 2);

        // A table nobody touched declares nothing, which is a perfectly good thing to declare.
        CHECK(bound.elements(sv::bindless_table::textures_cube).empty());
    }

    // The epoch tick clears the declaration lists, so the next epoch declares what that epoch acquired.
    ctx.advance_epoch_and_wait_for_idle();
    m.advance_to(ctx.current_epoch());
    {
        auto const bound = m.freeze();
        CHECK(bound.elements(sv::bindless_table::textures_2d).empty());
    }

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv - a pinned texture is declared and outlives its epoch")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    // What a material buffer will hold: an index that stays true across epochs, so the buffer can be uploaded
    // once and cached by content hash rather than re-uploaded whenever the tables move.
    auto m = sv::gpu_resource_manager::create(ctx, {.bindless = only(sv::bindless_table::textures_2d, 4)});
    m.advance_to(ctx.current_epoch());

    auto const tex = make_texture(ctx);
    auto const pin = m.pin_texture(sv::bindless_table::textures_2d, tex.as_readonly_view());
    REQUIRE(pin != nullptr);
    auto const index = pin->index();

    // A pinned element belongs in the access declaration even though nothing acquired it this epoch: a dispatch
    // reaches it through the buffer, never through acquire.
    {
        auto const bound = m.freeze();
        CHECK(bound.elements(sv::bindless_table::textures_2d).size() == 1);
        CHECK(bound.elements(sv::bindless_table::textures_2d)[0] == index);
    }

    // Churn the transient working set through later epochs; the pinned index is unmoved.
    for (auto i = 0; i < 3; ++i)
    {
        ctx.advance_epoch_and_wait_for_idle();
        m.advance_to(ctx.current_epoch());
        (void)m.acquire_texture(sv::bindless_table::textures_2d, make_texture(ctx).as_readonly_view());
        CHECK(pin->index() == index);
    }

    ctx.advance_epoch_and_wait_for_idle();
}

namespace
{
/// A `width` x `height` RGBA8 image whose bytes vary with `seed`, plus every mip if `all_mips`.
[[nodiscard]] cc::vector<byte> make_pixels(i32 width, i32 height, u8 seed, bool all_mips)
{
    auto const levels = all_mips ? sv::impl::mip_count_of(width, height) : 1;
    auto bytes = cc::vector<byte>();
    for (auto mip = 0; mip < levels; ++mip)
    {
        auto const size = sv::impl::mip_byte_size(sg::pixel_format::rgba8_unorm, width, height, mip);
        for (auto i = isize(0); i < size; ++i)
            bytes.push_back(byte(u8(i) ^ seed));
    }
    return bytes;
}
} // namespace

TEST("sv - a texture acquire is content-addressed and pins its element")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto m = sv::gpu_resource_manager::create(ctx);
    m.advance_to(ctx.current_epoch());

    auto const base_only = sv::texture_data::create(make_pixels(16, 16, 1, false), sg::pixel_format::rgba8_unorm, 16, 16);
    auto const id = m.acquire_texture(base_only);
    REQUIRE(m.textures.contains(id));

    auto const* const record = m.textures.get_ptr(id);
    REQUIRE(record != nullptr);

    // The chain is allocated in full even though only the base level was supplied, so filling the rest later
    // writes into this texture rather than replacing it — which would move the index a material buffer stored.
    CHECK(record->total_mips == sv::impl::mip_count_of(16, 16));
    CHECK(record->uploaded_mips == 1);
    CHECK(record->state == sv::residency::base_resident);
    CHECK(record->element != nullptr);

    // Same content, same id, no second upload.
    auto const again = sv::texture_data::create(make_pixels(16, 16, 1, false), sg::pixel_format::rgba8_unorm, 16, 16);
    CHECK(m.acquire_texture(again) == id);
    CHECK(m.textures.count() == 1);

    // Different content is a different texture, at a different element.
    auto const other = sv::texture_data::create(make_pixels(16, 16, 2, false), sg::pixel_format::rgba8_unorm, 16, 16);
    auto const other_id = m.acquire_texture(other);
    CHECK(other_id != id);
    CHECK(m.textures.get_ptr(other_id)->index() != record->index());

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv - the same pixels at a different shape are a different texture")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    // The shape is part of the key, not just the bytes: read as 32x8 and as 8x32 these are two textures, and a
    // pool keyed on the bytes alone would hand back the first for the second.
    auto const pixels = make_pixels(32, 8, 3, false);
    auto m = sv::gpu_resource_manager::create(ctx);
    m.advance_to(ctx.current_epoch());

    auto const wide = m.acquire_texture(sv::texture_data::create(pixels, sg::pixel_format::rgba8_unorm, 32, 8));
    auto const tall = m.acquire_texture(sv::texture_data::create(pixels, sg::pixel_format::rgba8_unorm, 8, 32));
    CHECK(wide != tall);
    CHECK(m.textures.count() == 2);

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv - a texture given every mip is complete")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto m = sv::gpu_resource_manager::create(ctx);
    m.advance_to(ctx.current_epoch());

    auto const full = sv::texture_data::create(make_pixels(8, 8, 4, true), sg::pixel_format::rgba8_unorm, 8, 8,
                                               sv::impl::mip_count_of(8, 8));
    auto const id = m.acquire_texture(full);
    auto const* const record = m.textures.get_ptr(id);
    REQUIRE(record != nullptr);

    // Nothing is left for a follow-up to do, which is what `complete` means — the policy asked for mips and got them.
    CHECK(record->state == sv::residency::complete);
    CHECK(record->uploaded_mips == record->total_mips);

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv - a texture's element is declared for the epoch that acquired it")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto m = sv::gpu_resource_manager::create(ctx);
    m.advance_to(ctx.current_epoch());

    auto const id
        = m.acquire_texture(sv::texture_data::create(make_pixels(8, 8, 5, false), sg::pixel_format::rgba8_unorm, 8, 8));
    auto const index = m.textures.get_ptr(id)->index();

    {
        auto const bound = m.freeze();
        CHECK(bound.elements(sv::bindless_table::textures_2d).size() == 1);
        CHECK(bound.elements(sv::bindless_table::textures_2d)[0] == index);
    }

    // Its index is pinned, so it survives an epoch nobody re-acquired it in — that is the whole point of the pin.
    ctx.advance_epoch_and_wait_for_idle();
    m.advance_to(ctx.current_epoch());
    CHECK(m.textures.get_ptr(id)->index() == index);

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv - mip generation is queued, not done inline")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    // record_pending_work drives sr::box_filter_mipmap_routine, whose init blocks on its shader compile — so
    // this needs the shared library and a compiler, not just a device.
    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the mipmap shader");

    auto m = sv::gpu_resource_manager::create(ctx);
    m.advance_to(ctx.current_epoch());

    // An acquire is on the caller's critical path, so the follow-up is queued rather than recorded there.
    auto const id = m.acquire_texture(
        sv::texture_data::create(make_pixels(16, 16, 6, false), sg::pixel_format::rgba8_unorm, 16, 16));
    CHECK(m.pending_work_count() == 1);
    CHECK(m.textures.get_ptr(id)->state == sv::residency::base_resident);

    // Re-acquiring the same content does not queue it a second time.
    (void)m.acquire_texture(
        sv::texture_data::create(make_pixels(16, 16, 6, false), sg::pixel_format::rgba8_unorm, 16, 16));
    CHECK(m.pending_work_count() == 1);

    auto cmd = ctx.create_command_list();
    auto const spent = m.record_pending_work(*cmd);
    ctx.submit_command_list(cc::move(cmd));

    CHECK(spent == sv::impl::mip_count_of(16, 16) - 1);
    CHECK(m.pending_work_count() == 0);
    CHECK(m.textures.get_ptr(id)->state == sv::residency::complete);

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv - the work budget spreads mip generation across epochs")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    // record_pending_work drives sr::box_filter_mipmap_routine, whose init blocks on its shader compile — so
    // this needs the shared library and a compiler, not just a device.
    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the mipmap shader");

    // The microstutter guard: several textures landing at once must not record every chain in one frame.
    // 16x16 is 5 levels, so 4 dispatches each — a budget of 5 admits exactly one per epoch.
    auto m = sv::gpu_resource_manager::create(ctx, {.work = {.max_dispatches_per_epoch = 5}});
    m.advance_to(ctx.current_epoch());

    for (auto seed = u8(0); seed < 3; ++seed)
        (void)m.acquire_texture(
            sv::texture_data::create(make_pixels(16, 16, u8(20 + seed), false), sg::pixel_format::rgba8_unorm, 16, 16));
    CHECK(m.pending_work_count() == 3);

    auto const drain = [&]
    {
        auto cmd = ctx.create_command_list();
        auto const spent = m.record_pending_work(*cmd);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch_and_wait_for_idle();
        m.advance_to(ctx.current_epoch());
        return spent;
    };

    CHECK(drain() == 4);
    CHECK(m.pending_work_count() == 2);
    CHECK(drain() == 4);
    CHECK(m.pending_work_count() == 1);
    CHECK(drain() == 4);
    CHECK(m.pending_work_count() == 0);
    CHECK(drain() == 0);

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv - a texture policy that wants no mips queues nothing")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto m = sv::gpu_resource_manager::create(ctx, {.textures_policy = {.generate_mips = false}});
    m.advance_to(ctx.current_epoch());

    auto const id = m.acquire_texture(
        sv::texture_data::create(make_pixels(16, 16, 7, false), sg::pixel_format::rgba8_unorm, 16, 16));
    CHECK(m.pending_work_count() == 0);

    // It stays at its base level, which is a resolvable state rather than a failure.
    CHECK(m.textures.get_ptr(id)->state == sv::residency::base_resident);

    ctx.advance_epoch_and_wait_for_idle();
}

// --- the material chain's GPU half: attribute upload and the per-instance parameter block ---------------------

namespace
{
/// Both tables the material path pins into, at a size a test can exhaust nothing of.
[[nodiscard]] sv::bindless_config material_tables()
{
    return {.tables = cc::vector<sv::bindless_table_budget>{{.table = sv::bindless_table::textures_2d, .count = 16},
                                                            {.table = sv::bindless_table::buffers, .count = 16}}};
}

[[nodiscard]] sv::mesh_attribute scalar_attribute(cc::string name, sv::attribute_frequency f, f32 a, f32 b, f32 c)
{
    return sv::mesh_attribute::create(cc::move(name), f, cc::vector<f32>{a, b, c});
}

/// The u32 at `offset` of a parameter block, which is how every non-constant slot is read back.
[[nodiscard]] u32 u32_at(cc::span<byte const> bytes, i32 offset)
{
    REQUIRE(offset + isize(sizeof(u32)) <= bytes.size());
    auto const view
        = bytes.subspan(cc::offset_size{.offset = offset, .size = isize(sizeof(u32))}).try_reinterpret_as<u32 const>();
    REQUIRE(view.has_value());
    return view.value()[0];
}
} // namespace

TEST("sv - an attribute is uploaded once and pinned")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto m = sv::gpu_resource_manager::create(ctx, {.bindless = material_tables()});
    m.advance_to(ctx.current_epoch());

    auto const normals = scalar_attribute("roughness", sv::attribute_frequency::per_vertex, 0.1f, 0.2f, 0.3f);
    auto const id = m.attributes.acquire(normals);
    CHECK(m.attributes.count() == 1);

    // Content-keyed on the attribute's own hash, so an unchanged mesh re-acquired every frame costs a lookup.
    CHECK(m.attributes.acquire(normals) == id);
    CHECK(m.attributes.acquire(scalar_attribute("other_name", sv::attribute_frequency::per_vertex, 0.1f, 0.2f, 0.3f))
          == id);
    CHECK(m.attributes.count() == 1);

    auto const& record = m.attributes.get(id);
    CHECK(record.element_count == 3);
    CHECK(record.format == sv::attribute_format_of<f32>);
    CHECK(record.frequency == sv::attribute_frequency::per_vertex);

    // The element is pinned rather than acquired, and that is the whole point: its index has to stay true across the epochs a
    // parameter block holding it is cached over.
    REQUIRE(record.element != nullptr);
    auto const index = record.index();

    ctx.advance_epoch_and_wait_for_idle();
    m.advance_to(ctx.current_epoch());
    CHECK(m.attributes.get(id).index() == index);

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv - a parameter block is filled at the offsets the generated shader reads")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto m = sv::gpu_resource_manager::create(ctx, {.bindless = material_tables()});
    m.advance_to(ctx.current_epoch());

    auto lib = sv::material_library::create();
    sv::register_builtin_material_types(lib);
    auto const pbr = lib.acquire_type(sv::builtin_material::pbr).value();

    // roughness constant on the material, metallic from a per-vertex attribute, base_color from a texture.
    auto overrides = cc::vector<sv::material_attribute_binding>();
    overrides.push_back(sv::material_attribute_binding::of("roughness", 0.25f));
    auto const gold = lib.acquire(sv::material::create("gold", pbr, overrides));

    auto const pixels = cc::vector<byte>::create_filled(4 * 4 * 4, byte(0xFF));
    auto const texture = m.acquire_texture(sv::texture_data::create(pixels, sg::pixel_format::rgba8_unorm, 4, 4));

    auto const positions = cc::vector<tg::pos3f>{tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(0, 1, 0)};
    auto mesh = sv::mesh{.name = "tri", .geometry = sv::triangle_geometry::create_from_positions(positions)};
    mesh.attributes.push_back(scalar_attribute("metallic", sv::attribute_frequency::per_vertex, 0.4f, 0.5f, 0.6f));
    mesh.attributes.push_back(
        sv::mesh_attribute::create("uv", sv::attribute_frequency::per_vertex,
                                   cc::vector<tg::vec2f>{tg::vec2f(0, 0), tg::vec2f(1, 0), tg::vec2f(0, 1)}));
    mesh.textures.push_back({.name = "base_color", .source = {.texture = texture, .uv_attribute = "uv"}});

    auto const resolved = sv::resolve_material(lib, gold, mesh);
    auto const generated = sv::generate_material_shader(resolved);
    auto const bytes = m.build_instance_parameters(resolved, generated.layout);
    CHECK(bytes.size() == generated.layout.size_bytes);
    auto const block = cc::span<byte const>(bytes);

    auto const slot_of = [&](cc::string_view name) -> sv::material_slot const&
    {
        for (auto const& s : generated.layout.slots)
            if (s.name == name)
                return s;
        FAIL("no such slot");
        return generated.layout.slots[0];
    };

    // The constant lands inline, byte for byte.
    auto const rough = cc::span<byte const>(bytes)
                           .subspan(cc::offset_size{.offset = slot_of("roughness").offset, .size = isize(sizeof(f32))})
                           .try_reinterpret_as<f32 const>();
    REQUIRE(rough.has_value());
    CHECK(rough.value()[0] == 0.25f);

    // The mesh-sourced attribute enters as a descriptor: its buffer's bindless index, offset 0, its element stride.
    auto const& metallic = slot_of("metallic");
    CHECK(metallic.kind == sv::material_slot_kind::attribute_descriptor);
    CHECK(u32_at(block, metallic.offset) == m.attributes.get(m.attributes.acquire(mesh.attributes[0])).index());
    CHECK(u32_at(block, metallic.offset + 4) == 0u);
    CHECK(u32_at(block, metallic.offset + 8) == u32(sizeof(f32)));

    // The sampled attribute enters as the texture's PINNED index, plus a descriptor for the uv set it samples through.
    CHECK(u32_at(block, slot_of("base_color").offset) == m.textures.get(texture).index());
    CHECK(u32_at(block, slot_of("base_color.uv").offset + 8) == u32(sizeof(tg::vec2f)));

    // The block is content-cached on parameter_key, so an unchanged mesh re-acquired every frame is a lookup.
    auto const instance = m.acquire_instance(resolved, generated.layout);
    CHECK(m.acquire_instance(resolved, generated.layout) == instance);
    CHECK(m.instances.count() == 1);
    CHECK(m.instances.get(instance).permutation == resolved.permutation_key);

    ctx.advance_epoch_and_wait_for_idle();
}
