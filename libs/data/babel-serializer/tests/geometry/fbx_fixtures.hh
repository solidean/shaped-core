#pragma once

#include <babel-serializer/fwd.hh> // babel's vocabulary aliases: byte, i32, f64, ...
#include <clean-core/bytes/compression.hh>
#include <clean-core/common/utility.hh> // cc::memcpy, cc::move
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
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
class fbx_binary_writer;
} // namespace babel::test

/// A minimal writer for BINARY FBX 7.4, so a test can build a scene in the encoding real files use without committing one.
///
/// Records nest through `begin` / `end`, and a property is appended to the record opened last, before its first child.
/// Arrays are written zlib-compressed, which is the path every DCC exporter takes and the one the ASCII fixtures never reach.
/// `array_count` overrides the element count an array STATES, which is how a test asks ufbx for more than the bytes hold.
class babel::test::fbx_binary_writer
{
public:
    fbx_binary_writer()
    {
        for (auto const c : cc::string_view("Kaydara FBX Binary  "))
            _out.push_back(byte(c));
        _out.push_back(byte(0));
        _out.push_back(byte(0x1a));
        _out.push_back(byte(0));
        put_u32(_out, 7400);
    }

    void begin(cc::string_view name)
    {
        if (!_open.empty())
            _open.back().has_children = true;
        flush_properties();
        _open.push_back({.record_start = _out.size()});
        for (auto i = 0; i < 12; ++i)
            _out.push_back(byte(0)); // end offset, property count and property bytes, patched by flush and end
        _out.push_back(byte(u8(name.size())));
        for (auto const c : name)
            _out.push_back(byte(c));
        _open.back().properties_start = _out.size();
    }

    void end()
    {
        flush_properties();
        if (_open.back().has_children)
            for (auto i = 0; i < 13; ++i)
                _out.push_back(byte(0));
        patch_u32(_open.back().record_start, u32(_out.size()));
        _open.remove_back();
    }

    void property(i32 v)
    {
        _properties.push_back(byte('I'));
        put_u32(_properties, u32(v));
        ++_property_count;
    }

    void property(f64 v)
    {
        auto bits = u64(0);
        cc::memcpy(&bits, &v, sizeof(bits));
        _properties.push_back(byte('D'));
        put_u64(_properties, bits);
        ++_property_count;
    }

    void property(i64 v)
    {
        _properties.push_back(byte('L'));
        put_u64(_properties, u64(v));
        ++_property_count;
    }

    void property(cc::string_view s)
    {
        _properties.push_back(byte('S'));
        put_u32(_properties, u32(s.size()));
        for (auto const c : s)
            _properties.push_back(byte(c));
        ++_property_count;
    }

    void array(cc::span<f64 const> values, i64 array_count = -1)
    {
        auto raw = cc::vector<byte>();
        for (auto const v : values)
        {
            auto bits = u64(0);
            cc::memcpy(&bits, &v, sizeof(bits));
            put_u64(raw, bits);
        }
        put_array('d', raw, array_count >= 0 ? array_count : values.size());
    }

    void array(cc::span<i32 const> values, i64 array_count = -1)
    {
        auto raw = cc::vector<byte>();
        for (auto const v : values)
            put_u32(raw, u32(v));
        put_array('i', raw, array_count >= 0 ? array_count : values.size());
    }

    /// The finished file: the top-level list's null record, and nothing after it — ufbx needs no footer.
    [[nodiscard]] cc::vector<byte> finish()
    {
        for (auto i = 0; i < 13; ++i)
            _out.push_back(byte(0));
        return cc::move(_out);
    }

private:
    struct open_record
    {
        isize record_start = 0;
        isize properties_start = 0;
        bool has_children = false;
    };

    static void put_u32(cc::vector<byte>& to, u32 v)
    {
        for (auto k = 0; k < 4; ++k)
            to.push_back(byte(u8((v >> (8 * k)) & 0xffu)));
    }

    static void put_u64(cc::vector<byte>& to, u64 v)
    {
        for (auto k = 0; k < 8; ++k)
            to.push_back(byte(u8((v >> (8 * k)) & 0xffu)));
    }

    void patch_u32(isize at, u32 v)
    {
        for (auto k = 0; k < 4; ++k)
            _out[at + k] = byte(u8((v >> (8 * k)) & 0xffu));
    }

    void put_array(char type, cc::span<byte const> raw, i64 count)
    {
        auto const packed = cc::compress(
            raw, {.algorithm = cc::compression_algorithm::deflate, .framing = cc::compression_framing::zlib});
        _properties.push_back(byte(type));
        put_u32(_properties, u32(count));
        put_u32(_properties, 1); // zlib-compressed
        put_u32(_properties, u32(packed.size()));
        for (auto const b : packed)
            _properties.push_back(b);
        ++_property_count;
    }

    /// Writes the properties gathered for the record opened last, once, before its first child or its end.
    void flush_properties()
    {
        if (_open.empty() || _open.back().properties_start < 0)
            return;
        patch_u32(_open.back().record_start + 4, _property_count);
        patch_u32(_open.back().record_start + 8, u32(_properties.size()));
        for (auto const b : _properties)
            _out.push_back(b);
        _properties.clear();
        _property_count = 0;
        _open.back().properties_start = -1;
    }

    cc::vector<byte> _out;
    cc::vector<open_record> _open;
    cc::vector<byte> _properties;
    u32 _property_count = 0;
};

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

/// The shared scene with `needle`'s first occurrence replaced, for a test that varies one detail of it.
[[nodiscard]] inline cc::string fbx_scene_with(cc::string_view needle, cc::string_view replacement, fbx_settings s = {})
{
    auto text = fbx_scene(s);
    [[maybe_unused]] auto const found = text.replace_first(needle, replacement);
    return text;
}

/// A BINARY FBX 7.4 scene: one triangle (0,0,0), (1,0,0), (0,1,0) in mesh "tri", placed by node "tri" at (0,0,5).
/// Y up, centimeters; both arrays zlib-compressed.
/// `stated_vertices` overrides the element count the Vertices array states, which is how a test claims far more than it carries.
[[nodiscard]] inline cc::vector<byte> fbx_binary_scene(i64 stated_vertices = -1)
{
    // Binary FBX names an object "name\0\1Class" where ASCII writes "Class::name".
    auto const object_name = [](cc::string_view name, cc::string_view type)
    {
        auto out = cc::string(name);
        out += cc::string_view("\0\x01", 2);
        out += type;
        return out;
    };

    auto w = fbx_binary_writer();
    auto const property = [&](cc::string_view name, cc::string_view type, i32 value)
    {
        w.begin("P");
        w.property(name);
        w.property(type);
        w.property(cc::string_view("Integer"));
        w.property(cc::string_view(""));
        w.property(value);
        w.end();
    };

    w.begin("GlobalSettings");
    w.property(i32(1000));
    w.begin("Properties70");
    property("UpAxis", "int", 1);
    property("UpAxisSign", "int", 1);
    property("FrontAxis", "int", 2);
    property("FrontAxisSign", "int", 1);
    property("CoordAxis", "int", 0);
    property("CoordAxisSign", "int", 1);
    w.end();
    w.end();

    w.begin("Objects");
    w.begin("Geometry");
    w.property(i64(100));
    w.property(cc::string_view(object_name("tri", "Geometry")));
    w.property(cc::string_view("Mesh"));
    auto const vertices = cc::vector<f64>{0, 0, 0, 1, 0, 0, 0, 1, 0};
    w.begin("Vertices");
    w.array(vertices, stated_vertices);
    w.end();
    auto const corners = cc::vector<i32>{0, 1, -3};
    w.begin("PolygonVertexIndex");
    w.array(corners);
    w.end();
    w.end();

    w.begin("Model");
    w.property(i64(200));
    w.property(cc::string_view(object_name("tri", "Model")));
    w.property(cc::string_view("Mesh"));
    w.begin("Properties70");
    w.begin("P");
    w.property(cc::string_view("Lcl Translation"));
    w.property(cc::string_view("Lcl Translation"));
    w.property(cc::string_view(""));
    w.property(cc::string_view("A"));
    w.property(f64(0));
    w.property(f64(0));
    w.property(f64(5));
    w.end();
    w.end();
    w.end();
    w.end();

    w.begin("Connections");
    auto const connect = [&](i64 child, i64 parent)
    {
        w.begin("C");
        w.property(cc::string_view("OO"));
        w.property(child);
        w.property(parent);
        w.end();
    };
    connect(100, 200);
    connect(200, 0);
    w.end();
    return w.finish();
}
} // namespace babel::test
