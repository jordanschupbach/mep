// STL, OBJ and glTF: Part B.5's mesh, spelled three ways (Part F.4).

#include "cad_exchange.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace cad {
namespace {

bool BuildMesh(const Model &model, const std::vector<EntityId> &bodies,
               const MeshExportOptions &options, TessellationMesh *out, std::string *error) {
    error->clear();
    if (bodies.empty()) {
        *error = "there are no bodies to export";
        return false;
    }
    for (EntityId body : bodies) {
        TessellationMesh one;
        if (!TessellateBody(model, body, options.tessellation, &one, error)) return false;
        const int base = out->VertexCount();
        for (const Vec3d &p : one.positions) out->positions.push_back(p);
        for (const Vec3d &n : one.normals) out->normals.push_back(n);
        for (int index : one.indices) out->indices.push_back(base + index);
        for (EntityId face : one.triangle_face) out->triangle_face.push_back(face);
    }
    return true;
}

// The face normal, which is what STL stores -- and it stores the
// *triangle's*, not the surface's, because a viewer that trusted a
// smoothed normal on a flat facet would shade a cube like a ball.
Vec3d FacetNormal(const TessellationMesh &mesh, std::size_t triangle) {
    const Vec3d &a = mesh.positions[static_cast<std::size_t>(mesh.indices[triangle * 3])];
    const Vec3d &b = mesh.positions[static_cast<std::size_t>(mesh.indices[triangle * 3 + 1])];
    const Vec3d &c = mesh.positions[static_cast<std::size_t>(mesh.indices[triangle * 3 + 2])];
    const Vec3d n = (b - a).Cross(c - a);
    return n.LengthSquared() > 0.0 ? n.Normalized() : Vec3d{0.0, 0.0, 1.0};
}

void AppendLittleEndian(std::string *out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

void AppendFloat(std::string *out, double value) {
    const float f = static_cast<float>(value);
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    AppendLittleEndian(out, bits);
}

const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64(const std::string &data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < data.size()) {
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[i])) << 16) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[i + 1])) << 8) |
            static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[i + 2]));
        out.push_back(kBase64[(triple >> 18) & 0x3F]);
        out.push_back(kBase64[(triple >> 12) & 0x3F]);
        out.push_back(kBase64[(triple >> 6) & 0x3F]);
        out.push_back(kBase64[triple & 0x3F]);
        i += 3;
    }
    if (i + 1 == data.size()) {
        const std::uint32_t triple = static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[i]))
                                     << 16;
        out.push_back(kBase64[(triple >> 18) & 0x3F]);
        out.push_back(kBase64[(triple >> 12) & 0x3F]);
        out += "==";
    } else if (i + 2 == data.size()) {
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[i])) << 16) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[i + 1])) << 8);
        out.push_back(kBase64[(triple >> 18) & 0x3F]);
        out.push_back(kBase64[(triple >> 12) & 0x3F]);
        out.push_back(kBase64[(triple >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string Number(double v) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.9g", v);
    return buffer;
}

}  // namespace

bool WriteStl(const Model &model, const std::vector<EntityId> &bodies, std::string *out,
              std::string *error, const MeshExportOptions &options) {
    out->clear();
    TessellationMesh mesh;
    if (!BuildMesh(model, bodies, options, &mesh, error)) return false;
    const std::size_t triangles = static_cast<std::size_t>(mesh.TriangleCount());

    if (options.ascii_stl) {
        *out = "solid " + options.name + "\n";
        for (std::size_t t = 0; t < triangles; ++t) {
            const Vec3d n = FacetNormal(mesh, t);
            *out += "facet normal " + Number(n.x) + " " + Number(n.y) + " " + Number(n.z) + "\n";
            *out += "  outer loop\n";
            for (int k = 0; k < 3; ++k) {
                const Vec3d &p = mesh.positions[static_cast<std::size_t>(mesh.indices[t * 3 + static_cast<std::size_t>(k)])];
                *out += "    vertex " + Number(p.x) + " " + Number(p.y) + " " + Number(p.z) + "\n";
            }
            *out += "  endloop\nendfacet\n";
        }
        *out += "endsolid " + options.name + "\n";
        return true;
    }

    // Binary: an eighty-byte header that must not begin with "solid",
    // since that is how every reader tells the two apart.
    std::string header = "mep binary STL: " + options.name;
    header.resize(80, ' ');
    *out = header;
    AppendLittleEndian(out, static_cast<std::uint32_t>(triangles));
    for (std::size_t t = 0; t < triangles; ++t) {
        const Vec3d n = FacetNormal(mesh, t);
        AppendFloat(out, n.x);
        AppendFloat(out, n.y);
        AppendFloat(out, n.z);
        for (int k = 0; k < 3; ++k) {
            const Vec3d &p = mesh.positions[static_cast<std::size_t>(mesh.indices[t * 3 + static_cast<std::size_t>(k)])];
            AppendFloat(out, p.x);
            AppendFloat(out, p.y);
            AppendFloat(out, p.z);
        }
        out->push_back('\0');
        out->push_back('\0');
    }
    return true;
}

bool ReadStl(const std::string &text, TessellationMesh *out, std::string *error) {
    error->clear();
    *out = TessellationMesh{};
    const bool ascii = text.size() > 5 && text.compare(0, 5, "solid") == 0 &&
                       text.find("facet") != std::string::npos;
    if (ascii) {
        std::size_t pos = 0;
        while ((pos = text.find("vertex", pos)) != std::string::npos) {
            pos += 6;
            Vec3d p;
            if (std::sscanf(text.c_str() + pos, "%lf %lf %lf", &p.x, &p.y, &p.z) != 3) {
                *error = "a vertex line is malformed";
                return false;
            }
            out->indices.push_back(out->VertexCount());
            out->positions.push_back(p);
            out->normals.push_back(Vec3d{0.0, 0.0, 1.0});
        }
        if (out->positions.size() % 3 != 0) {
            *error = "the file has a triangle with the wrong number of vertices";
            return false;
        }
        out->triangle_face.assign(out->positions.size() / 3, kNoEntity);
        return true;
    }
    if (text.size() < 84) {
        *error = "the file is too short to be a binary STL";
        return false;
    }
    auto read_u32 = [&text](std::size_t at) {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(text[at + static_cast<std::size_t>(i)]))
                     << (8 * i);
        }
        return value;
    };
    auto read_float = [&](std::size_t at) {
        const std::uint32_t bits = read_u32(at);
        float f = 0.0f;
        std::memcpy(&f, &bits, sizeof(f));
        return static_cast<double>(f);
    };
    const std::uint32_t count = read_u32(80);
    if (text.size() < 84 + static_cast<std::size_t>(count) * 50) {
        *error = "the file says it has " + std::to_string(count) +
                 " triangles and is not long enough to hold them";
        return false;
    }
    for (std::uint32_t t = 0; t < count; ++t) {
        const std::size_t at = 84 + static_cast<std::size_t>(t) * 50;
        for (int k = 0; k < 3; ++k) {
            const std::size_t v = at + 12 + static_cast<std::size_t>(k) * 12;
            out->indices.push_back(out->VertexCount());
            out->positions.push_back(Vec3d{read_float(v), read_float(v + 4), read_float(v + 8)});
            out->normals.push_back(Vec3d{read_float(at), read_float(at + 4), read_float(at + 8)});
        }
        out->triangle_face.push_back(kNoEntity);
    }
    return true;
}

bool WriteObj(const Model &model, const std::vector<EntityId> &bodies, std::string *out,
              std::string *error, const MeshExportOptions &options) {
    out->clear();
    TessellationMesh mesh;
    if (!BuildMesh(model, bodies, options, &mesh, error)) return false;
    *out = "# written by mep\no " + options.name + "\n";
    for (const Vec3d &p : mesh.positions) {
        *out += "v " + Number(p.x) + " " + Number(p.y) + " " + Number(p.z) + "\n";
    }
    for (const Vec3d &n : mesh.normals) {
        *out += "vn " + Number(n.x) + " " + Number(n.y) + " " + Number(n.z) + "\n";
    }
    // OBJ indexes from one, which is the single most common thing to get
    // wrong about it.
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        *out += "f";
        for (int k = 0; k < 3; ++k) {
            const std::string index = std::to_string(mesh.indices[t + static_cast<std::size_t>(k)] + 1);
            *out += " " + index + "//" + index;
        }
        *out += "\n";
    }
    return true;
}

bool WriteGltf(const Model &model, const std::vector<EntityId> &bodies, std::string *out,
               std::string *error, const MeshExportOptions &options) {
    out->clear();
    TessellationMesh mesh;
    if (!BuildMesh(model, bodies, options, &mesh, error)) return false;
    if (mesh.positions.size() > 0xFFFFFFFFu) {
        *error = "the mesh has more vertices than glTF can index";
        return false;
    }

    // One buffer: positions, then normals, then indices. glTF requires
    // each accessor's offset to be a multiple of its component size, and
    // all three here are four bytes, so packing them in order is enough.
    std::string buffer;
    Box3d extent;
    for (const Vec3d &p : mesh.positions) {
        AppendFloat(&buffer, p.x);
        AppendFloat(&buffer, p.y);
        AppendFloat(&buffer, p.z);
        extent.Expand(p);
    }
    const std::size_t normals_at = buffer.size();
    for (const Vec3d &n : mesh.normals) {
        AppendFloat(&buffer, n.x);
        AppendFloat(&buffer, n.y);
        AppendFloat(&buffer, n.z);
    }
    const std::size_t indices_at = buffer.size();
    for (int index : mesh.indices) AppendLittleEndian(&buffer, static_cast<std::uint32_t>(index));

    const std::size_t vertices = mesh.positions.size();
    const std::string lo = extent.IsEmpty() ? "[0,0,0]"
                                            : "[" + Number(extent.x.lo) + "," + Number(extent.y.lo) +
                                                  "," + Number(extent.z.lo) + "]";
    const std::string hi = extent.IsEmpty() ? "[0,0,0]"
                                            : "[" + Number(extent.x.hi) + "," + Number(extent.y.hi) +
                                                  "," + Number(extent.z.hi) + "]";
    *out =
        "{\"asset\":{\"version\":\"2.0\",\"generator\":\"mep\"},"
        "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0,\"name\":\"" + options.name + "\"}],"
        "\"meshes\":[{\"name\":\"" + options.name +
        "\",\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},\"indices\":2,\"mode\":4}]}],"
        "\"buffers\":[{\"byteLength\":" + std::to_string(buffer.size()) +
        ",\"uri\":\"data:application/octet-stream;base64," + Base64(buffer) + "\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" + std::to_string(normals_at) +
        ",\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(normals_at) +
        ",\"byteLength\":" + std::to_string(indices_at - normals_at) + ",\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(indices_at) +
        ",\"byteLength\":" + std::to_string(buffer.size() - indices_at) + ",\"target\":34963}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":" + std::to_string(vertices) +
        ",\"type\":\"VEC3\",\"min\":" + lo + ",\"max\":" + hi + "},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":" + std::to_string(vertices) +
        ",\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5125,\"count\":" + std::to_string(mesh.indices.size()) +
        ",\"type\":\"SCALAR\"}]}";
    return true;
}

}  // namespace cad
