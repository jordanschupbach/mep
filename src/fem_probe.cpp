#include "fem_probe.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fem {
namespace {

std::string Number(double value) {
    if (!std::isfinite(value)) {
        return std::isnan(value) ? "nan" : (value > 0.0 ? "inf" : "-inf");
    }
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return buffer;
}

}  // namespace

bool ProbePoints(const AnalysisModel &model, const std::vector<double> &values,
                 const std::vector<cad::Vec3d> &at, std::vector<Probe> *out, std::string *error) {
    out->clear();
    if (values.size() != model.nodes.size()) {
        *error = "the field is not one value per node";
        return false;
    }
    std::vector<double> sampled;
    MapReport report;
    if (!MapField(model.nodes, model.elements, values, at, &sampled, &report)) {
        *error = report.error;
        return false;
    }
    // MapField reports how many points were outside and the worst
    // distance, but not *which* ones -- it was written for a mesh-to-mesh
    // transfer where the aggregate is what matters. A probe needs it per
    // point, so each is asked on its own. That is slower by the number of
    // points, which for a handful of probes is nothing and for a path of
    // a few hundred is still nothing next to the solve that produced the
    // field.
    for (std::size_t i = 0; i < at.size(); ++i) {
        Probe probe;
        probe.at = at[i];
        probe.value = sampled[i];
        MapReport one;
        std::vector<double> ignored;
        if (MapField(model.nodes, model.elements, values, {at[i]}, &ignored, &one)) {
            probe.inside = one.inside > 0;
            probe.distance = one.worst_distance;
        }
        out->push_back(probe);
    }
    return true;
}

bool ProbePath(const AnalysisModel &model, const std::vector<double> &values,
               const std::vector<cad::Vec3d> &through, int samples,
               std::vector<PathSample> *out, std::string *error) {
    out->clear();
    if (through.size() < 2) {
        *error = "a path needs at least two points";
        return false;
    }
    if (samples < 2) {
        *error = "a path plot needs at least two samples";
        return false;
    }
    std::vector<double> cumulative{0.0};
    for (std::size_t i = 1; i < through.size(); ++i) {
        cumulative.push_back(cumulative.back() + (through[i] - through[i - 1]).Length());
    }
    const double total = cumulative.back();
    if (!(total > 0.0)) {
        *error = "the path has no length";
        return false;
    }
    std::vector<cad::Vec3d> at;
    std::vector<double> distance;
    for (int s = 0; s < samples; ++s) {
        const double want = total * static_cast<double>(s) / static_cast<double>(samples - 1);
        const auto upper = std::upper_bound(cumulative.begin(), cumulative.end(), want);
        std::size_t segment = static_cast<std::size_t>(upper - cumulative.begin());
        if (segment == 0) segment = 1;
        if (segment >= cumulative.size()) segment = cumulative.size() - 1;
        const double span = cumulative[segment] - cumulative[segment - 1];
        const double alpha = span > 0.0 ? (want - cumulative[segment - 1]) / span : 0.0;
        at.push_back(through[segment - 1] + (through[segment] - through[segment - 1]) * alpha);
        distance.push_back(want);
    }
    std::vector<Probe> probes;
    if (!ProbePoints(model, values, at, &probes, error)) return false;
    for (std::size_t i = 0; i < probes.size(); ++i) {
        PathSample sample;
        sample.distance = distance[i];
        sample.at = probes[i].at;
        sample.value = probes[i].value;
        sample.inside = probes[i].inside;
        out->push_back(sample);
    }
    return true;
}

bool ProbeTimeHistory(const AnalysisModel &model, const std::vector<double> &times,
                      const std::vector<std::vector<double>> &states,
                      const std::vector<cad::Vec3d> &at, TimeHistory *out, std::string *error) {
    *out = TimeHistory{};
    if (times.size() != states.size()) {
        *error = "there is not one recorded state per recorded time";
        return false;
    }
    if (times.empty()) {
        *error = "the history is empty";
        return false;
    }
    out->at = at;
    out->time = times;
    // THE POINTS ARE LOCATED ONCE, NOT ONCE PER FRAME. Inverting the
    // isoparametric map is Newton iteration, and a history of a thousand
    // frames at ten points would run it ten thousand times for an answer
    // that cannot change: the mesh does not move between frames of a
    // thermal transient. Locating once and reusing the shape functions is
    // the difference between a probe that is instant and one that is not.
    std::vector<double> marker(model.nodes.size(), 0.0);
    std::vector<std::vector<double>> weights(at.size());
    for (std::size_t p = 0; p < at.size(); ++p) {
        weights[p].assign(model.nodes.size(), 0.0);
    }
    for (std::size_t node = 0; node < model.nodes.size(); ++node) {
        std::fill(marker.begin(), marker.end(), 0.0);
        marker[node] = 1.0;
        std::vector<double> sampled;
        MapReport report;
        if (!MapField(model.nodes, model.elements, marker, at, &sampled, &report)) {
            *error = report.error;
            return false;
        }
        for (std::size_t p = 0; p < at.size(); ++p) weights[p][node] = sampled[p];
    }
    std::vector<Probe> where;
    if (!ProbePoints(model, marker, at, &where, error)) return false;
    for (const Probe &probe : where) out->inside.push_back(probe.inside);
    for (std::size_t frame = 0; frame < times.size(); ++frame) {
        if (states[frame].size() != model.nodes.size()) {
            *error = "a recorded state is not one value per node";
            return false;
        }
        std::vector<double> row(at.size(), 0.0);
        for (std::size_t p = 0; p < at.size(); ++p) {
            double sum = 0.0;
            for (std::size_t node = 0; node < model.nodes.size(); ++node) {
                if (weights[p][node] == 0.0) continue;
                sum += weights[p][node] * states[frame][node];
            }
            row[p] = sum;
        }
        out->value.push_back(std::move(row));
    }
    return true;
}

Table PathTable(const std::vector<PathSample> &path, const std::string &field) {
    Table table;
    table.columns = {"distance", "x", "y", "z", field};
    for (const PathSample &sample : path) {
        table.rows.push_back(
            {sample.distance, sample.at.x, sample.at.y, sample.at.z, sample.value});
    }
    return table;
}

Table HistoryTable(const TimeHistory &history, const std::string &field) {
    Table table;
    table.columns.push_back("time");
    for (std::size_t p = 0; p < history.at.size(); ++p) {
        char buffer[80];
        std::snprintf(buffer, sizeof(buffer), "%s at (%g, %g, %g)", field.c_str(),
                      history.at[p].x, history.at[p].y, history.at[p].z);
        table.columns.push_back(buffer);
    }
    for (std::size_t frame = 0; frame < history.time.size(); ++frame) {
        std::vector<double> row{history.time[frame]};
        for (const double value : history.value[frame]) row.push_back(value);
        table.rows.push_back(std::move(row));
    }
    return table;
}

std::string ToCsv(const Table &table) {
    std::string out;
    for (std::size_t c = 0; c < table.columns.size(); ++c) {
        if (c > 0) out += ",";
        // A column name with a comma in it -- which "von Mises at (1, 2,
        // 3)" has three of -- would otherwise silently become three
        // columns, and the file would still parse.
        const bool quote = table.columns[c].find_first_of(",\"\n") != std::string::npos;
        if (!quote) {
            out += table.columns[c];
            continue;
        }
        out += '"';
        for (const char ch : table.columns[c]) {
            if (ch == '"') out += '"';
            out += ch;
        }
        out += '"';
    }
    out += "\n";
    for (const std::vector<double> &row : table.rows) {
        for (std::size_t c = 0; c < row.size(); ++c) {
            if (c > 0) out += ",";
            out += Number(row[c]);
        }
        out += "\n";
    }
    return out;
}

std::string ToOrgTable(const Table &table) {
    // Rendered with the numbers formatted first, so the column widths can
    // be measured and the table lines up in a plain text editor. An org
    // table that does not line up is still valid and is unreadable, which
    // for something being pasted into a notebook is the whole point.
    std::vector<std::vector<std::string>> cells;
    cells.push_back(table.columns);
    for (const std::vector<double> &row : table.rows) {
        std::vector<std::string> line;
        for (const double value : row) line.push_back(Number(value));
        cells.push_back(std::move(line));
    }
    std::size_t width = 0;
    for (const std::vector<std::string> &row : cells) width = std::max(width, row.size());
    std::vector<std::size_t> widths(width, 0);
    for (const std::vector<std::string> &row : cells) {
        for (std::size_t c = 0; c < row.size(); ++c) widths[c] = std::max(widths[c], row[c].size());
    }
    auto line = [&](const std::vector<std::string> &row) {
        std::string out = "|";
        for (std::size_t c = 0; c < width; ++c) {
            const std::string &cell = c < row.size() ? row[c] : std::string();
            out += " " + cell + std::string(widths[c] - cell.size(), ' ') + " |";
        }
        out += "\n";
        return out;
    };
    std::string out = line(cells.front());
    out += "|";
    for (std::size_t c = 0; c < width; ++c) out += std::string(widths[c] + 2, '-') + "+";
    if (!out.empty() && out.back() == '+') out.back() = '|';
    out += "\n";
    for (std::size_t r = 1; r < cells.size(); ++r) out += line(cells[r]);
    return out;
}

}  // namespace fem
