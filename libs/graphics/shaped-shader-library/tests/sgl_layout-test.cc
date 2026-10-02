#include <clean-core/common/utility.hh>
#include <clean-core/container/fixed_array.hh>
#include <nexus/test.hh>
#include <slib_test_sgl_shaders.hh>
#include <typed-geometry/linalg/vec.hh>

using namespace cc::primitive_defines;

// `@layout(.cpp)` blocks of host_layout.sgl: placed as C++ places a struct of the generated host types, so what the
// package generates is the struct a host writes by hand, byte for byte.

namespace
{
namespace shaders = slib_test::sgl_shaders;

/// What a host writes for `host_push` with no knowledge of HLSL's rows.
struct plain_push
{
    float scale;
    tg::vec3f tint;
    float bias;
    tg::vec2f shift;
    tg::vec4f weights;
    u32 count;
};

/// What a host writes for the plain members of `host_group`.
struct plain_group_constants
{
    float gain;
    tg::vec3f dir;
    float after;
};
} // namespace

// The generated inline struct is the plain one, member for member, and no padding is added to it.
static_assert(sizeof(shaders::host_push) == sizeof(plain_push));
static_assert(alignof(shaders::host_push) == alignof(plain_push));
static_assert(offsetof(shaders::host_push, scale) == offsetof(plain_push, scale));
static_assert(offsetof(shaders::host_push, tint) == offsetof(plain_push, tint));
static_assert(offsetof(shaders::host_push, bias) == offsetof(plain_push, bias));
static_assert(offsetof(shaders::host_push, shift) == offsetof(plain_push, shift));
static_assert(offsetof(shaders::host_push, weights) == offsetof(plain_push, weights));
static_assert(offsetof(shaders::host_push, count) == offsetof(plain_push, count));
static_assert(shaders::host_push::block_size == sizeof(plain_push));

// The group's constant buffer is the plain struct of its plain members.
static_assert(shaders::host_group::constants_size == sizeof(plain_group_constants));

TEST("slib sgl layout - a @layout(.cpp) inline block is the bytes of the host's own struct")
{
    auto const plain = plain_push{.scale = 1.0f,
                                  .tint = tg::vec3f(2, 3, 4),
                                  .bias = 5.0f,
                                  .shift = tg::vec2f(6, 7),
                                  .weights = tg::vec4f(8, 9, 10, 11),
                                  .count = 12};
    auto const generated = shaders::host_push{.scale = plain.scale,
                                              .tint = plain.tint,
                                              .bias = plain.bias,
                                              .shift = plain.shift,
                                              .weights = plain.weights,
                                              .count = plain.count};
    auto const block = generated.to_block();
    CHECK(cc::memcmp(block.data(), &plain, sizeof(plain)) == 0);
}

TEST("slib sgl layout - a @layout(.cpp) group writes its constants as the host's own struct holds them")
{
    auto const plain = plain_group_constants{.gain = 1.0f, .dir = tg::vec3f(2, 3, 4), .after = 5.0f};
    auto const group = shaders::host_group{.gain = plain.gain, .dir = plain.dir, .after = plain.after, .values = {}};
    auto block = cc::fixed_array<byte, shaders::host_group::constants_size>{};
    group.write_constants(block);
    CHECK(cc::memcmp(block.data(), &plain, sizeof(plain)) == 0);
}
