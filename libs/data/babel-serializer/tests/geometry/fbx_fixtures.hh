#pragma once

#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

// Small FBX scenes written out as ASCII FBX 7.4, for the tests that read them.
//
// Header-only, so another library's tests can import one too: babel's `BABEL_TEST_DIR` is the include root.
// No FBX writer exists to produce these, and a binary scene is not something to hand-edit, so the ASCII encoding is the one the fixtures are in; ufbx reads both through the same object model.

namespace babel::test
{
struct fbx_settings;
} // namespace babel::test

/// The coordinate system and unit a fixture declares in its `GlobalSettings`, as FBX spells them:
/// an axis is 0 / 1 / 2 for X / Y / Z, a sign is +1 or -1, and `unit_scale_factor` is centimeters per unit.
struct babel::test::fbx_settings
{
    int up_axis = 1;
    int up_sign = 1;
    int front_axis = 2;
    int front_sign = 1;
    int coord_axis = 0;
    int coord_sign = 1;
    double unit_scale_factor = 1;
};

namespace babel::test
{
/// The scene every fixture shares, below its `GlobalSettings`.
///
/// - One mesh, "shape": a quad (0,0,0)-(1,1,0) and a triangle beside it, with per-corner normals, one uv set "map1"
///   and per-face materials: the quad is slot 0, the triangle slot 1.
/// - Two nodes INSTANCING it: "parent" at (10,0,0) scaled by 2, with slots [red, glow]; and "child" under it at
///   (0,5,0) in the parent's space, with the slots the other way round, [glow, red].
/// - A point light "lamp" on node "lamp" at (0,0,3): color (1, 0.5, 0.25), FBX Intensity 250, quadratic decay.
/// - Material "red": Phong, DiffuseColor (1,0,0), Shininess 20, a texture "wood" on DiffuseColor.
/// - Material "glow": Lambert, DiffuseColor (0,0,1), EmissiveColor (1,1,0), EmissiveFactor 4.
/// - Texture "wood": relative `textures\wood.png`, absolute `C:/somewhere/textures/wood.png`, no embedded content.
inline constexpr cc::string_view fbx_scene_objects = R"(
Objects:  {
	Geometry: 100, "Geometry::shape", "Mesh" {
		Vertices: *15 {
			a: 0,0,0,1,0,0,1,1,0,0,1,0,2,0,0
		}
		PolygonVertexIndex: *7 {
			a: 0,1,2,-4,1,4,-3
		}
		GeometryVersion: 124
		LayerElementNormal: 0 {
			Version: 102
			Name: ""
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "Direct"
			Normals: *21 {
				a: 0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1
			}
		}
		LayerElementUV: 0 {
			Version: 101
			Name: "map1"
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "IndexToDirect"
			UV: *8 {
				a: 0,0,1,0,1,1,0,1
			}
			UVIndex: *7 {
				a: 0,1,2,3,0,1,2
			}
		}
		LayerElementMaterial: 0 {
			Version: 101
			Name: ""
			MappingInformationType: "ByPolygon"
			ReferenceInformationType: "IndexToDirect"
			Materials: *2 {
				a: 0,1
			}
		}
		Layer: 0 {
			Version: 100
			LayerElement:  {
				Type: "LayerElementNormal"
				TypedIndex: 0
			}
			LayerElement:  {
				Type: "LayerElementUV"
				TypedIndex: 0
			}
			LayerElement:  {
				Type: "LayerElementMaterial"
				TypedIndex: 0
			}
		}
	}
	NodeAttribute: 200, "NodeAttribute::lamp", "Light" {
		Properties70:  {
			P: "LightType", "enum", "", "",0
			P: "Color", "Color", "", "A",1,0.5,0.25
			P: "Intensity", "Number", "", "A",250
			P: "DecayType", "enum", "", "",2
		}
		TypeFlags: "Light"
		GeometryVersion: 124
	}
	Model: 300, "Model::parent", "Mesh" {
		Version: 232
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",10,0,0
			P: "Lcl Scaling", "Lcl Scaling", "", "A",2,2,2
		}
		Shading: T
		Culling: "CullingOff"
	}
	Model: 301, "Model::child", "Mesh" {
		Version: 232
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",0,5,0
		}
		Shading: T
		Culling: "CullingOff"
	}
	Model: 302, "Model::lamp", "Light" {
		Version: 232
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",0,0,3
		}
	}
	Material: 400, "Material::red", "" {
		Version: 102
		ShadingModel: "phong"
		Properties70:  {
			P: "DiffuseColor", "Color", "", "A",1,0,0
			P: "Shininess", "double", "Number", "",20
		}
	}
	Material: 401, "Material::glow", "" {
		Version: 102
		ShadingModel: "lambert"
		Properties70:  {
			P: "DiffuseColor", "Color", "", "A",0,0,1
			P: "EmissiveColor", "Color", "", "A",1,1,0
			P: "EmissiveFactor", "Number", "", "A",4
		}
	}
	Texture: 500, "Texture::wood", "" {
		Type: "TextureVideoClip"
		Version: 202
		TextureName: "Texture::wood"
		FileName: "C:/somewhere/textures/wood.png"
		RelativeFilename: "textures\wood.png"
	}
}
Connections:  {
	C: "OO",300,0
	C: "OO",301,300
	C: "OO",302,0
	C: "OO",100,300
	C: "OO",100,301
	C: "OO",200,302
	C: "OO",400,300
	C: "OO",401,300
	C: "OO",401,301
	C: "OO",400,301
	C: "OP",500,400, "DiffuseColor"
}
)";

/// The shared scene under the given `GlobalSettings`, as the text of an ASCII FBX file.
[[nodiscard]] inline cc::string fbx_scene(fbx_settings s = {})
{
    auto const property = [](cc::string_view name, auto value)
    { return cc::format("\t\tP: \"{}\", \"int\", \"Integer\", \"\",{}\n", name, value); };

    auto out = cc::string("; FBX 7.4.0 project file\n"
                          "FBXHeaderExtension:  {\n"
                          "\tFBXHeaderVersion: 1003\n"
                          "\tFBXVersion: 7400\n"
                          "\tCreator: \"shaped-core test\"\n"
                          "}\n"
                          "GlobalSettings:  {\n"
                          "\tVersion: 1000\n"
                          "\tProperties70:  {\n");
    out += property("UpAxis", s.up_axis);
    out += property("UpAxisSign", s.up_sign);
    out += property("FrontAxis", s.front_axis);
    out += property("FrontAxisSign", s.front_sign);
    out += property("CoordAxis", s.coord_axis);
    out += property("CoordAxisSign", s.coord_sign);
    out += cc::format("\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",{}\n", s.unit_scale_factor);
    out += "\t}\n}\n";
    out += fbx_scene_objects;
    return out;
}
} // namespace babel::test
