// Mesh/material/transform extraction from .blend files. See blend_scene.h.

#include "blend_scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <unordered_map>

#include "blend_file.h"

namespace blend {

namespace {

// Column-major like Blender's float[4][4]: m[col][row].
struct Mat4 {
    double m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
};

Mat4 Mul(const Mat4 &a, const Mat4 &b) {
    Mat4 r;
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            double s = 0.0;
            for (int k = 0; k < 4; k++) s += a.m[k][row] * b.m[c][k];
            r.m[c][row] = s;
        }
    }
    return r;
}

using Mat3 = std::array<std::array<double, 3>, 3>;  // [col][row]

Mat3 Mul3(const Mat3 &a, const Mat3 &b) {
    Mat3 r{};
    for (int c = 0; c < 3; c++) {
        for (int row = 0; row < 3; row++) {
            double s = 0.0;
            for (int k = 0; k < 3; k++) s += a[static_cast<size_t>(k)][static_cast<size_t>(row)] * b[static_cast<size_t>(c)][static_cast<size_t>(k)];
            r[static_cast<size_t>(c)][static_cast<size_t>(row)] = s;
        }
    }
    return r;
}

Mat3 Identity3() { return Mat3{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}; }

Mat3 AxisRotation(int axis, double angle) {
    double c = std::cos(angle), s = std::sin(angle);
    Mat3 r = Identity3();
    // Standard right-handed rotations, stored column-major.
    if (axis == 0) {
        r[1][1] = c;
        r[1][2] = s;
        r[2][1] = -s;
        r[2][2] = c;
    } else if (axis == 1) {
        r[0][0] = c;
        r[0][2] = -s;
        r[2][0] = s;
        r[2][2] = c;
    } else {
        r[0][0] = c;
        r[0][1] = s;
        r[1][0] = -s;
        r[1][1] = c;
    }
    return r;
}

// Blender rotation modes 1..6 are Euler orders XYZ, XZY, YXZ, YZX, ZXY,
// ZYX; the first axis named is applied first.
Mat3 EulerToMat3(const double e[3], int mode) {
    static const int kOrders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    const int *o = kOrders[(mode >= 1 && mode <= 6) ? mode - 1 : 0];
    Mat3 r = AxisRotation(o[0], e[o[0]]);
    r = Mul3(AxisRotation(o[1], e[o[1]]), r);
    r = Mul3(AxisRotation(o[2], e[o[2]]), r);
    return r;
}

Mat3 QuatToMat3(double w, double x, double y, double z) {
    double n = std::sqrt(w * w + x * x + y * y + z * z);
    if (n < 1e-12) return Identity3();
    w /= n;
    x /= n;
    y /= n;
    z /= n;
    Mat3 r{};
    r[0][0] = 1 - 2 * (y * y + z * z);
    r[0][1] = 2 * (x * y + w * z);
    r[0][2] = 2 * (x * z - w * y);
    r[1][0] = 2 * (x * y - w * z);
    r[1][1] = 1 - 2 * (x * x + z * z);
    r[1][2] = 2 * (y * z + w * x);
    r[2][0] = 2 * (x * z + w * y);
    r[2][1] = 2 * (y * z - w * x);
    r[2][2] = 1 - 2 * (x * x + y * y);
    return r;
}

Mat3 AxisAngleToMat3(const double axis[3], double angle) {
    double n = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (n < 1e-12) return Identity3();
    double h = angle * 0.5;
    double s = std::sin(h) / n;
    return QuatToMat3(std::cos(h), axis[0] * s, axis[1] * s, axis[2] * s);
}

double Det3(const Mat4 &m) {
    return m.m[0][0] * (m.m[1][1] * m.m[2][2] - m.m[2][1] * m.m[1][2]) -
           m.m[1][0] * (m.m[0][1] * m.m[2][2] - m.m[2][1] * m.m[0][2]) +
           m.m[2][0] * (m.m[0][1] * m.m[1][2] - m.m[1][1] * m.m[0][2]);
}

// Inverse-transpose of the upper 3x3 (cofactor matrix; scale does not
// matter since normals are renormalised).
Mat3 NormalMatrix(const Mat4 &m) {
    auto a = [&](int c, int r) { return m.m[c][r]; };
    Mat3 n{};
    // cofactor(r, c) of the row-major matrix A[r][c] = m[c][r]; the
    // inverse-transpose equals cofactor / det, so map cofactors straight
    // into column-major storage: n[c][r] = cof(r, c).
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (c + 1) % 3, c2 = (c + 2) % 3;
            n[static_cast<size_t>(c)][static_cast<size_t>(r)] = a(c1, r1) * a(c2, r2) - a(c2, r1) * a(c1, r2);
        }
    }
    return n;
}

bool StartsWith(const std::string &s, const char *prefix) { return s.rfind(prefix, 0) == 0; }

// An ID's display name: ID.name minus its two-letter type code.
std::string IdName(const View &v) {
    std::string n = v.Sub("id").Str("name");
    return n.size() > 2 ? n.substr(2) : n;
}

// Attribute kinds, independent of which storage system the file uses.
enum class Kind { Unknown, Bool, Int8, Int32, Int32x2, Float, Float2, Float3 };

size_t KindSize(Kind k) {
    switch (k) {
        case Kind::Bool:
        case Kind::Int8:
            return 1;
        case Kind::Int32:
        case Kind::Float:
            return 4;
        case Kind::Int32x2:
        case Kind::Float2:
            return 8;
        case Kind::Float3:
            return 12;
        case Kind::Unknown:
            break;
    }
    return 0;
}

enum Domain { kPoint = 0, kEdge = 1, kFace = 2, kCorner = 3, kTessFace = 100 };

struct Attr {
    std::string name;
    int domain = -1;
    Kind kind = Kind::Unknown;
    int cd_type = -1;                  // CustomData layer type, -1 for AttributeStorage
    const Struct *layout = nullptr;    // legacy struct layers (MVert, MPoly, ...)
    size_t offset = 0;                 // absolute file offset of element 0
    size_t count = 0;                  // elements available
    size_t stride = 0;
    bool single = false;               // one value for every element
    int active_rnd = 0;
};

struct Geometry {
    bool ok = false;
    std::vector<float> pos;               // 3 per vertex
    std::vector<uint32_t> face_start;     // faces + 1
    std::vector<uint32_t> corner_vert;    // per corner
    std::vector<float> corner_uv;         // 2 per corner, or empty
    std::vector<int> face_mat;            // per face
    std::vector<uint8_t> face_smooth;     // per face
    size_t Faces() const { return face_start.empty() ? 0 : face_start.size() - 1; }
};

class Extractor {
public:
    Extractor(const File &f, const std::string &base_dir, Scene *out) : f_(f), base_dir_(base_dir), out_(out) {}

    bool Run(std::string *error) {
        out_->version = f_.version();
        out_->compression = f_.compression();
        if (!f_.FindStruct("Object") || !f_.FindStruct("Mesh")) {
            if (error) *error = ".blend file has no Object/Mesh types in its SDNA";
            return false;
        }
        for (const Block &b : f_.blocks()) {
            if (!b.Is("OB")) continue;
            View ob(&f_, f_.FindStruct("Object"), b.offset, b.group);
            if (b.len < ob.type()->size) continue;
            ExtractObject(ob);
        }
        if (out_->parts.empty()) {
            if (error) {
                *error = "no mesh geometry found in .blend file";
                if (!out_->warnings.empty()) *error += " (" + out_->warnings.front() + ")";
            }
            return false;
        }
        return true;
    }

private:
    const File &f_;
    std::string base_dir_;
    Scene *out_;
    std::map<uint64_t, int> materials_;
    std::map<size_t, Mat4> world_cache_;
    std::map<size_t, Geometry> geometry_cache_;

    void Warn(const std::string &w) {
        if (std::find(out_->warnings.begin(), out_->warnings.end(), w) == out_->warnings.end()) out_->warnings.push_back(w);
    }

    std::string StringAt(uint64_t ptr, size_t group, size_t max = 256) const {
        size_t inner = 0;
        const Block *b = f_.BlockFor(ptr, group, &inner);
        if (!b) return {};
        return f_.ReadString(b->offset + inner, std::min(max, b->len - inner));
    }

    // Byte range a pointer refers to: (absolute offset, bytes available).
    bool Range(uint64_t ptr, size_t group, size_t *offset, size_t *avail) const {
        size_t inner = 0;
        const Block *b = f_.BlockFor(ptr, group, &inner);
        if (!b) return false;
        *offset = b->offset + inner;
        *avail = b->len - inner;
        return true;
    }

    // ---- transforms -------------------------------------------------

    Mat4 Local(const View &ob) const {
        double loc[3], scale[3], rot[3], drot[3];
        for (size_t i = 0; i < 3; i++) {
            loc[i] = ob.Float("loc", 0.0, i) + ob.Float("dloc", 0.0, i);
            double s = ob.Float("size", 1.0, i);
            if (ob.Has("dscale")) {
                s *= ob.Float("dscale", 1.0, i);
            } else if (ob.Has("dsize")) {
                s += ob.Float("dsize", 0.0, i);
            }
            scale[i] = s;
            rot[i] = ob.Float("rot", 0.0, i);
            drot[i] = ob.Float("drot", 0.0, i);
        }
        int mode = ob.Has("rotmode") ? static_cast<int>(ob.Int("rotmode", 1)) : 1;
        Mat3 r, d;
        if (mode > 0) {
            r = EulerToMat3(rot, mode);
            d = EulerToMat3(drot, mode);
        } else if (mode == -1) {
            double axis[3], daxis[3];
            for (size_t i = 0; i < 3; i++) {
                axis[i] = ob.Float("rotAxis", i == 1 ? 1.0 : 0.0, i);
                daxis[i] = ob.Float("drotAxis", i == 1 ? 1.0 : 0.0, i);
            }
            r = AxisAngleToMat3(axis, ob.Float("rotAngle"));
            d = AxisAngleToMat3(daxis, ob.Float("drotAngle"));
        } else {
            r = QuatToMat3(ob.Float("quat", 1.0, 0), ob.Float("quat", 0.0, 1), ob.Float("quat", 0.0, 2), ob.Float("quat", 0.0, 3));
            d = QuatToMat3(ob.Float("dquat", 1.0, 0), ob.Float("dquat", 0.0, 1), ob.Float("dquat", 0.0, 2),
                           ob.Float("dquat", 0.0, 3));
        }
        Mat3 rs = Mul3(d, r);
        Mat4 m;
        for (size_t c = 0; c < 3; c++) {
            for (size_t row = 0; row < 3; row++) m.m[c][row] = rs[c][row] * scale[c];
            m.m[c][3] = 0.0;
        }
        for (size_t row = 0; row < 3; row++) m.m[3][row] = loc[row];
        m.m[3][3] = 1.0;
        return m;
    }

    Mat4 World(const View &ob, int depth) {
        auto it = world_cache_.find(ob.offset());
        if (it != world_cache_.end()) return it->second;
        Mat4 w = Local(ob);
        uint64_t parent = ob.Ptr("parent");
        if (parent != 0 && depth < 64) {
            View pv = ViewAt(f_, parent, ob.group(), "Object");
            if (pv.valid()) {
                int partype = static_cast<int>(ob.Int("partype")) & 0x0F;  // PARTYPE mask
                if (partype != 0) Warn("objects parented to bones/vertices are placed relative to the parent object");
                Mat4 pinv;
                for (size_t c = 0; c < 4; c++) {
                    for (size_t row = 0; row < 4; row++) pinv.m[c][row] = ob.Float("parentinv", c == row ? 1.0 : 0.0, c * 4 + row);
                }
                w = Mul(Mul(World(pv, depth + 1), pinv), w);
            }
        }
        world_cache_[ob.offset()] = w;
        return w;
    }

    // ---- attributes --------------------------------------------------

    std::vector<Attr> Attributes(const View &me) const {
        std::vector<Attr> attrs;
        // Blender 5.x: Mesh.attribute_storage.
        View storage = me.Sub("attribute_storage");
        if (storage.valid()) {
            int64_t n = storage.Int("dna_attributes_num");
            View a0 = ViewAt(f_, storage.Ptr("dna_attributes"), me.group(), "Attribute");
            for (int64_t i = 0; a0.valid() && i < n; i++) {
                View a = a0.At(static_cast<size_t>(i));
                if (!a.valid()) break;
                Attr at;
                at.name = StringAt(a.Ptr("name"), me.group());
                at.domain = static_cast<int>(a.Int("domain"));
                switch (a.Int("data_type", -1)) {
                    case 0: at.kind = Kind::Bool; break;
                    case 1: at.kind = Kind::Int8; break;
                    case 3: at.kind = Kind::Int32; break;
                    case 4: at.kind = Kind::Int32x2; break;
                    case 5: at.kind = Kind::Float; break;
                    case 6: at.kind = Kind::Float2; break;
                    case 7: at.kind = Kind::Float3; break;
                    default: at.kind = Kind::Unknown; break;
                }
                uint64_t data_ptr;
                size_t want;
                if (a.Int("storage_type") == 0) {
                    View arr = ViewAt(f_, a.Ptr("data"), me.group(), "AttributeArray");
                    data_ptr = arr.Ptr("data");
                    want = static_cast<size_t>(std::max<int64_t>(0, arr.Int("size")));
                } else {
                    View single = ViewAt(f_, a.Ptr("data"), me.group(), "AttributeSingle");
                    data_ptr = single.Ptr("data");
                    want = 1;
                    at.single = true;
                }
                at.stride = KindSize(at.kind);
                size_t avail = 0;
                if (at.stride == 0 || !Range(data_ptr, me.group(), &at.offset, &avail)) continue;
                at.count = std::min(want, avail / at.stride);
                attrs.push_back(at);
            }
        }
        // Blender 2.4x-4.x: CustomData layers.
        static const struct {
            const char *field;
            int domain;
        } kData[] = {{"vdata", kPoint}, {"edata", kEdge}, {"pdata", kFace}, {"ldata", kCorner}, {"fdata", kTessFace}};
        for (const auto &cd : kData) {
            View data = me.Sub(cd.field);
            if (!data.valid()) continue;
            int64_t n = data.Int("totlayer");
            View l0 = ViewAt(f_, data.Ptr("layers"), me.group(), "CustomDataLayer");
            for (int64_t i = 0; l0.valid() && i < n; i++) {
                View l = l0.At(static_cast<size_t>(i));
                if (!l.valid()) break;
                Attr at;
                at.name = l.Str("name");
                at.domain = cd.domain;
                at.cd_type = static_cast<int>(l.Int("type", -1));
                at.active_rnd = static_cast<int>(l.Int("active_rnd"));
                switch (at.cd_type) {
                    case 10: at.kind = Kind::Float; break;
                    case 11: at.kind = Kind::Int32; break;
                    case 45: at.kind = Kind::Int8; break;
                    case 46: at.kind = Kind::Int32x2; break;
                    case 48: at.kind = Kind::Float3; break;
                    case 49: at.kind = Kind::Float2; break;
                    case 50: at.kind = Kind::Bool; break;
                    default: at.kind = Kind::Unknown; break;
                }
                uint64_t data_ptr = l.Ptr("data");
                size_t inner = 0;
                const Block *b = f_.BlockFor(data_ptr, me.group(), &inner);
                if (!b) continue;
                at.offset = b->offset + inner;
                size_t avail = b->len - inner;
                if (at.kind != Kind::Unknown) {
                    at.stride = KindSize(at.kind);
                } else {
                    // Legacy struct layers: MVert, MPoly, MLoop, MLoopUV, MFace, MTFace.
                    at.layout = f_.StructOf(*b);
                    if (!at.layout || at.layout->size == 0) continue;
                    at.stride = at.layout->size;
                }
                at.count = avail / at.stride;
                attrs.push_back(at);
            }
        }
        return attrs;
    }

    static const Attr *FindAttr(const std::vector<Attr> &attrs, const std::string &name, int domain, Kind kind) {
        for (const Attr &a : attrs) {
            if (a.name == name && a.domain == domain && a.kind == kind) return &a;
        }
        return nullptr;
    }

    static const Attr *FindLayer(const std::vector<Attr> &attrs, int domain, int cd_type) {
        for (const Attr &a : attrs) {
            if (a.domain == domain && a.cd_type == cd_type) return &a;
        }
        return nullptr;
    }

    size_t Elem(const Attr &a, size_t i) const { return a.offset + (a.single ? 0 : i) * a.stride; }

    // Struct-array view for a legacy pointer field (Mesh.mvert etc.) or,
    // failing that, a CustomData layer of type `cd_type`.
    View LegacyArray(const View &me, const char *field, const char *type, const std::vector<Attr> &attrs, int domain,
                     int cd_type, size_t need, size_t *count) const {
        size_t inner = 0;
        uint64_t ptr = me.Ptr(field);
        const Struct *st = f_.FindStruct(type);
        if (!st || st->size == 0) return {};
        if (const Block *b = ptr ? f_.BlockFor(ptr, me.group(), &inner) : nullptr) {
            size_t n = (b->len - inner) / st->size;
            if (n >= need) {
                *count = n;
                return View(&f_, st, b->offset + inner, me.group());
            }
        }
        if (const Attr *a = FindLayer(attrs, domain, cd_type)) {
            if (a->layout == st && a->count >= need) {
                *count = a->count;
                return View(&f_, st, a->offset, me.group());
            }
        }
        return {};
    }

    // ---- geometry -----------------------------------------------------

    Geometry ReadGeometry(const View &me, const std::string &obname) {
        Geometry g;
        std::vector<Attr> attrs = Attributes(me);
        size_t nv = static_cast<size_t>(std::max<int64_t>(0, me.Int("totvert")));
        size_t npoly = static_cast<size_t>(std::max<int64_t>(0, me.Int("totpoly")));
        size_t nloop = static_cast<size_t>(std::max<int64_t>(0, me.Int("totloop")));
        size_t ntess = static_cast<size_t>(std::max<int64_t>(0, me.Int("totface")));
        if (nv == 0) return g;

        // Positions.
        if (const Attr *p = FindAttr(attrs, "position", kPoint, Kind::Float3); p && p->count >= nv) {
            g.pos.resize(nv * 3);
            for (size_t i = 0; i < nv; i++) {
                for (size_t k = 0; k < 3; k++) g.pos[i * 3 + k] = f_.ReadFloat(Elem(*p, i) + 4 * k);
            }
        } else {
            size_t n = 0;
            View mv = LegacyArray(me, "mvert", "MVert", attrs, kPoint, 0, nv, &n);
            if (!mv.valid()) {
                Warn("object " + obname + ": mesh has no readable vertex positions");
                return g;
            }
            g.pos.resize(nv * 3);
            for (size_t i = 0; i < nv; i++) {
                View v = mv.At(i);
                for (size_t k = 0; k < 3; k++) g.pos[i * 3 + k] = static_cast<float>(v.Float("co", 0.0, k));
            }
        }

        // Faces.
        uint64_t offsets_ptr = me.Ptr("poly_offset_indices");
        if (!offsets_ptr) offsets_ptr = me.Ptr("face_offset_indices");
        const Attr *corner_vert = FindAttr(attrs, ".corner_vert", kCorner, Kind::Int32);
        bool have_faces = false;
        if (npoly > 0 && offsets_ptr && corner_vert && corner_vert->count >= nloop) {
            size_t off = 0, avail = 0;
            if (Range(offsets_ptr, me.group(), &off, &avail) && avail >= (npoly + 1) * 4) {
                g.face_start.resize(npoly + 1);
                for (size_t i = 0; i <= npoly; i++) g.face_start[i] = static_cast<uint32_t>(f_.ReadSigned(off + 4 * i, 4));
                g.corner_vert.resize(nloop);
                for (size_t i = 0; i < nloop; i++) g.corner_vert[i] = static_cast<uint32_t>(f_.ReadSigned(Elem(*corner_vert, i), 4));
                g.face_mat.assign(npoly, 0);
                g.face_smooth.assign(npoly, 1);
                have_faces = true;
            }
        }
        if (!have_faces && npoly > 0) {
            size_t np = 0, nl = 0;
            View mp = LegacyArray(me, "mpoly", "MPoly", attrs, kFace, 25, npoly, &np);
            View ml = LegacyArray(me, "mloop", "MLoop", attrs, kCorner, 26, nloop, &nl);
            if (mp.valid() && ml.valid()) {
                g.face_start.resize(npoly + 1);
                g.face_mat.resize(npoly);
                g.face_smooth.resize(npoly);
                // MPoly stores (loopstart, totloop); faces are normally
                // contiguous but keep corners in face order regardless.
                g.corner_vert.clear();
                for (size_t i = 0; i < npoly; i++) {
                    View p = mp.At(i);
                    int64_t start = p.Int("loopstart");
                    int64_t count = p.Int("totloop");
                    g.face_start[i] = static_cast<uint32_t>(g.corner_vert.size());
                    for (int64_t k = 0; k < count; k++) {
                        int64_t li = start + k;
                        if (li < 0 || static_cast<size_t>(li) >= nloop) break;
                        g.corner_vert.push_back(static_cast<uint32_t>(ml.At(static_cast<size_t>(li)).Int("v")));
                    }
                    g.face_mat[i] = static_cast<int>(p.Int("mat_nr"));
                    g.face_smooth[i] = static_cast<uint8_t>(p.Int("flag") & 1);
                }
                g.face_start[npoly] = static_cast<uint32_t>(g.corner_vert.size());
                have_faces = true;
                // UVs: the active MLoopUV layer (Mesh.mloopuv points at it
                // in 2.6x-2.7x), remapped to our corner order.
                size_t nuv = 0;
                View uv = LegacyArray(me, "mloopuv", "MLoopUV", attrs, kCorner, 16, nloop, &nuv);
                if (!uv.valid()) {
                    // Pick the active-render MLoopUV layer.
                    std::vector<const Attr *> layers;
                    for (const Attr &a : attrs) {
                        if (a.domain == kCorner && a.cd_type == 16 && a.count >= nloop) layers.push_back(&a);
                    }
                    if (!layers.empty()) {
                        size_t pick = static_cast<size_t>(std::clamp(layers[0]->active_rnd, 0, static_cast<int>(layers.size()) - 1));
                        uv = View(&f_, layers[pick]->layout, layers[pick]->offset, me.group());
                    }
                }
                if (uv.valid() && uv.Has("uv")) {
                    g.corner_uv.reserve(g.corner_vert.size() * 2);
                    for (size_t i = 0; i < npoly; i++) {
                        View p = mp.At(i);
                        int64_t start = p.Int("loopstart");
                        size_t count = g.face_start[i + 1] - g.face_start[i];
                        for (size_t k = 0; k < count; k++) {
                            View e = uv.At(static_cast<size_t>(start) + k);
                            g.corner_uv.push_back(static_cast<float>(e.Float("uv", 0.0, 0)));
                            g.corner_uv.push_back(static_cast<float>(e.Float("uv", 0.0, 1)));
                        }
                    }
                }
            }
        }
        if (!have_faces && ntess > 0) {
            size_t nf = 0;
            View mf = LegacyArray(me, "mface", "MFace", attrs, kTessFace, 4, ntess, &nf);
            if (mf.valid()) {
                size_t nt = 0;
                View tf = LegacyArray(me, "mtface", "MTFace", attrs, kTessFace, 5, ntess, &nt);
                g.face_start.push_back(0);
                for (size_t i = 0; i < ntess; i++) {
                    View fc = mf.At(i);
                    uint32_t v[4] = {static_cast<uint32_t>(fc.Int("v1")), static_cast<uint32_t>(fc.Int("v2")),
                                     static_cast<uint32_t>(fc.Int("v3")), static_cast<uint32_t>(fc.Int("v4"))};
                    size_t n = v[3] != 0 ? 4 : 3;
                    for (size_t k = 0; k < n; k++) {
                        g.corner_vert.push_back(v[k]);
                        if (tf.valid()) {
                            View t = tf.At(i);
                            g.corner_uv.push_back(static_cast<float>(t.Float("uv", 0.0, k * 2)));
                            g.corner_uv.push_back(static_cast<float>(t.Float("uv", 0.0, k * 2 + 1)));
                        }
                    }
                    g.face_start.push_back(static_cast<uint32_t>(g.corner_vert.size()));
                    g.face_mat.push_back(static_cast<int>(fc.Int("mat_nr")));
                    g.face_smooth.push_back(static_cast<uint8_t>(fc.Int("flag") & 1));
                }
                have_faces = true;
            }
        }
        if (!have_faces) {
            if (npoly > 0 || ntess > 0) Warn("object " + obname + ": mesh faces could not be read");
            return g;
        }
        size_t corners = g.corner_vert.size();

        // Modern per-face/per-corner attributes (override legacy fields).
        if (const Attr *mi = FindAttr(attrs, "material_index", kFace, Kind::Int32); mi && (mi->single || mi->count >= g.Faces())) {
            for (size_t i = 0; i < g.Faces(); i++) g.face_mat[i] = static_cast<int>(f_.ReadSigned(Elem(*mi, i), 4));
        }
        if (const Attr *sh = FindAttr(attrs, "sharp_face", kFace, Kind::Bool); sh && (sh->single || sh->count >= g.Faces())) {
            for (size_t i = 0; i < g.Faces(); i++) g.face_smooth[i] = f_.ReadUnsigned(Elem(*sh, i), 1) ? 0 : 1;
        }
        if (g.corner_uv.empty()) {
            std::vector<const Attr *> uvs;
            for (const Attr &a : attrs) {
                if (a.domain == kCorner && a.kind == Kind::Float2 && !StartsWith(a.name, ".") && a.count >= corners) uvs.push_back(&a);
            }
            if (!uvs.empty()) {
                const Attr *pick = uvs[0];
                std::string want = StringAt(me.Ptr("default_uv_map_attribute"), me.group());
                if (want.empty()) want = StringAt(me.Ptr("active_uv_map_attribute"), me.group());
                bool found = false;
                for (const Attr *a : uvs) {
                    if (!want.empty() && a->name == want) {
                        pick = a;
                        found = true;
                    }
                }
                if (!found && uvs[0]->cd_type >= 0) {
                    pick = uvs[static_cast<size_t>(std::clamp(uvs[0]->active_rnd, 0, static_cast<int>(uvs.size()) - 1))];
                }
                g.corner_uv.resize(corners * 2);
                for (size_t i = 0; i < corners; i++) {
                    g.corner_uv[i * 2] = f_.ReadFloat(Elem(*pick, i));
                    g.corner_uv[i * 2 + 1] = f_.ReadFloat(Elem(*pick, i) + 4);
                }
            }
        }
        if (!g.corner_uv.empty() && g.corner_uv.size() != corners * 2) g.corner_uv.clear();

        // Validate indices.
        for (uint32_t &v : g.corner_vert) {
            if (v >= nv) {
                Warn("object " + obname + ": mesh has out-of-range vertex indices");
                return Geometry{};
            }
        }
        for (size_t i = 0; i < g.Faces(); i++) {
            if (g.face_start[i] > g.face_start[i + 1] || g.face_start[i + 1] > corners) {
                Warn("object " + obname + ": mesh has inconsistent face offsets");
                return Geometry{};
            }
        }
        g.ok = true;
        return g;
    }

    // ---- materials ------------------------------------------------------

    void ReadImage(uint64_t ptr, size_t group, SceneMaterial *m) const {
        View im = ViewAt(f_, ptr, group, "Image");
        if (!im.valid()) return;
        m->image_name = IdName(im);
        std::string path = im.Has("filepath") ? im.Str("filepath") : im.Str("name");
        std::replace(path.begin(), path.end(), '\\', '/');
        if (StartsWith(path, "//")) path = (base_dir_.empty() ? std::string(".") : base_dir_) + "/" + path.substr(2);
        m->image_path = path;
        uint64_t pf = im.Ptr("packedfile");
        if (!pf) {
            View ipf = ViewAt(f_, im.Sub("packedfiles").Ptr("first"), im.group(), "ImagePackedFile");
            pf = ipf.Ptr("packedfile");
        }
        View packed = ViewAt(f_, pf, im.group(), "PackedFile");
        if (packed.valid()) {
            size_t off = 0, avail = 0;
            int64_t size = packed.Int("size");
            if (size > 0 && Range(packed.Ptr("data"), packed.group(), &off, &avail) && static_cast<size_t>(size) <= avail) {
                m->image_packed.assign(f_.data() + off, f_.data() + off + static_cast<size_t>(size));
            }
        }
    }

    static std::vector<View> ListItems(const View &list, const char *type) {
        std::vector<View> out;
        if (!list.valid()) return out;
        const File &f = *list.file();
        uint64_t p = list.Ptr("first");
        size_t group = list.group();
        for (int guard = 0; p != 0 && guard < 100000; guard++) {
            View v = ViewAt(f, p, group, type);
            if (!v.valid()) break;
            out.push_back(v);
            p = v.Ptr("next");
        }
        return out;
    }

    // Principled/Diffuse BSDF defaults and a directly linked image.
    bool ReadNodeMaterial(const View &ma, SceneMaterial *m) const {
        View tree = ma.Deref("nodetree");
        if (!tree.valid()) return false;
        std::vector<View> nodes = ListItems(tree.Sub("nodes"), "bNode");
        std::vector<View> links = ListItems(tree.Sub("links"), "bNodeLink");
        auto socket = [&](const View &node, const char *ident) -> std::pair<View, uint64_t> {
            uint64_t p = node.Sub("inputs").Ptr("first");
            for (int guard = 0; p != 0 && guard < 1000; guard++) {
                View s = ViewAt(f_, p, node.group(), "bNodeSocket");
                if (!s.valid()) break;
                if (s.Str("identifier") == ident || (s.Str("identifier").empty() && s.Str("name") == ident)) return {s, p};
                p = s.Ptr("next");
            }
            return {View{}, 0};
        };
        auto linked_from = [&](uint64_t sock) -> View {
            for (const View &l : links) {
                if (l.Ptr("tosock") == sock) return ViewAt(f_, l.Ptr("fromnode"), l.group(), "bNode");
            }
            return {};
        };
        auto idname = [](const View &n) { return n.Str("idname"); };

        View shader;
        for (const View &n : nodes) {
            if (idname(n) != "ShaderNodeOutputMaterial") continue;
            auto [surf, sp] = socket(n, "Surface");
            View from = sp ? linked_from(sp) : View{};
            if (from.valid()) {
                shader = from;
                break;
            }
        }
        std::string sid = shader.valid() ? idname(shader) : "";
        if (sid != "ShaderNodeBsdfPrincipled" && sid != "ShaderNodeBsdfDiffuse") {
            shader = View{};
            for (const View &n : nodes) {
                if (idname(n) == "ShaderNodeBsdfPrincipled") {
                    shader = n;
                    break;
                }
            }
            sid = shader.valid() ? idname(shader) : "";
        }
        if (!shader.valid()) return false;

        auto [base, base_ptr] = socket(shader, sid == "ShaderNodeBsdfPrincipled" ? "Base Color" : "Color");
        if (base.valid()) {
            View val = ViewAt(f_, base.Ptr("default_value"), base.group(), "bNodeSocketValueRGBA");
            for (size_t i = 0; val.valid() && i < 4; i++) m->color[i] = static_cast<float>(val.Float("value", 1.0, i));
            View from = linked_from(base_ptr);
            if (from.valid() && idname(from) == "ShaderNodeTexImage") ReadImage(from.Ptr("id"), from.group(), m);
        }
        auto scalar = [&](const char *ident, float *dst) {
            auto [s, sp] = socket(shader, ident);
            View val = ViewAt(f_, s.Ptr("default_value"), s.group(), "bNodeSocketValueFloat");
            if (val.valid()) *dst = static_cast<float>(val.Float("value", static_cast<double>(*dst)));
        };
        scalar("Roughness", &m->roughness);
        if (sid == "ShaderNodeBsdfPrincipled") {
            scalar("Metallic", &m->metallic);
            float alpha = m->color[3];
            scalar("Alpha", &alpha);
            m->color[3] = alpha;
        }
        return true;
    }

    int MaterialIndex(uint64_t ptr, size_t group) {
        View ma = ViewAt(f_, ptr, group, "Material");
        uint64_t key = ma.valid() ? ma.offset() : (uint64_t{1} << 63) | ptr;
        auto it = materials_.find(key);
        if (it != materials_.end()) return it->second;
        SceneMaterial m;
        if (ma.valid()) {
            m.name = IdName(ma);
            m.color[0] = static_cast<float>(ma.Float("r", 0.8));
            m.color[1] = static_cast<float>(ma.Float("g", 0.8));
            m.color[2] = static_cast<float>(ma.Float("b", 0.8));
            m.color[3] = static_cast<float>(ma.Has("a") ? ma.Float("a", 1.0) : ma.Float("alpha", 1.0));
            if (ma.Has("roughness")) m.roughness = static_cast<float>(ma.Float("roughness", 0.5));
            if (ma.Has("metallic")) m.metallic = static_cast<float>(ma.Float("metallic", 0.0));
            bool nodes = f_.version() >= 500 || ma.Int("use_nodes") != 0;
            bool from_nodes = nodes && ReadNodeMaterial(ma, &m);
            if (!from_nodes) {
                // Blender Internal (<= 2.7x) image textures: the first
                // MTex mapped to colour whose Tex is an image.
                const Field *mtex = ma.type()->Find("mtex");
                for (size_t i = 0; mtex && i < mtex->count; i++) {
                    View mt = ma.Deref("mtex", i);
                    if (!mt.valid() || !(mt.Int("mapto") & 1)) continue;
                    View tex = mt.Deref("tex");
                    if (tex.Int("type") != 8) continue;  // TEX_IMAGE
                    ReadImage(tex.Ptr("ima"), tex.group(), &m);
                    break;
                }
            }
        } else {
            m.name = "Material";
            Warn("some materials are linked from other .blend libraries; using a default colour");
        }
        int idx = static_cast<int>(out_->materials.size());
        out_->materials.push_back(std::move(m));
        materials_[key] = idx;
        return idx;
    }

    uint64_t PointerInArray(uint64_t array_ptr, size_t group, size_t i) const {
        size_t off = 0, avail = 0;
        if (!Range(array_ptr, group, &off, &avail)) return 0;
        size_t ps = f_.pointer_size();
        if ((i + 1) * ps > avail) return 0;
        return f_.ReadPointer(off + i * ps);
    }

    // ---- objects ---------------------------------------------------------

    void ExtractObject(const View &ob) {
        std::string name = IdName(ob);
        if (ob.Sub("id").Ptr("lib") != 0) {
            Warn("object " + name + " is linked from another .blend library and was skipped");
            return;
        }
        if (ob.Int("type") != 1) return;  // OB_MESH
        uint64_t me_ptr = ob.Ptr("data");
        View me = ViewAt(f_, me_ptr, ob.group(), "Mesh");
        if (!me.valid()) return;
        if (me.Sub("id").Ptr("lib") != 0) {
            Warn("object " + name + " uses a mesh linked from another .blend library and was skipped");
            return;
        }

        std::vector<std::string> mods;
        for (const View &md : ListItems(ob.Sub("modifiers"), "ModifierData")) mods.push_back(md.Str("name"));
        if (!mods.empty()) {
            std::string list;
            for (const std::string &s : mods) list += (list.empty() ? "" : ", ") + s;
            Warn("object " + name + ": modifiers not applied (" + list + ")");
        }

        auto git = geometry_cache_.find(me.offset());
        if (git == geometry_cache_.end()) git = geometry_cache_.emplace(me.offset(), ReadGeometry(me, name)).first;
        const Geometry &g = git->second;
        if (!g.ok || g.Faces() == 0) return;

        // Material slots: object-linked slots override the mesh's.
        size_t slots = static_cast<size_t>(std::max<int64_t>({1, me.Int("totcol"), ob.Int("totcol")}));
        std::vector<int> slot_material(slots, -1);
        uint64_t matbits_ptr = ob.Ptr("matbits");
        size_t mb_off = 0, mb_avail = 0;
        bool have_matbits = matbits_ptr && Range(matbits_ptr, ob.group(), &mb_off, &mb_avail);
        int64_t colbits = ob.Int("colbits");
        for (size_t s = 0; s < slots; s++) {
            bool object_linked = have_matbits ? (s < mb_avail && f_.data()[mb_off + s] != 0)
                                              : (!ob.Has("matbits") && s < 32 && ((colbits >> s) & 1));
            uint64_t from_ob = PointerInArray(ob.Ptr("mat"), ob.group(), s);
            uint64_t from_me = PointerInArray(me.Ptr("mat"), me.group(), s);
            if (object_linked && from_ob) {
                slot_material[s] = MaterialIndex(from_ob, ob.group());
            } else if (from_me) {
                slot_material[s] = MaterialIndex(from_me, me.group());
            } else if (from_ob) {
                slot_material[s] = MaterialIndex(from_ob, ob.group());
            }
        }

        Mat4 w = World(ob, 0);
        Mat3 nm = NormalMatrix(w);
        bool flip = Det3(w) < 0.0;

        size_t nv = g.pos.size() / 3;
        size_t nf = g.Faces();
        // Face normals (Newell's method) and area-weighted vertex normals.
        std::vector<double> fnorm(nf * 3, 0.0), vnorm(nv * 3, 0.0);
        for (size_t fi = 0; fi < nf; fi++) {
            double n[3] = {0, 0, 0};
            uint32_t s = g.face_start[fi], e = g.face_start[fi + 1];
            for (uint32_t c = s; c < e; c++) {
                const float *a = &g.pos[g.corner_vert[c] * 3];
                const float *b = &g.pos[g.corner_vert[c + 1 < e ? c + 1 : s] * 3];
                n[0] += static_cast<double>((a[1] - b[1]) * (a[2] + b[2]));
                n[1] += static_cast<double>((a[2] - b[2]) * (a[0] + b[0]));
                n[2] += static_cast<double>((a[0] - b[0]) * (a[1] + b[1]));
            }
            for (size_t k = 0; k < 3; k++) fnorm[fi * 3 + k] = n[k];
            for (uint32_t c = s; c < e; c++) {
                for (size_t k = 0; k < 3; k++) vnorm[g.corner_vert[c] * 3 + k] += n[k];
            }
        }

        struct Key {
            uint32_t vert;
            uint32_t face;  // UINT32_MAX for smooth corners
            uint32_t u, v;
            bool operator==(const Key &o) const { return vert == o.vert && face == o.face && u == o.u && v == o.v; }
        };
        struct KeyHash {
            size_t operator()(const Key &k) const {
                uint64_t h = k.vert * 0x9E3779B97F4A7C15ull;
                h ^= (static_cast<uint64_t>(k.face) + 0x632BE59BD9B4E019ull) * 0xBF58476D1CE4E5B9ull;
                h ^= (static_cast<uint64_t>(k.u) << 32 | k.v) * 0x94D049BB133111EBull;
                return static_cast<size_t>(h ^ (h >> 31));
            }
        };
        struct Building {
            ScenePart part;
            std::unordered_map<Key, uint32_t, KeyHash> map;
        };
        std::map<size_t, Building> parts;
        bool has_uv = !g.corner_uv.empty();

        auto emit = [&](Building &bd, uint32_t corner, uint32_t face) -> uint32_t {
            uint32_t v = g.corner_vert[corner];
            Key key{v, g.face_smooth[face] ? UINT32_MAX : face, 0, 0};
            float u = 0, vv = 0;
            if (has_uv) {
                u = g.corner_uv[corner * 2];
                vv = g.corner_uv[corner * 2 + 1];
                std::memcpy(&key.u, &u, 4);
                std::memcpy(&key.v, &vv, 4);
            }
            auto [it, inserted] = bd.map.emplace(key, 0);
            if (!inserted) return it->second;
            ScenePart &p = bd.part;
            uint32_t index = static_cast<uint32_t>(p.positions.size() / 3);
            it->second = index;
            const float *src = &g.pos[v * 3];
            double world[3];
            for (size_t r = 0; r < 3; r++) {
                world[r] = w.m[0][r] * static_cast<double>(src[0]) + w.m[1][r] * static_cast<double>(src[1]) +
                           w.m[2][r] * static_cast<double>(src[2]) + w.m[3][r];
            }
            const double *n = g.face_smooth[face] ? &vnorm[v * 3] : &fnorm[face * 3];
            double wn[3];
            for (size_t r = 0; r < 3; r++) wn[r] = nm[0][r] * n[0] + nm[1][r] * n[1] + nm[2][r] * n[2];
            double len = std::sqrt(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
            if (len > 1e-20) {
                for (double &c : wn) c /= len;
            } else {
                wn[0] = 0;
                wn[1] = 0;
                wn[2] = 1;
            }
            // Z-up -> Y-up: (x, y, z) -> (x, z, -y).
            p.positions.push_back(static_cast<float>(world[0]));
            p.positions.push_back(static_cast<float>(world[2]));
            p.positions.push_back(static_cast<float>(-world[1]));
            p.normals.push_back(static_cast<float>(wn[0]));
            p.normals.push_back(static_cast<float>(wn[2]));
            p.normals.push_back(static_cast<float>(-wn[1]));
            if (has_uv) {
                p.texcoords.push_back(u);
                p.texcoords.push_back(1.0f - vv);
            }
            return index;
        };

        for (size_t fi = 0; fi < nf; fi++) {
            uint32_t s = g.face_start[fi], e = g.face_start[fi + 1];
            if (e - s < 3) continue;
            size_t slot = static_cast<size_t>(std::clamp(g.face_mat[fi], 0, static_cast<int>(slots) - 1));
            Building &bd = parts[slot];
            uint32_t face = static_cast<uint32_t>(fi);
            uint32_t a = emit(bd, s, face);
            for (uint32_t c = s + 1; c + 1 < e; c++) {
                uint32_t b = emit(bd, c, face);
                uint32_t d = emit(bd, c + 1, face);
                bd.part.indices.push_back(a);
                bd.part.indices.push_back(flip ? d : b);
                bd.part.indices.push_back(flip ? b : d);
            }
        }
        for (auto &[slot, bd] : parts) {
            ScenePart &p = bd.part;
            if (p.indices.empty()) continue;
            p.material = slot_material[slot];
            p.name = name;
            if (parts.size() > 1) {
                p.name += "." + (p.material >= 0 ? out_->materials[static_cast<size_t>(p.material)].name : std::to_string(slot));
            }
            out_->parts.push_back(std::move(p));
        }
    }
};

}  // namespace

bool ReadSceneFromBytes(std::string bytes, const std::string &base_dir, Scene *out, std::string *error) {
    File f;
    if (!f.Parse(std::move(bytes), error)) return false;
    Extractor ex(f, base_dir, out);
    return ex.Run(error);
}

bool ReadScene(const std::string &path, Scene *out, std::string *error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t slash = path.find_last_of('/');
    std::string dir = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
    return ReadSceneFromBytes(std::move(bytes), dir, out, error);
}

}  // namespace blend
