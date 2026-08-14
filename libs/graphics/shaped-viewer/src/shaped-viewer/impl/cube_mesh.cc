#include <shaped-viewer/impl/cube_mesh.hh>

namespace sv::impl
{
namespace
{
/// Appends a quad (triangles a-b-c and a-c-d) carrying material `m` to a cube.
/// The closest-hit shades two-sided, so winding is irrelevant.
void push_quad(cube_mesh& cube, tg::pos3f a, tg::pos3f b, tg::pos3f c, tg::pos3f d, pbr_material const& m)
{
    cube.positions.push_back(a);
    cube.positions.push_back(b);
    cube.positions.push_back(c);
    cube.positions.push_back(a);
    cube.positions.push_back(c);
    cube.positions.push_back(d);
    cube.materials.push_back(m);
    cube.materials.push_back(m);
}

[[nodiscard]] pbr_material face_material(tg::vec3f color)
{
    return {.base_color = color, .metallic = 0.0f, .roughness = 0.6f, .emissive = tg::vec3f(0, 0, 0)};
}
} // namespace

cube_mesh make_cube(float half_extent)
{
    auto const h = half_extent;
    auto const p = [&](float x, float y, float z) { return tg::pos3f(x * h, y * h, z * h); };

    auto cube = cube_mesh{};
    cube.positions.reserve(36);
    cube.materials.reserve(12);

    // -z (front, toward the default camera) and +z (back)
    push_quad(cube, p(-1, -1, -1), p(1, -1, -1), p(1, 1, -1), p(-1, 1, -1),
              face_material(tg::vec3f(0.85f, 0.25f, 0.22f)));
    push_quad(cube, p(-1, -1, 1), p(-1, 1, 1), p(1, 1, 1), p(1, -1, 1), face_material(tg::vec3f(0.20f, 0.55f, 0.85f)));
    // -x (left) and +x (right)
    push_quad(cube, p(-1, -1, -1), p(-1, 1, -1), p(-1, 1, 1), p(-1, -1, 1),
              face_material(tg::vec3f(0.30f, 0.75f, 0.35f)));
    push_quad(cube, p(1, -1, -1), p(1, -1, 1), p(1, 1, 1), p(1, 1, -1), face_material(tg::vec3f(0.90f, 0.75f, 0.20f)));
    // -y (bottom) and +y (top)
    push_quad(cube, p(-1, -1, -1), p(-1, -1, 1), p(1, -1, 1), p(1, -1, -1),
              face_material(tg::vec3f(0.55f, 0.35f, 0.75f)));
    push_quad(cube, p(-1, 1, -1), p(1, 1, -1), p(1, 1, 1), p(-1, 1, 1), face_material(tg::vec3f(0.85f, 0.85f, 0.85f)));

    return cube;
}
} // namespace sv::impl
