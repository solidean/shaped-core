#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/ir/dxil/dxil.hh>

using namespace cc::primitive_defines;

namespace
{
/// `20 + n` bytes counting up by 7 from 3, as the expected digests below were computed over.
[[nodiscard]] cc::vector<byte> counted(isize n)
{
    auto bytes = cc::vector<byte>();
    for (auto i = isize(0); i < 20 + n; ++i)
        bytes.push_back(byte((i * 7 + 3) & 0xff));
    return bytes;
}

[[nodiscard]] cc::string hex_of(cc::fixed_array<byte, 16> const& digest)
{
    auto text = cc::string();
    for (auto const b : digest)
        text.appendf("{:02x}", u32(u8(b)));
    return text;
}
} // namespace

// The expected digests are DXBC's checksum as an independent implementation computes it, one that matched dxil.dll's
// digest on five containers DXC signed.
TEST("sgl dxil - a container's digest is DXBC's checksum, whichever way its last block falls")
{
    struct row
    {
        isize length;
        cc::string_view digest;
    };
    row const rows[] = {
        {0, "140d60f6b775e2ba4e4abed401b2e9a1"},   // nothing past the digest
        {36, "5b24515b756d32e1547d0aebfa221b48"},  // a last block with room for both counts
        {60, "119aa1c913917afdca9545e7470f7ca3"},  // one too full, so the counts take a block of their own
        {64, "c1f54b1b564c9ad315e16f47b5ffcb18"},  // a whole block, then the counts
        {100, "44f95f1857d4157e4bab42d56b9fa322"}, // a block and a short one
        {128, "92f40506414668174c7615900f9394b2"}, // two whole blocks
    };
    for (auto const& r : rows)
        CHECK(hex_of(sgl::ir::dxbc_digest(counted(r.length))) == r.digest).context(cc::format("{} bytes", r.length));
}

TEST("sgl dxil - the digest covers every byte past it and none before")
{
    auto const base = counted(100);
    auto const digest = sgl::ir::dxbc_digest(base);
    for (auto const at : {isize(0), isize(4), isize(19)})
    {
        auto changed = base;
        changed[at] = byte(u8(changed[at]) ^ 0x5a);
        CHECK(sgl::ir::dxbc_digest(changed) == digest).context(cc::format("byte {}", at));
    }
    for (auto const at : {isize(20), isize(70), isize(119)})
    {
        auto changed = base;
        changed[at] = byte(u8(changed[at]) ^ 1);
        CHECK(sgl::ir::dxbc_digest(changed) != digest).context(cc::format("byte {}", at));
    }
}
