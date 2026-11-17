#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-shader-compiler-dxc/compiler.hh>

using namespace cc::primitive_defines;

// The draw's start under SGL's `root_constant` fill of `draw_start` (EMIT-128, EMIT-267): a dx12 vertex entry reads it
// from the block `sgl_draw_start` at `b1, space9`, at Shader Model 6.6. SGL's own DXIL passes the validator and states
// the block as DXC's compile of SGL's text does, which DXC builds at the 6.6 profile and reflects at that address.

namespace
{
/// A vertex entry that reads each of the three inputs the draw's start reaches.
constexpr auto k_draws = cc::string_view("require first_instance\n"
                                         "struct varyings:\n"
                                         "    @position position: hpos4\n"
                                         "\n"
                                         "@vertex fun vs(@vertex_index v: int, @instance_index i: int, "
                                         "@first_instance f: int) -> varyings:\n"
                                         "    return { position = hpos4(v as float, i as float, f as float, 1.0) }\n"
                                         "\n"
                                         "@vertex fun unread(@vertex_index v: int) -> varyings:\n"
                                         "    return { position = hpos4(0.0, 0.0, 0.0, 1.0) }\n");

[[nodiscard]] cc::result<sgl::emitted_source, cc::string> under_the_fill(cc::string_view entry,
                                                                         sgl::emit::native_route route)
{
    return sgl::compile_to_text(
        {.source = k_draws,
         .entry_point = entry,
         .target = sgl::emit::target::hlsl_dx12,
         .core = {.draw_start = sgl::check::fill::root_constant, .wave_size = sgl::check::fill::none},
         .native = route});
}

/// The size a disassembly's resource metadata states of the constant buffer at `b1, space9`; -1 where it states none.
[[nodiscard]] i64 draw_start_size(cc::string_view disassembly)
{
    // a CBV's record: id, type, name, space, lower bound, range, size, extra
    constexpr auto k_at = cc::string_view("i32 9, i32 1, i32 1, i32 ");
    auto const found = disassembly.find(k_at);
    if (found < 0)
        return -1;
    auto size = i64(0);
    for (auto i = found + k_at.size(); i < disassembly.size() && disassembly[i] >= '0' && disassembly[i] <= '9'; ++i)
        size = size * 10 + (disassembly[i] - '0');
    return size;
}
} // namespace

TEST("slib - SGL's DXIL of the root-constant fill reads the draw's start from b1, space9 at 6.6, as DXC's of its text "
     "(EMIT-128)")
{
#ifndef CC_OS_WINDOWS
    SKIP("a DXIL compile reflects through the Windows SDK's d3d12shader.h, which the Linux DXC release does not ship");
#endif
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");

    auto const native = under_the_fill("vs", sgl::emit::native_route::require);
    REQUIRE(native.has_value()).dump("error", native.has_error() ? native.error() : cc::string());
    auto const refused = compiler.value().validate(native.value().dxil);
    CHECK(!refused.has_value()).dump("validator", refused.value_or(cc::string()));
    auto const ours = compiler.value().disassemble(native.value().dxil);
    REQUIRE(ours.has_value());
    CHECK(ours.value().contains("!{!\"vs\", i32 6, i32 6}")).dump("ours", ours.value());
    CHECK(ours.value().contains("@dx.op.cbufferLoadLegacy.i32")).dump("ours", ours.value());
    CHECK(!ours.value().contains("startVertexLocation")).dump("ours", ours.value());
    CHECK(!ours.value().contains("startInstanceLocation")).dump("ours", ours.value());

    // DXC builds the text at the 6.6 profile, and reflects the block where SGL lists it
    auto const text = under_the_fill("vs", sgl::emit::native_route::text);
    REQUIRE(text.has_value());
    auto options = ssc::dxc::compile_options();
    options.extra_args.push_back(cc::string("-enable-16bit-types"));
    auto const built = compiler.value().compile({.source = text.value().text,
                                                 .entry_point = text.value().entry_point,
                                                 .stage = sg::shader_stage::vertex,
                                                 .model = ssc::dxc::shader_model::sm_6_6},
                                                options);
    REQUIRE(built.has_value()).dump("dxc", built.has_error() ? built.error().to_string() : cc::string());
    auto reflected = 0;
    for (auto const& b : built.value().bindings)
        if (b.space == 9u && b.index == 1)
        {
            ++reflected;
            CHECK(b.type == sg::binding_type::constants_buffer);
            CHECK(b.block_size.has_value());
        }
    CHECK(reflected == 1);
    auto const theirs = compiler.value().disassemble(built.value().bytecode);
    REQUIRE(theirs.has_value());
    CHECK(!theirs.value().contains("startVertexLocation"));
    // the size DXC's metadata states of the block, which SGL's states too
    auto const size = draw_start_size(theirs.value());
    CHECK(size > 0).dump("theirs", theirs.value());
    CHECK(draw_start_size(ours.value()) == size).dump("ours", ours.value()).dump("theirs", theirs.value());
}

TEST("slib - SGL's DXIL of an entry that takes the draw's start and reads none declares no block of it (EMIT-267)")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto const native = under_the_fill("unread", sgl::emit::native_route::require);
    REQUIRE(native.has_value()).dump("error", native.has_error() ? native.error() : cc::string());
    CHECK(!compiler.value().validate(native.value().dxil).has_value());
    auto const ours = compiler.value().disassemble(native.value().dxil);
    REQUIRE(ours.has_value());
    CHECK(draw_start_size(ours.value()) == -1).dump("ours", ours.value());
    // the slot is listed all the same, unused, so a host lays it out and may skip writing it
    auto listed = 0;
    for (auto const& b : native.value().bindings)
        if (b.reserved == sgl::reserved_slot::draw_start)
        {
            ++listed;
            CHECK(!b.is_used);
        }
    CHECK(listed == 1);
}

namespace
{
/// A vertex entry that reads its `@inline` block, at `b0, space9`, beside the draw's start, at `b1, space9`: the
/// block's first row and the draw's start's are read in one basic block.
constexpr auto k_inline_beside = cc::string_view("@inline binding tuning:\n"
                                                 "    offset: int\n"
                                                 "    scale: float\n"
                                                 "\n"
                                                 "struct varyings:\n"
                                                 "    @position position: hpos4\n"
                                                 "\n"
                                                 "@vertex fun vs(@vertex_index v: int, @instance_index i: int){tuning} "
                                                 "-> varyings:\n"
                                                 "    let x = ((v + tuning.offset) as float) * tuning.scale\n"
                                                 "    return { position = hpos4(x, i as float, 0.0, 1.0) }\n");

/// How often `text` holds `part`.
[[nodiscard]] isize count_of(cc::string_view text, cc::string_view part)
{
    auto n = isize(0);
    for (auto at = text.find(part); at >= 0; at = text.find(part, at + part.size()))
        ++n;
    return n;
}
} // namespace

TEST("slib - an entry reading its @inline block and the draw's start binds both blocks of space 9 apart (EMIT-267)")
{
#ifndef CC_OS_WINDOWS
    SKIP("a DXIL compile reflects through the Windows SDK's d3d12shader.h, which the Linux DXC release does not ship");
#endif
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto const request = [&](sgl::emit::native_route route)
    {
        return sgl::compile_to_text(
            {.source = k_inline_beside,
             .entry_point = "vs",
             .target = sgl::emit::target::hlsl_dx12,
             .core = {.draw_start = sgl::check::fill::root_constant, .wave_size = sgl::check::fill::none},
             .native = route});
    };

    // the interface lists the program's @inline block and the reserved draw start, each once
    auto const native = request(sgl::emit::native_route::require);
    REQUIRE(native.has_value()).dump("error", native.has_error() ? native.error() : cc::string());
    auto inline_blocks = 0;
    auto draw_starts = 0;
    for (auto const& b : native.value().bindings)
    {
        inline_blocks += b.is_inline ? 1 : 0;
        if (b.reserved == sgl::reserved_slot::draw_start)
        {
            ++draw_starts;
            CHECK(b.is_used);
            CHECK(b.group == 9);
            CHECK(b.slot == 1);
        }
    }
    CHECK(inline_blocks == 1);
    CHECK(draw_starts == 1);

    // SGL's DXIL: two constant buffers in space 9, each loaded through its own handle, which the validator takes
    auto const refused = compiler.value().validate(native.value().dxil);
    CHECK(!refused.has_value()).dump("validator", refused.value_or(cc::string()));
    auto const ours = compiler.value().disassemble(native.value().dxil);
    REQUIRE(ours.has_value());
    CHECK(draw_start_size(ours.value()) == 8).dump("ours", ours.value());
    CHECK(ours.value().contains("i32 9, i32 0, i32 1, i32 ")).dump("ours", ours.value());
    CHECK(count_of(ours.value(), "call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32") >= 2).dump("ours", ours.value());
    // a negative base vertex arrives as its bits, so the addition must wrap: no flag may make an overflow poison
    CHECK(!ours.value().contains("add nsw")).dump("ours", ours.value());
    CHECK(!ours.value().contains("add nuw")).dump("ours", ours.value());

    // the text: DXC builds it at 6.6 and reflects the two blocks at b0 and b1 of space 9
    auto const text = request(sgl::emit::native_route::text);
    REQUIRE(text.has_value()).dump("error", text.has_error() ? text.error() : cc::string());
    CHECK(text.value().text.contains("register(b0, space9)")).dump("text", text.value().text);
    CHECK(text.value().text.contains("register(b1, space9)")).dump("text", text.value().text);
    auto options = ssc::dxc::compile_options();
    options.extra_args.push_back(cc::string("-enable-16bit-types"));
    auto const built = compiler.value().compile({.source = text.value().text,
                                                 .entry_point = text.value().entry_point,
                                                 .stage = sg::shader_stage::vertex,
                                                 .model = ssc::dxc::shader_model::sm_6_6},
                                                options);
    REQUIRE(built.has_value()).dump("dxc", built.has_error() ? built.error().to_string() : cc::string());
    auto at_b0 = 0;
    auto at_b1 = 0;
    for (auto const& b : built.value().bindings)
        if (b.space == 9u)
        {
            CHECK(b.type == sg::binding_type::constants_buffer);
            at_b0 += b.index == 0 ? 1 : 0;
            at_b1 += b.index == 1 ? 1 : 0;
        }
    CHECK(at_b0 == 1);
    CHECK(at_b1 == 1);
    auto const theirs = compiler.value().disassemble(built.value().bytecode);
    REQUIRE(theirs.has_value());
    CHECK(draw_start_size(theirs.value()) == 8).dump("theirs", theirs.value());
}
