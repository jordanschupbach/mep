#ifndef MEP_FEM_PROBE_H
#define MEP_FEM_PROBE_H

#include "fem_map.h"
#include "fem_study.h"

#include <string>
#include <vector>

// Probes, path plots and time histories (plans/CAD_FEM_PLAN.md Part J.4).
//
// A PROBE IS A FIELD MAPPING ONTO ONE POINT, and that is not a glib
// remark -- it is the reason there is no point-location code here. Part
// I.3 already had to find which element a physical point falls in, invert
// the isoparametric map onto it by Newton, and decide what to do about a
// point that lands just outside a faceted boundary, because a
// thermal-to-structural coupling needs exactly that. Writing a second
// point locator for the results pane would be writing the harder half of
// I.3 again, and the copy that is only exercised interactively is the one
// that would be wrong.
//
// So `ProbePoints` is `MapField` with a different name for its target,
// and the interesting content here is the rest: arc length along a path,
// histories through time, and getting the numbers out in a form the
// spreadsheet and org tooling already read.
namespace fem {

struct Probe {
    cad::Vec3d at;
    double value = 0.0;
    // False when the point fell outside every element. The value is then
    // the nearest element's, evaluated at its closest point, and
    // `distance` says how far that was -- which is the difference between
    // a point a hair outside a faceted boundary and a point somewhere
    // else entirely, and only the caller can judge which it has.
    bool inside = true;
    double distance = 0.0;
};

bool ProbePoints(const AnalysisModel &model, const std::vector<double> &values,
                 const std::vector<cad::Vec3d> &at, std::vector<Probe> *out, std::string *error);

struct PathSample {
    // Arc length from the start of the path, which is the x axis of the
    // plot this exists to draw.
    double distance = 0.0;
    cad::Vec3d at;
    double value = 0.0;
    bool inside = true;
};

// Evenly spaced samples along a polyline. EVENLY IN ARC LENGTH, NOT PER
// SEGMENT: a polyline whose segments differ in length would otherwise be
// sampled densely on the short ones and sparsely on the long, and the
// plot would show a feature moving as the path was re-drawn.
bool ProbePath(const AnalysisModel &model, const std::vector<double> &values,
               const std::vector<cad::Vec3d> &through, int samples,
               std::vector<PathSample> *out, std::string *error);

struct TimeHistory {
    std::vector<double> time;
    // One row per time, one column per probe point.
    std::vector<std::vector<double>> value;
    std::vector<cad::Vec3d> at;
    // Per point, as `Probe` reports it -- constant through the history,
    // since the points do not move.
    std::vector<bool> inside;
};

// A recorded transient (a solve's `sample_times` and `samples`) read at
// fixed points.
bool ProbeTimeHistory(const AnalysisModel &model, const std::vector<double> &times,
                      const std::vector<std::vector<double>> &states,
                      const std::vector<cad::Vec3d> &at, TimeHistory *out, std::string *error);

// --- Getting the numbers out ----------------------------------------------
//
// A result that cannot leave the program is half a result. Both formats
// are written here rather than reached for through the editor because
// they are the *contents* of a table, and the editor's spreadsheet and
// org buffers read text.
struct Table {
    std::vector<std::string> columns;
    std::vector<std::vector<double>> rows;
};

Table PathTable(const std::vector<PathSample> &path, const std::string &field);
Table HistoryTable(const TimeHistory &history, const std::string &field);

// Comma-separated, which is what the spreadsheet reads.
//
// SEVENTEEN SIGNIFICANT DIGITS, not six. A result exported to be plotted
// can spare the characters, and a result exported to be *compared* --
// against another run, another code, an earlier version of this one --
// is worthless rounded: the difference being looked for is usually
// smaller than what six digits keeps. Seventeen is what round-trips a
// double exactly.
std::string ToCsv(const Table &table);

// An org-mode table, which lands in a notebook as a table rather than as
// a block of text, and which org's own spreadsheet can then compute on.
std::string ToOrgTable(const Table &table);

}  // namespace fem

#endif
