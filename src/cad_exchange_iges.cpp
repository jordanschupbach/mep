// IGES read (Part F.4).
//
// IGES IS A PUNCHED-CARD FORMAT and reads like one. Every line is exactly
// eighty columns: seventy-two of content, a section letter in column
// seventy-three, and a sequence number after it. There are five sections
// -- Start, Global, Directory Entry, Parameter Data, Terminate -- and the
// two that matter are the last two before the end: the directory is a
// fixed-width table with two eighty-column lines per entity, and the
// parameter data is free-form, comma-separated, terminated by a
// semicolon, with each record pointing back at its directory line.
//
// An entity is referred to by its *directory line number*, which is odd
// (1, 3, 5, ...) because each entry takes two lines. Getting that wrong
// is the classic IGES reader bug: the pointers are line numbers, not
// entity indices, and they are one-based.
//
// WHAT IS READ is the B-rep family -- 186 manifold solid, 514 shell, 510
// face, 508 loop, 504 edge list, 502 vertex list -- and the geometry
// under it. Trimmed-surface files (type 144) are far more common in the
// wild and are a different shape of problem; they are counted and named
// rather than half-read. IGES is not written at all: files arrive in it,
// and nothing is improved by sending one back when STEP says the same
// things better.

#include "cad_exchange.h"
#include "cad_pcurve.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace cad {
namespace {

struct DirectoryEntry {
    int type = 0;
    int parameter_line = 0;   // first line of its parameter record
    int line_count = 0;
    int form = 0;
    std::string label;
};

struct IgesData {
    std::map<int, DirectoryEntry> directory;  // by directory line number (odd)
    std::map<int, std::vector<std::string>> parameters;  // by directory line number
    double unit_scale = 1.0;
};

std::string Trim(const std::string &text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) --end;
    return text.substr(begin, end - begin);
}

int Field(const std::string &line, int from, int width) {
    const std::size_t at = static_cast<std::size_t>(from);
    if (at >= line.size()) return 0;
    return std::atoi(Trim(line.substr(at, static_cast<std::size_t>(width))).c_str());
}

double AsReal(const std::string &text) {
    // IGES writes exponents with D as often as with E, being older than
    // the convention that settled on E.
    std::string fixed = text;
    for (char &c : fixed) {
        if (c == 'D' || c == 'd') c = 'E';
    }
    return std::strtod(fixed.c_str(), nullptr);
}

bool Parse(const std::string &text, IgesData *out, std::string *error) {
    std::vector<std::string> lines;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t newline = text.find('\n', at);
        if (newline == std::string::npos) newline = text.size();
        std::string line = text.substr(at, newline - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        at = newline + 1;
    }
    if (lines.empty()) {
        *error = "the file is empty";
        return false;
    }

    std::vector<std::string> directory_lines;
    std::string parameter_text;
    std::vector<int> parameter_owner;  // the directory pointer of each parameter line
    bool any_section = false;
    for (const std::string &line : lines) {
        if (line.size() < 73) continue;
        const char section = line[72];
        any_section = true;
        if (section == 'G') {
            // The global section's third-from-last useful field is the
            // model space scale; reading it properly needs the Hollerith
            // parser, and every file in practice uses 1.0 there. The
            // units flag is what actually matters and it is not applied
            // here -- see the note in the header about what is read.
            continue;
        }
        if (section == 'D') {
            directory_lines.push_back(line.substr(0, 72));
        } else if (section == 'P') {
            parameter_text += line.substr(0, 64);
            parameter_owner.push_back(Field(line, 64, 8));
        }
    }
    if (!any_section) {
        *error = "no line of this file has a section letter in column 73, so it is not IGES";
        return false;
    }
    if (directory_lines.size() % 2 != 0) {
        *error = "the directory section has an odd number of lines; every entry takes two";
        return false;
    }

    for (std::size_t i = 0; i + 1 < directory_lines.size(); i += 2) {
        DirectoryEntry entry;
        entry.type = Field(directory_lines[i], 0, 8);
        entry.parameter_line = Field(directory_lines[i], 8, 8);
        entry.line_count = Field(directory_lines[i + 1], 24, 8);
        entry.form = Field(directory_lines[i + 1], 32, 8);
        entry.label = Trim(directory_lines[i + 1].substr(56, 8));
        // Directory pointers are *line* numbers, one-based and odd.
        out->directory[static_cast<int>(i) + 1] = entry;
    }

    // Parameter records: split the accumulated text on semicolons, and
    // attribute each record to the directory entry its lines pointed at.
    std::string record;
    std::size_t line_index = 0;
    std::size_t consumed = 0;
    for (std::size_t i = 0; i < parameter_text.size(); ++i) {
        record.push_back(parameter_text[i]);
        ++consumed;
        if (consumed == 64) {
            consumed = 0;
            ++line_index;
        }
        if (parameter_text[i] != ';') continue;
        const int owner = line_index < parameter_owner.size()
                              ? parameter_owner[line_index]
                              : (parameter_owner.empty() ? 0 : parameter_owner.back());
        std::vector<std::string> fields;
        std::string field;
        for (char c : record) {
            if (c == ',' || c == ';') {
                fields.push_back(Trim(field));
                field.clear();
            } else {
                field.push_back(c);
            }
        }
        out->parameters[owner] = std::move(fields);
        record.clear();
    }
    return true;
}

class IgesReader {
public:
    IgesReader(const IgesData &data, Model *model, IgesReadReport *report)
        : data_(data), model_(model), report_(report) {}

    bool Run();

private:
    const DirectoryEntry *Entry(int pointer) const {
        const auto found = data_.directory.find(pointer);
        return found == data_.directory.end() ? nullptr : &found->second;
    }
    const std::vector<std::string> *Params(int pointer) const {
        const auto found = data_.parameters.find(pointer);
        return found == data_.parameters.end() ? nullptr : &found->second;
    }
    static double Real(const std::vector<std::string> &fields, std::size_t index, double fallback) {
        return index < fields.size() ? AsReal(fields[index]) : fallback;
    }
    static int Integer(const std::vector<std::string> &fields, std::size_t index, int fallback) {
        return index < fields.size() ? std::atoi(fields[index].c_str()) : fallback;
    }

    int CurveOf(int pointer);
    int SurfaceOf(int pointer);
    EntityId VertexOf(int list_pointer, int index);
    EntityId EdgeOf(int list_pointer, int index);
    bool LoopOf(int pointer, std::vector<EntityId> *coedges);
    EntityId FaceOf(int pointer);

    const IgesData &data_;
    Model *model_;
    IgesReadReport *report_;
    std::map<int, int> curves_;
    std::map<int, int> surfaces_;
    std::map<long long, EntityId> vertices_;
    std::map<long long, EntityId> edges_;
};

int IgesReader::CurveOf(int pointer) {
    const auto found = curves_.find(pointer);
    if (found != curves_.end()) return found->second;
    const DirectoryEntry *entry = Entry(pointer);
    const std::vector<std::string> *fields = Params(pointer);
    int index = -1;
    if (entry != nullptr && fields != nullptr) {
        if (entry->type == 110) {
            // Line: x1,y1,z1,x2,y2,z2.
            const Vec3d a{Real(*fields, 1, 0.0), Real(*fields, 2, 0.0), Real(*fields, 3, 0.0)};
            const Vec3d b{Real(*fields, 4, 0.0), Real(*fields, 5, 0.0), Real(*fields, 6, 0.0)};
            index = model_->AddCurve(std::make_shared<Line3>(Line3::FromPoints(a, b)));
        } else if (entry->type == 100) {
            // Circular arc, in a plane z = ZT, by centre and two ends.
            const double z = Real(*fields, 1, 0.0);
            const Vec2d centre{Real(*fields, 2, 0.0), Real(*fields, 3, 0.0)};
            const Vec2d start{Real(*fields, 4, 0.0), Real(*fields, 5, 0.0)};
            const double radius = (start - centre).Length();
            index = model_->AddCurve(std::make_shared<Circle3>(Vec3d{centre.x, centre.y, z},
                                                               Vec3d{1.0, 0.0, 0.0},
                                                               Vec3d{0.0, 1.0, 0.0}, radius, 0.0,
                                                               kTwoPi));
        } else if (entry->type == 126) {
            // Rational B-spline curve: K, M, flags, then knots, weights,
            // control points.
            const int last = Integer(*fields, 1, 0);
            const int degree = Integer(*fields, 2, 3);
            const int rational = Integer(*fields, 4, 0) == 0 ? 1 : 0;
            const int count = last + 1;
            const int knot_count = count + degree + 1;
            std::size_t at = 7;
            std::vector<double> knots;
            for (int i = 0; i < knot_count; ++i) knots.push_back(Real(*fields, at++, 0.0));
            std::vector<double> weights;
            for (int i = 0; i < count; ++i) weights.push_back(Real(*fields, at++, 1.0));
            std::vector<Vec4d> control;
            for (int i = 0; i < count; ++i) {
                const double x = Real(*fields, at++, 0.0);
                const double y = Real(*fields, at++, 0.0);
                const double z = Real(*fields, at++, 0.0);
                const double w = rational != 0 ? weights[static_cast<std::size_t>(i)] : 1.0;
                control.push_back(Vec4d{x * w, y * w, z * w, w});
            }
            auto curve = std::make_shared<NurbsCurve3>();
            std::string error;
            if (NurbsCurve3::Create(degree, knots, control, curve.get(), &error)) {
                index = model_->AddCurve(curve);
            }
        }
    }
    if (index < 0 && entry != nullptr) ++report_->unsupported[entry->type];
    curves_[pointer] = index;
    return index;
}

int IgesReader::SurfaceOf(int pointer) {
    const auto found = surfaces_.find(pointer);
    if (found != surfaces_.end()) return found->second;
    const DirectoryEntry *entry = Entry(pointer);
    const std::vector<std::string> *fields = Params(pointer);
    int index = -1;
    if (entry != nullptr && fields != nullptr) {
        if (entry->type == 190) {
            // Plane surface: a point and a normal, by pointer.
            const int point_pointer = Integer(*fields, 1, 0);
            const int normal_pointer = Integer(*fields, 2, 0);
            const std::vector<std::string> *point = Params(point_pointer);
            const std::vector<std::string> *normal = Params(normal_pointer);
            if (point != nullptr && normal != nullptr) {
                const Vec3d origin{Real(*point, 1, 0.0), Real(*point, 2, 0.0), Real(*point, 3, 0.0)};
                const Vec3d n = Vec3d{Real(*normal, 1, 0.0), Real(*normal, 2, 0.0),
                                      Real(*normal, 3, 1.0)}
                                    .Normalized();
                const Vec3d x = n.AnyPerpendicular();
                index = model_->AddSurface(
                    std::make_shared<PlaneSurface>(origin, x, n.Cross(x), -1e5, 1e5, -1e5, 1e5));
            }
        } else if (entry->type == 192) {
            // Right circular cylindrical surface: a point, an axis and a
            // radius.
            const std::vector<std::string> *point = Params(Integer(*fields, 1, 0));
            const std::vector<std::string> *axis = Params(Integer(*fields, 2, 0));
            if (point != nullptr && axis != nullptr) {
                const Vec3d origin{Real(*point, 1, 0.0), Real(*point, 2, 0.0), Real(*point, 3, 0.0)};
                const Vec3d a = Vec3d{Real(*axis, 1, 0.0), Real(*axis, 2, 0.0), Real(*axis, 3, 1.0)}
                                    .Normalized();
                index = model_->AddSurface(std::make_shared<CylinderSurface>(
                    origin, a.AnyPerpendicular(), a, Real(*fields, 3, 1.0), 0.0, kTwoPi, -1e5, 1e5));
            }
        } else if (entry->type == 196) {
            const std::vector<std::string> *point = Params(Integer(*fields, 1, 0));
            if (point != nullptr) {
                const Vec3d centre{Real(*point, 1, 0.0), Real(*point, 2, 0.0), Real(*point, 3, 0.0)};
                index = model_->AddSurface(
                    std::make_shared<SphereSurface>(centre, Real(*fields, 2, 1.0)));
            }
        } else if (entry->type == 128) {
            // Rational B-spline surface.
            const int last_u = Integer(*fields, 1, 0);
            const int last_v = Integer(*fields, 2, 0);
            const int degree_u = Integer(*fields, 3, 3);
            const int degree_v = Integer(*fields, 4, 3);
            const int rational = Integer(*fields, 7, 0) == 0 ? 1 : 0;
            const int count_u = last_u + 1;
            const int count_v = last_v + 1;
            std::size_t at = 10;
            std::vector<double> knots_u;
            for (int i = 0; i < count_u + degree_u + 1; ++i) knots_u.push_back(Real(*fields, at++, 0.0));
            std::vector<double> knots_v;
            for (int i = 0; i < count_v + degree_v + 1; ++i) knots_v.push_back(Real(*fields, at++, 0.0));
            std::vector<double> weights;
            for (int i = 0; i < count_u * count_v; ++i) weights.push_back(Real(*fields, at++, 1.0));
            // IGES stores the grid v-major; this kernel wants u-major.
            std::vector<Vec4d> control(static_cast<std::size_t>(count_u * count_v));
            for (int j = 0; j < count_v; ++j) {
                for (int i = 0; i < count_u; ++i) {
                    const double x = Real(*fields, at++, 0.0);
                    const double y = Real(*fields, at++, 0.0);
                    const double z = Real(*fields, at++, 0.0);
                    const double w =
                        rational != 0 ? weights[static_cast<std::size_t>(j * count_u + i)] : 1.0;
                    control[static_cast<std::size_t>(i * count_v + j)] =
                        Vec4d{x * w, y * w, z * w, w};
                }
            }
            auto surface = std::make_shared<NurbsSurface>();
            std::string error;
            if (NurbsSurface::Create(degree_u, degree_v, knots_u, knots_v, control, count_u, count_v,
                                     surface.get(), &error)) {
                index = model_->AddSurface(surface);
            }
        }
    }
    if (index < 0 && entry != nullptr) ++report_->unsupported[entry->type];
    surfaces_[pointer] = index;
    return index;
}

EntityId IgesReader::VertexOf(int list_pointer, int index) {
    const long long key = static_cast<long long>(list_pointer) * 100000 + index;
    const auto found = vertices_.find(key);
    if (found != vertices_.end()) return found->second;
    const std::vector<std::string> *fields = Params(list_pointer);
    EntityId id = kNoEntity;
    if (fields != nullptr && index >= 1) {
        // 502: type, count, then x,y,z per vertex. Index is one-based.
        const std::size_t at = 2 + static_cast<std::size_t>(index - 1) * 3;
        id = model_->AddVertex(Vec3d{Real(*fields, at, 0.0), Real(*fields, at + 1, 0.0),
                                     Real(*fields, at + 2, 0.0)});
    }
    vertices_[key] = id;
    return id;
}

EntityId IgesReader::EdgeOf(int list_pointer, int index) {
    const long long key = static_cast<long long>(list_pointer) * 100000 + index;
    const auto found = edges_.find(key);
    if (found != edges_.end()) return found->second;
    const std::vector<std::string> *fields = Params(list_pointer);
    EntityId id = kNoEntity;
    if (fields != nullptr && index >= 1) {
        // 504: type, count, then five fields per edge -- the curve, the
        // start vertex list and index, the end vertex list and index.
        const std::size_t at = 2 + static_cast<std::size_t>(index - 1) * 5;
        const int curve_pointer = Integer(*fields, at, 0);
        const EntityId start = VertexOf(Integer(*fields, at + 1, 0), Integer(*fields, at + 2, 0));
        const EntityId end = VertexOf(Integer(*fields, at + 3, 0), Integer(*fields, at + 4, 0));
        const int curve = CurveOf(curve_pointer);
        if (curve >= 0 && start != kNoEntity && end != kNoEntity) {
            const Curve3 *c = model_->CurveAt(curve);
            double lo = 0.0;
            double hi = 1.0;
            c->Domain(&lo, &hi);
            double t_start = lo;
            double t_end = hi;
            Vec3d hit;
            c->ClosestPoint(model_->GetVertex(start)->point, &t_start, &hit);
            c->ClosestPoint(model_->GetVertex(end)->point, &t_end, &hit);
            if (start == end) {
                t_end = t_start + (c->Kind() == CurveKind::Circle ? kTwoPi : hi - lo);
            } else if (c->Kind() == CurveKind::Circle && t_end <= t_start) {
                t_end += kTwoPi;
            }
            id = model_->AddEdge(curve, start, end, t_start, t_end);
        }
    }
    edges_[key] = id;
    return id;
}

bool IgesReader::LoopOf(int pointer, std::vector<EntityId> *coedges) {
    const std::vector<std::string> *fields = Params(pointer);
    if (fields == nullptr) return false;
    // 508: type, n, then per edge: type flag, edge list pointer, index,
    // orientation, and a count of parameter-space curves (followed by
    // that many pairs, which are skipped).
    const int count = Integer(*fields, 1, 0);
    std::size_t at = 2;
    for (int i = 0; i < count; ++i) {
        const int list_pointer = Integer(*fields, at + 1, 0);
        const int index = Integer(*fields, at + 2, 0);
        const bool forward = Integer(*fields, at + 3, 1) != 0;
        const int isop_count = Integer(*fields, at + 4, 0);
        at += 5 + static_cast<std::size_t>(isop_count) * 2;
        const EntityId edge = EdgeOf(list_pointer, index);
        if (edge == kNoEntity) return false;
        coedges->push_back(
            model_->AddCoEdge(edge, forward ? Orientation::Forward : Orientation::Reversed));
    }
    return !coedges->empty();
}

EntityId IgesReader::FaceOf(int pointer) {
    const std::vector<std::string> *fields = Params(pointer);
    if (fields == nullptr) return kNoEntity;
    // 510: surface pointer, loop count, outer-loop flag, then the loops.
    const int surface = SurfaceOf(Integer(*fields, 1, 0));
    if (surface < 0) return kNoEntity;
    const int loop_count = Integer(*fields, 2, 0);
    const bool outer_first = Integer(*fields, 3, 1) != 0;
    std::vector<EntityId> loops;
    for (int i = 0; i < loop_count; ++i) {
        std::vector<EntityId> coedges;
        if (!LoopOf(Integer(*fields, 4 + static_cast<std::size_t>(i), 0), &coedges)) {
            report_->warnings.push_back("a loop of face " + std::to_string(pointer) +
                                        " could not be built");
            continue;
        }
        loops.push_back(model_->AddLoop(coedges, i == 0 && outer_first));
    }
    if (loops.empty()) return kNoEntity;
    if (!outer_first) model_->GetLoop(loops.front())->is_outer = true;
    return model_->AddFace(surface, Orientation::Forward, loops, "iges");
}

bool IgesReader::Run() {
    for (const auto &entry : data_.directory) {
        if (entry.second.type != 186) continue;  // manifold solid B-rep
        const std::vector<std::string> *fields = Params(entry.first);
        if (fields == nullptr) continue;
        const std::vector<std::string> *shell = Params(Integer(*fields, 1, 0));
        if (shell == nullptr) continue;
        // 514: type, face count, then (face pointer, orientation) pairs.
        const int faces = Integer(*shell, 1, 0);
        std::vector<EntityId> built;
        for (int i = 0; i < faces; ++i) {
            const EntityId face = FaceOf(Integer(*shell, 2 + static_cast<std::size_t>(i) * 2, 0));
            if (face != kNoEntity) built.push_back(face);
        }
        if (built.empty()) continue;
        report_->bodies.push_back(
            model_->AddBody({model_->AddShell(built, true)}, entry.second.label));
    }
    if (report_->bodies.empty()) {
        report_->error =
            "this file has no solid (IGES type 186) that could be built. Trimmed-surface files -- "
            "type 144, which is what most IGES exporters produce -- are not read";
        return false;
    }
    std::string error;
    BuildAllPCurves(model_, {}, &error);
    report_->ok = true;
    return true;
}

}  // namespace

bool ReadIgesText(const std::string &text, Model *out, IgesReadReport *report) {
    *report = IgesReadReport{};
    IgesData data;
    if (!Parse(text, &data, &report->error)) return false;
    IgesReader reader(data, out, report);
    return reader.Run();
}

}  // namespace cad
