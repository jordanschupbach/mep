// DXF, for 2D sketches (Part F.4).
//
// DXF is a list of (group code, value) pairs, two lines each, and an
// entity is a run of them starting at code 0. That is the whole format as
// far as anything here is concerned, and it is why the reader below is
// short: everything hard about DXF is in the parts nobody exchanging a
// profile uses.
//
// WHAT DOES NOT SURVIVE is the constraints. DXF has no way to say "these
// two lines are perpendicular" or "this dimension is 40", so a sketch
// written out and read back is geometry and nothing else -- the same
// geometry, in the same place, and no longer parametric. `.mepcad` is
// the format that keeps them, and this one is for handing a profile to
// somebody whose tools only read DXF.

#include "cad_exchange.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace cad {
namespace {

std::string Number(double v) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.10g", v);
    return buffer;
}

void Pair(std::string *out, int code, const std::string &value) {
    *out += std::to_string(code) + "\n" + value + "\n";
}

void Pair(std::string *out, int code, double value) { Pair(out, code, Number(value)); }

std::string Trim(const std::string &text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) --end;
    return text.substr(begin, end - begin);
}

struct Group {
    int code = 0;
    std::string value;
};

// One entity: the pairs from its 0/NAME up to the next 0. Repeated codes
// are kept in order, because an LWPOLYLINE's vertices are a run of 10s
// and 20s and collapsing them would lose the polygon.
struct DxfEntity {
    std::string type;
    std::vector<Group> groups;

    double Get(int code, double fallback, int occurrence = 0) const {
        int seen = 0;
        for (const Group &group : groups) {
            if (group.code != code) continue;
            if (seen++ != occurrence) continue;
            return std::strtod(group.value.c_str(), nullptr);
        }
        return fallback;
    }
    int Count(int code) const {
        int seen = 0;
        for (const Group &group : groups) {
            if (group.code == code) ++seen;
        }
        return seen;
    }
};

}  // namespace

bool WriteDxf(const Sketch &sketch, std::string *out, std::string *error) {
    error->clear();
    out->clear();
    // The smallest DXF a reader will accept: a header saying which
    // version, then the entities. No tables, no blocks -- both are
    // optional and every reader copes without them.
    Pair(out, 0, "SECTION");
    Pair(out, 2, "HEADER");
    Pair(out, 9, "$ACADVER");
    Pair(out, 1, "AC1015");
    Pair(out, 9, "$INSUNITS");
    Pair(out, 70, "4");
    Pair(out, 0, "ENDSEC");
    Pair(out, 0, "SECTION");
    Pair(out, 2, "ENTITIES");

    const SketchPlane &plane = sketch.Plane();
    auto point_of = [&](SketchId id, Vec2d *out_point) {
        const SketchPoint *point = sketch.GetPoint(id);
        if (point == nullptr) return false;
        *out_point = point->position;
        return true;
    };

    for (const SketchEntity &entity : sketch.Entities()) {
        // Construction geometry goes on its own layer rather than being
        // dropped: it is what the profile was built against, and a
        // recipient can hide a layer.
        const std::string layer = entity.construction ? "construction" : "profile";
        switch (entity.kind) {
            case SketchEntityKind::Line: {
                Vec2d a;
                Vec2d b;
                if (entity.points.size() < 2 || !point_of(entity.points[0], &a) ||
                    !point_of(entity.points[1], &b)) {
                    continue;
                }
                Pair(out, 0, "LINE");
                Pair(out, 8, layer);
                Pair(out, 10, a.x);
                Pair(out, 20, a.y);
                Pair(out, 30, 0.0);
                Pair(out, 11, b.x);
                Pair(out, 21, b.y);
                Pair(out, 31, 0.0);
                break;
            }
            case SketchEntityKind::Circle: {
                Vec2d c;
                if (entity.points.empty() || !point_of(entity.points[0], &c)) continue;
                Pair(out, 0, "CIRCLE");
                Pair(out, 8, layer);
                Pair(out, 10, c.x);
                Pair(out, 20, c.y);
                Pair(out, 30, 0.0);
                Pair(out, 40, entity.scalars.empty() ? 1.0 : entity.scalars[0]);
                break;
            }
            case SketchEntityKind::Arc: {
                Vec2d centre;
                Vec2d start;
                Vec2d end;
                if (entity.points.size() < 3 || !point_of(entity.points[0], &centre) ||
                    !point_of(entity.points[1], &start) || !point_of(entity.points[2], &end)) {
                    continue;
                }
                // DXF arcs always run counter-clockwise from the start
                // angle to the end angle, so a clockwise one is written
                // with its ends swapped rather than with a negative
                // sweep, which DXF has no way to say.
                const double radius = (start - centre).Length();
                double from = std::atan2(start.y - centre.y, start.x - centre.x);
                double to = std::atan2(end.y - centre.y, end.x - centre.x);
                if (!entity.arc_ccw) std::swap(from, to);
                Pair(out, 0, "ARC");
                Pair(out, 8, layer);
                Pair(out, 10, centre.x);
                Pair(out, 20, centre.y);
                Pair(out, 30, 0.0);
                Pair(out, 40, radius);
                Pair(out, 50, from * 180.0 / kPi);
                Pair(out, 51, to * 180.0 / kPi);
                break;
            }
            case SketchEntityKind::Ellipse: {
                Vec2d centre;
                if (entity.points.empty() || !point_of(entity.points[0], &centre)) continue;
                const double major = entity.scalars.size() > 0 ? entity.scalars[0] : 1.0;
                const double minor = entity.scalars.size() > 1 ? entity.scalars[1] : 1.0;
                const double rotation = entity.scalars.size() > 2 ? entity.scalars[2] : 0.0;
                Pair(out, 0, "ELLIPSE");
                Pair(out, 8, layer);
                Pair(out, 10, centre.x);
                Pair(out, 20, centre.y);
                Pair(out, 30, 0.0);
                // The major axis as a vector from the centre, which is
                // how DXF stores it rather than as a length and an angle.
                Pair(out, 11, major * std::cos(rotation));
                Pair(out, 21, major * std::sin(rotation));
                Pair(out, 31, 0.0);
                Pair(out, 40, major > 0.0 ? minor / major : 1.0);
                Pair(out, 41, 0.0);
                Pair(out, 42, kTwoPi);
                break;
            }
            case SketchEntityKind::Spline: {
                // As a polyline through its control points. A DXF SPLINE
                // exists, but its knot conventions are a reliable source
                // of disagreement between writers, and a recipient who
                // wanted the exact curve would be asking for STEP.
                Pair(out, 0, "LWPOLYLINE");
                Pair(out, 8, layer);
                Pair(out, 90, std::to_string(entity.points.size()));
                Pair(out, 70, "0");
                for (SketchId id : entity.points) {
                    Vec2d p;
                    if (!point_of(id, &p)) continue;
                    Pair(out, 10, p.x);
                    Pair(out, 20, p.y);
                }
                break;
            }
            case SketchEntityKind::Point: {
                Vec2d p;
                if (entity.points.empty() || !point_of(entity.points[0], &p)) continue;
                Pair(out, 0, "POINT");
                Pair(out, 8, layer);
                Pair(out, 10, p.x);
                Pair(out, 20, p.y);
                Pair(out, 30, 0.0);
                break;
            }
        }
    }
    Pair(out, 0, "ENDSEC");
    Pair(out, 0, "EOF");
    (void)plane;
    return true;
}

bool ReadDxf(const std::string &text, const SketchPlane &plane, Sketch *out,
             std::vector<std::string> *unsupported, std::string *error) {
    error->clear();
    unsupported->clear();
    *out = Sketch(plane);

    // Split into (code, value) pairs. A DXF line may have leading spaces
    // on the code, which older writers produce.
    std::vector<Group> groups;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t newline = text.find('\n', at);
        if (newline == std::string::npos) newline = text.size();
        const std::string code_text = Trim(text.substr(at, newline - at));
        at = newline + 1;
        if (at > text.size()) break;
        std::size_t value_end = text.find('\n', at);
        if (value_end == std::string::npos) value_end = text.size();
        const std::string value = Trim(text.substr(at, value_end - at));
        at = value_end + 1;
        if (code_text.empty()) continue;
        char *stop = nullptr;
        const long code = std::strtol(code_text.c_str(), &stop, 10);
        if (stop == code_text.c_str()) {
            *error = "a group code is not a number: '" + code_text + "'";
            return false;
        }
        groups.push_back(Group{static_cast<int>(code), value});
    }
    if (groups.empty()) {
        *error = "the file has no group codes in it, so it is not DXF";
        return false;
    }

    // Entities: a run beginning at each 0, inside the ENTITIES section.
    std::vector<DxfEntity> entities;
    bool in_entities = false;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (groups[i].code != 0) continue;
        if (groups[i].value == "SECTION") {
            in_entities = i + 1 < groups.size() && groups[i + 1].code == 2 &&
                          groups[i + 1].value == "ENTITIES";
            continue;
        }
        if (groups[i].value == "ENDSEC" || groups[i].value == "EOF") {
            in_entities = false;
            continue;
        }
        if (!in_entities) continue;
        DxfEntity entity;
        entity.type = groups[i].value;
        for (std::size_t j = i + 1; j < groups.size() && groups[j].code != 0; ++j) {
            entity.groups.push_back(groups[j]);
        }
        entities.push_back(std::move(entity));
    }

    std::map<std::string, int> skipped;
    for (const DxfEntity &entity : entities) {
        const bool construction = false;
        if (entity.type == "LINE") {
            out->AddLine(Vec2d{entity.Get(10, 0.0), entity.Get(20, 0.0)},
                         Vec2d{entity.Get(11, 0.0), entity.Get(21, 0.0)}, construction);
        } else if (entity.type == "CIRCLE") {
            out->AddCircle(Vec2d{entity.Get(10, 0.0), entity.Get(20, 0.0)}, entity.Get(40, 1.0),
                           construction);
        } else if (entity.type == "ARC") {
            const Vec2d centre{entity.Get(10, 0.0), entity.Get(20, 0.0)};
            const double radius = entity.Get(40, 1.0);
            const double from = entity.Get(50, 0.0) * kPi / 180.0;
            const double to = entity.Get(51, 0.0) * kPi / 180.0;
            out->AddArc(centre, centre + Vec2d{radius * std::cos(from), radius * std::sin(from)},
                        centre + Vec2d{radius * std::cos(to), radius * std::sin(to)}, true,
                        construction);
        } else if (entity.type == "LWPOLYLINE") {
            const int count = entity.Count(10);
            std::vector<SketchId> points;
            for (int i = 0; i < count; ++i) {
                points.push_back(out->AddPoint(Vec2d{entity.Get(10, 0.0, i), entity.Get(20, 0.0, i)}));
            }
            // Group code 70 bit 1 says the polyline is closed.
            const bool closed = (static_cast<int>(entity.Get(70, 0.0)) & 1) != 0;
            for (std::size_t i = 0; i + 1 < points.size(); ++i) {
                out->AddLineFromPoints(points[i], points[i + 1], construction);
            }
            if (closed && points.size() > 2) {
                out->AddLineFromPoints(points.back(), points.front(), construction);
            }
        } else if (entity.type == "POINT") {
            out->AddPoint(Vec2d{entity.Get(10, 0.0), entity.Get(20, 0.0)});
        } else if (entity.type == "ELLIPSE") {
            const Vec2d centre{entity.Get(10, 0.0), entity.Get(20, 0.0)};
            const Vec2d major_axis{entity.Get(11, 1.0), entity.Get(21, 0.0)};
            const double major = major_axis.Length();
            out->AddEllipse(centre, major, major * entity.Get(40, 1.0),
                            std::atan2(major_axis.y, major_axis.x), construction);
        } else {
            ++skipped[entity.type];
        }
    }
    for (const auto &entry : skipped) {
        unsupported->push_back(entry.first + " x" + std::to_string(entry.second));
    }
    return true;
}

}  // namespace cad
