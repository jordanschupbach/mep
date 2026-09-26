#include "cad_pcurve.h"

#include <algorithm>
#include <cmath>

namespace cad {
namespace {

inline std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

// A surface's period in each direction, or 0 where it does not close.
// This is what makes the seam handling possible: a jump of exactly one
// period in parameter space is not a discontinuity, it is the same point
// named twice.
void SurfacePeriods(const Surface &surface, const Tolerance &tolerance, double *period_u, double *period_v) {
    double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
    surface.Domain(&u_lo, &u_hi, &v_lo, &v_hi);
    *period_u = surface.IsClosedU(tolerance) ? (u_hi - u_lo) : 0.0;
    *period_v = surface.IsClosedV(tolerance) ? (v_hi - v_lo) : 0.0;
}

// Is the surface degenerate here -- a pole, where the two tangents are
// parallel (or one vanishes) and the parameterization collapses? At such
// a point the projection's u is meaningless and must come from elsewhere.
bool IsDegenerateAt(const Surface &surface, double u, double v) {
    std::vector<std::vector<Vec3d>> ders;
    surface.Derivatives(u, v, 1, &ders);
    const double area = ders[1][0].Cross(ders[0][1]).Length();
    const double scale = ders[1][0].Length() * ders[0][1].Length();
    // Relative, not absolute: a large surface has large tangents, and an
    // absolute threshold would call every big patch degenerate.
    return scale <= 0.0 || area <= scale * 1e-9;
}

// Shifts `value` by whole periods to land as close to `reference` as
// possible. The whole of the seam logic, in one line of arithmetic.
double UnwrapToward(double value, double reference, double period) {
    if (period <= 0.0) return value;
    return value - period * std::round((value - reference) / period);
}

// Projects one 3D point into a surface's parameter space, seeded near a
// previous answer so that seams unwrap continuously and poles inherit a
// sensible u.
Vec2d ProjectSample(const Surface &surface, const Vec3d &point, const Vec2d &previous, bool have_previous,
                    double period_u, double period_v, const Tolerance &tolerance) {
    double u = 0.0;
    double v = 0.0;
    surface.ClosestPoint(point, &u, &v, nullptr, tolerance);
    if (!have_previous) return Vec2d{u, v};
    // At a pole the returned u is arbitrary, so keep the previous one --
    // the p-curve then runs straight to the pole rather than jumping
    // sideways along a parameter that does not correspond to any motion.
    if (IsDegenerateAt(surface, u, v)) return Vec2d{previous.x, v};
    return Vec2d{UnwrapToward(u, previous.x, period_u), UnwrapToward(v, previous.y, period_v)};
}

// Recognises samples that lie on a straight line in parameter space and
// returns it exactly. Worth doing rather than always fitting: an edge on
// a plane, a seam, and every isoparametric edge all produce straight
// p-curves, which is most p-curves in most models -- and an exact line
// beats a spline that merely passes through the samples.
bool TryFitLine(const std::vector<Vec2d> &samples, double tolerance, std::shared_ptr<const Curve3> *out) {
    if (samples.size() < 2) return false;
    const Vec2d start = samples.front();
    const Vec2d end = samples.back();
    const Vec2d direction = end - start;
    const double length = direction.Length();
    if (length <= 0.0) return false;
    const Vec2d unit = direction / length;
    for (const Vec2d &sample : samples) {
        const Vec2d offset = sample - start;
        const double across = std::fabs(offset.Cross(unit));
        const double along = offset.Dot(unit);
        if (across > tolerance) return false;
        // Also require monotone progress: a curve that doubles back is
        // not a segment even if every sample is on the infinite line.
        if (along < -tolerance || along > length + tolerance) return false;
    }
    *out = std::make_shared<Line3>(Line3::FromPoints(Vec3d{start.x, start.y, 0.0}, Vec3d{end.x, end.y, 0.0}));
    return true;
}

// Recognises samples that lie on a circle in parameter space and returns
// it exactly. The case that makes this worth having is the commonest
// trimmed face there is: a circular edge bounding a planar face -- a
// cylinder's cap, a hole's rim, a disc. Fitting a spline through 64
// samples of a circle leaves about 4e-7 of error, which is above the
// default tolerance and would make every such face fail validation for
// no better reason than that nobody looked for the circle.
bool TryFitCircle(const std::vector<Vec2d> &samples, double tolerance, std::shared_ptr<const Curve3> *out) {
    if (samples.size() < 4) return false;
    // Three well-separated samples determine the circle; the rest verify
    // it. Picking them a third of the way apart avoids the near-collinear
    // configuration that three adjacent samples would give.
    const Vec2d a = samples.front();
    const Vec2d b = samples[samples.size() / 3];
    const Vec2d c = samples[2 * samples.size() / 3];
    Circle3 circle;
    if (!Circle3::FromThreePoints(Vec3d{a.x, a.y, 0.0}, Vec3d{b.x, b.y, 0.0}, Vec3d{c.x, c.y, 0.0}, &circle)) {
        return false;
    }
    const Vec3d center3 = circle.Center();
    const Vec2d center{center3.x, center3.y};
    const double radius = circle.Radius();
    for (const Vec2d &sample : samples) {
        if (std::fabs((sample - center).Length() - radius) > tolerance) return false;
    }
    // Rebuild with a frame and sweep matching the samples' own direction,
    // so the p-curve runs the way the coedge does rather than whichever
    // way FromThreePoints happened to orient it.
    const Vec2d first = samples.front() - center;
    const Vec2d last = samples.back() - center;
    const Vec2d middle = samples[samples.size() / 2] - center;
    const double start_angle = std::atan2(first.y, first.x);
    double mid_angle = std::atan2(middle.y, middle.x) - start_angle;
    double end_angle = std::atan2(last.y, last.x) - start_angle;
    // Unwrap the sweep so it is monotone from the start, in whichever
    // direction the middle sample says the curve actually goes.
    while (mid_angle < 0.0) mid_angle += kTwoPi;
    while (end_angle <= 0.0) end_angle += kTwoPi;
    if (mid_angle > end_angle) {
        // Travelling clockwise: flip the frame's y axis and re-measure.
        const Vec3d x_axis{first.x / radius, first.y / radius, 0.0};
        const Vec3d y_axis = Vec3d{0.0, 0.0, -1.0}.Cross(x_axis);
        double sweep = -end_angle + kTwoPi;
        if (sweep <= 0.0) sweep += kTwoPi;
        *out = std::make_shared<Circle3>(Vec3d{center.x, center.y, 0.0}, x_axis, y_axis, radius, 0.0, sweep);
        return true;
    }
    const Vec3d x_axis{first.x / radius, first.y / radius, 0.0};
    const Vec3d y_axis = Vec3d{0.0, 0.0, 1.0}.Cross(x_axis);
    *out = std::make_shared<Circle3>(Vec3d{center.x, center.y, 0.0}, x_axis, y_axis, radius, 0.0, end_angle);
    return true;
}

}  // namespace

Vec2d PCurvePoint(const Curve3 &pcurve, double t) {
    const Vec3d p = pcurve.Point(t);
    return Vec2d{p.x, p.y};
}

Vec2d PCurveTangent(const Curve3 &pcurve, double t) {
    std::vector<Vec3d> ders;
    pcurve.Derivatives(t, 1, &ders);
    return Vec2d{ders[1].x, ders[1].y};
}

// A sample sitting exactly on a degenerate point of the surface -- a
// sphere's pole, a cone's apex -- has one parameter that means nothing:
// every value of it maps to the same point, so the inversion returns
// whichever one the arithmetic happened to land on. That is decided by
// the last bit of a cosine which is supposed to be zero, and it comes out
// differently depending on how the edge's parameters were written down.
//
// Found by reading a sphere back from a STEP file. The file carried the
// same seam as the sphere it was written from, described with its
// parameters a full turn along -- the same arc by every geometric
// measure -- and the seam projected to a different place in parameter
// space, leaving a face whose boundary crossed its own middle.
//
// The free parameter is taken from the nearest sample that is not
// degenerate. Which one is free is not assumed: it is whichever
// derivative vanished.
void FixDegenerateSamples(const Surface &surface, std::vector<Vec2d> *samples) {
    const std::size_t count = samples->size();
    if (count < 2) return;
    std::vector<int> free_axis(count, -1);  // 0 = u is meaningless, 1 = v is
    bool any = false;
    std::vector<std::vector<Vec3d>> ders;
    for (std::size_t i = 0; i < count; ++i) {
        const Vec2d &uv = (*samples)[i];
        surface.Derivatives(uv.x, uv.y, 1, &ders);
        const double du = ders[1][0].LengthSquared();
        const double dv = ders[0][1].LengthSquared();
        const double scale = std::max(du, dv);
        if (!(scale > 0.0)) continue;
        if (du <= scale * 1e-18) {
            free_axis[i] = 0;
            any = true;
        } else if (dv <= scale * 1e-18) {
            free_axis[i] = 1;
            any = true;
        }
    }
    if (!any) return;
    auto copy_from = [&](std::size_t target, std::size_t source) {
        if (free_axis[target] == 0) {
            (*samples)[target].x = (*samples)[source].x;
        } else {
            (*samples)[target].y = (*samples)[source].y;
        }
        free_axis[target] = -1;
    };
    for (std::size_t i = 1; i < count; ++i) {
        if (free_axis[i] >= 0 && free_axis[i - 1] < 0) copy_from(i, i - 1);
    }
    for (std::size_t i = count - 1; i > 0; --i) {
        if (free_axis[i - 1] >= 0 && free_axis[i] < 0) copy_from(i - 1, i);
    }
}

bool BuildLoopPCurves(Model *model, EntityId loop_id, const PCurveOptions &options, std::string *error) {
    const Loop *loop = model->GetLoop(loop_id);
    if (loop == nullptr) {
        *error = "no such loop";
        return false;
    }
    const Face *face = model->GetFace(loop->face);
    if (face == nullptr) {
        *error = "loop " + std::to_string(loop_id) + " has no face";
        return false;
    }
    const Surface *surface = model->SurfaceAt(face->surface);
    if (surface == nullptr) {
        *error = "face " + std::to_string(face->id) + " has no surface";
        return false;
    }
    double period_u = 0.0;
    double period_v = 0.0;
    SurfacePeriods(*surface, options.tolerance, &period_u, &period_v);
    double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
    surface->Domain(&u_lo, &u_hi, &v_lo, &v_hi);

    const int sample_count = std::max(3, options.sample_count);

    // Pass one: sample every coedge in its own traversal direction,
    // unwrapping within the coedge only. The result is continuous along
    // each coedge but each may sit on the wrong side of a seam.
    std::vector<std::vector<Vec2d>> per_coedge;
    for (EntityId coedge_id : loop->coedges) {
        const CoEdge *coedge = model->GetCoEdge(coedge_id);
        if (coedge == nullptr) {
            *error = "loop " + std::to_string(loop_id) + " references a missing coedge";
            return false;
        }
        const Edge *edge = model->GetEdge(coedge->edge);
        if (edge == nullptr) {
            *error = "coedge " + std::to_string(coedge_id) + " references a missing edge";
            return false;
        }
        const Curve3 *curve = model->CurveAt(edge->curve);
        if (curve == nullptr) {
            *error = "edge " + std::to_string(edge->id) + " has no curve";
            return false;
        }
        std::vector<Vec2d> samples;
        samples.reserve(Idx(sample_count));
        Vec2d previous;
        bool have_previous = false;
        for (int i = 0; i < sample_count; ++i) {
            const double fraction = static_cast<double>(i) / static_cast<double>(sample_count - 1);
            // Traverse in the coedge's direction, so the p-curve runs the
            // way the loop does.
            const double t = coedge->orientation == Orientation::Forward
                                 ? edge->t_start + fraction * (edge->t_end - edge->t_start)
                                 : edge->t_end - fraction * (edge->t_end - edge->t_start);
            const Vec3d point = curve->Point(t);
            const Vec2d uv = ProjectSample(*surface, point, previous, have_previous, period_u, period_v,
                                           options.tolerance);
            samples.push_back(uv);
            previous = uv;
            have_previous = true;
        }
        FixDegenerateSamples(*surface, &samples);
        per_coedge.push_back(std::move(samples));
    }
    if (per_coedge.empty()) {
        *error = "loop " + std::to_string(loop_id) + " has no coedges";
        return false;
    }

    // Pass two: place each coedge that lies on a seam.
    //
    // This is the part that cannot be done by chaining each coedge to its
    // predecessor, and it took a wrong implementation to make that
    // obvious. A sphere's face is bounded by *one* seam edge used twice;
    // chaining places the second use next to where the first ended, so
    // both land on the same side of the parameter rectangle and the loop
    // encloses zero area. The two uses have to end up a full period
    // apart, and nothing local to either one says so.
    //
    // The rule that does work is geometric: for a counter-clockwise outer
    // loop the face's interior lies to the *left* of the direction of
    // travel. On a u-seam, travelling in +v puts "left" at -u, so the
    // coedge belongs at u_hi (the interior is below it); travelling in -v
    // puts it at u_lo. A v-seam is the same statement rotated: +u belongs
    // at v_lo, -u at v_hi.
    //
    // Checked against all three periodic primitives -- it reproduces what
    // the cylinder and torus already had, and fixes the sphere.
    const double seam_tolerance = 1e-9;
    std::vector<bool> placed(per_coedge.size(), false);
    for (std::size_t i = 0; i < per_coedge.size(); ++i) {
        std::vector<Vec2d> &samples = per_coedge[i];
        if (samples.size() < 2) continue;

        if (period_u > 0.0) {
            // Constant in u, and that constant is on the domain boundary
            // modulo the period?
            double min_u = samples.front().x;
            double max_u = samples.front().x;
            for (const Vec2d &sample : samples) {
                min_u = std::min(min_u, sample.x);
                max_u = std::max(max_u, sample.x);
            }
            const double offset_from_boundary =
                std::fabs(UnwrapToward(min_u, u_lo, period_u) - u_lo);
            if (max_u - min_u <= seam_tolerance && offset_from_boundary <= seam_tolerance) {
                const double dv = samples.back().y - samples.front().y;
                const double target = (dv >= 0.0) ? u_hi : u_lo;
                for (Vec2d &sample : samples) sample.x = target;
                placed[i] = true;
                continue;
            }
        }
        if (period_v > 0.0) {
            double min_v = samples.front().y;
            double max_v = samples.front().y;
            for (const Vec2d &sample : samples) {
                min_v = std::min(min_v, sample.y);
                max_v = std::max(max_v, sample.y);
            }
            const double offset_from_boundary =
                std::fabs(UnwrapToward(min_v, v_lo, period_v) - v_lo);
            if (max_v - min_v <= seam_tolerance && offset_from_boundary <= seam_tolerance) {
                const double du = samples.back().x - samples.front().x;
                const double target = (du >= 0.0) ? v_lo : v_hi;
                for (Vec2d &sample : samples) sample.y = target;
                placed[i] = true;
                continue;
            }
        }
    }

    // Pass three: everything not on a seam is placed by continuity with a
    // neighbour that has been, falling back to the domain's centre when
    // the loop has no seam at all (the ordinary case, where any placement
    // works because nothing is ambiguous).
    for (std::size_t pass = 0; pass < per_coedge.size(); ++pass) {
        bool progress = false;
        for (std::size_t i = 0; i < per_coedge.size(); ++i) {
            if (placed[i]) continue;
            const std::size_t previous = (i + per_coedge.size() - 1) % per_coedge.size();
            const std::size_t next = (i + 1) % per_coedge.size();
            const Vec2d *anchor = nullptr;
            if (placed[previous]) {
                anchor = &per_coedge[previous].back();
            } else if (placed[next]) {
                anchor = &per_coedge[next].front();
            }
            if (anchor == nullptr) continue;
            const Vec2d &reference = *anchor;
            const Vec2d &start = placed[previous] ? per_coedge[i].front() : per_coedge[i].back();
            const double shift_u = UnwrapToward(start.x, reference.x, period_u) - start.x;
            const double shift_v = UnwrapToward(start.y, reference.y, period_v) - start.y;
            for (Vec2d &sample : per_coedge[i]) {
                sample.x += shift_u;
                sample.y += shift_v;
            }
            placed[i] = true;
            progress = true;
        }
        if (!progress) break;
    }
    for (std::size_t i = 0; i < per_coedge.size(); ++i) {
        if (placed[i]) continue;
        const double anchor_u = 0.5 * (u_lo + u_hi);
        const double anchor_v = 0.5 * (v_lo + v_hi);
        const Vec2d &start = per_coedge[i].front();
        const double shift_u = UnwrapToward(start.x, anchor_u, period_u) - start.x;
        const double shift_v = UnwrapToward(start.y, anchor_v, period_v) - start.y;
        for (Vec2d &sample : per_coedge[i]) {
            sample.x += shift_u;
            sample.y += shift_v;
        }
        placed[i] = true;
    }

    // Pass four: fit and attach.
    for (std::size_t i = 0; i < loop->coedges.size(); ++i) {
        const std::vector<Vec2d> &samples = per_coedge[i];
        std::shared_ptr<const Curve3> pcurve;
        if (!TryFitLine(samples, options.fit_tolerance, &pcurve) &&
            !TryFitCircle(samples, options.fit_tolerance, &pcurve)) {
            std::vector<Vec3d> lifted;
            lifted.reserve(samples.size());
            for (const Vec2d &sample : samples) lifted.push_back(Vec3d{sample.x, sample.y, 0.0});
            NurbsCurve3 fitted;
            if (!NurbsCurve3::Interpolate(lifted, std::min(options.fit_degree, static_cast<int>(lifted.size()) - 1),
                                          Parameterization::Centripetal, &fitted)) {
                *error = "could not fit a p-curve for coedge " + std::to_string(loop->coedges[i]);
                return false;
            }
            pcurve = std::make_shared<NurbsCurve3>(fitted);
        }
        const int index = model->AddPCurve(pcurve);
        if (CoEdge *coedge = model->GetCoEdge(loop->coedges[i])) coedge->pcurve = index;
    }
    return true;
}

bool BuildAllPCurves(Model *model, const PCurveOptions &options, std::string *error) {
    for (const Loop &loop : model->Loops()) {
        bool needs_build = false;
        for (EntityId c : loop.coedges) {
            const CoEdge *coedge = model->GetCoEdge(c);
            if (coedge != nullptr && coedge->pcurve < 0) needs_build = true;
        }
        if (!needs_build) continue;
        if (!BuildLoopPCurves(model, loop.id, options, error)) return false;
    }
    return true;
}

double LoopSignedArea(const Model &model, EntityId loop_id, int samples_per_coedge) {
    const Loop *loop = model.GetLoop(loop_id);
    if (loop == nullptr) return 0.0;
    std::vector<Vec2d> polygon;
    for (EntityId coedge_id : loop->coedges) {
        const CoEdge *coedge = model.GetCoEdge(coedge_id);
        if (coedge == nullptr) continue;
        const Curve3 *pcurve = model.PCurveAt(coedge->pcurve);
        if (pcurve == nullptr) return 0.0;
        double lo = 0.0;
        double hi = 0.0;
        pcurve->Domain(&lo, &hi);
        const int steps = std::max(2, samples_per_coedge);
        for (int i = 0; i < steps; ++i) {
            const double t = lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(steps);
            polygon.push_back(PCurvePoint(*pcurve, t));
        }
        polygon.push_back(PCurvePoint(*pcurve, hi));
    }
    if (polygon.size() < 3) return 0.0;
    // Shoelace. Any gap between one coedge's end and the next one's start
    // is closed by the straight segment the polygon already implies --
    // see this function's note in the header on why that is correct for a
    // pole rather than a defect being hidden.
    double twice_area = 0.0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Vec2d &a = polygon[i];
        const Vec2d &b = polygon[(i + 1) % polygon.size()];
        twice_area += a.Cross(b);
    }
    return 0.5 * twice_area;
}

}  // namespace cad
