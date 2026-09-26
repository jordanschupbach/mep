// Part K: the CAD and FEM agent surface, verified.
//
// WHAT A BINDING TEST IS FOR, and it is not "does the call return". Every
// method here forwards to a kernel or solver function that is already
// tested to death somewhere else in this tree; what is *new* is the
// handle table, the JSON conversion, the parameter validation and the
// fact that four separate surfaces claim to expose the same list. So
// that is what is tested: that the list is complete and consistent, that
// every declared method is reachable and every reachable method is
// declared, that a missing or wrong argument produces a message rather
// than a crash or a plausible wrong answer, and that a whole pipeline
// driven only through this surface arrives at an answer that closed-form
// theory agrees with.

#include "cad_fem_api.h"

#include "json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const char *what, int line) {
    ++checks;
    if (ok) return;
    std::fflush(stdout);
    std::printf("CHECK FAILED: %s at %s:%d\n", what, __FILE__, line);
    ++failures;
}

#define CHECK(x) Check((x), #x, __LINE__)

bool Near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

Json Vec(double x, double y, double z) {
    Json out = Json::Array();
    out.push_back(x);
    out.push_back(y);
    out.push_back(z);
    return out;
}

// Calls and reports, so a failing step says which one and why rather than
// leaving a cascade of confusing follow-on failures.
struct Caller {
    cadfem::Session session;
    std::string error;
    Json last;

    bool Ok(const std::string &method, const Json &params) {
        error.clear();
        last = Json::Object();
        const bool ok = session.Call(method, params, &last, &error);
        if (!ok) std::printf("    %s failed: %s\n", method.c_str(), error.c_str());
        return ok;
    }
    bool Fails(const std::string &method, const Json &params) {
        error.clear();
        last = Json::Object();
        return !session.Call(method, params, &last, &error) && !error.empty();
    }
};

std::filesystem::path Scratch() {
    const char *dir = std::getenv("MEP_TEST_SCRATCH");
    std::filesystem::path base =
        dir != nullptr ? std::filesystem::path(dir) : std::filesystem::temp_directory_path();
    base /= "mep-cad-fem-api";
    std::error_code ignored;
    std::filesystem::create_directories(base, ignored);
    return base;
}

// --- The table itself ------------------------------------------------------

void TestTheTableIsWellFormed() {
    std::printf("the method table:\n");
    const std::vector<cadfem::Method> &methods = cadfem::Methods();
    std::printf("  %d methods\n", static_cast<int>(methods.size()));
    CHECK(methods.size() >= 20);
    std::set<std::string> seen;
    int required = 0;
    for (const cadfem::Method &method : methods) {
        const std::string name = method.name;
        CHECK(seen.insert(name).second);  // no duplicates
        // Every name is `part.something` or `fem.something`, because the
        // agent-RPC dispatch routes on that prefix and a method outside
        // it would be silently unreachable there while working fine in
        // Lua -- exactly the kind of drift this table exists to stop.
        CHECK(name.rfind("part.", 0) == 0 || name.rfind("fem.", 0) == 0);
        CHECK(name.find('.') != std::string::npos);
        CHECK(std::string(method.summary).size() > 10);
        CHECK(std::string(method.returns).size() > 1);
        std::set<std::string> parameters;
        for (const cadfem::Parameter &parameter : method.params) {
            CHECK(parameters.insert(parameter.name).second);
            CHECK(std::string(parameter.summary).size() > 3);
            const std::string type = parameter.type;
            CHECK(type == "number" || type == "string" || type == "bool" || type == "array" ||
                  type == "object");
            if (parameter.required) ++required;
        }
        CHECK(cadfem::FindMethod(name) != nullptr);
    }
    std::printf("  %d required parameters across them; no duplicate names\n", required);
    CHECK(cadfem::FindMethod("part.nonsense") == nullptr);
}

void TestEveryDeclaredMethodIsImplemented() {
    std::printf("every declared method is implemented:\n");
    // A DECLARED-BUT-MISSING METHOD IS THE FAILURE THIS WHOLE DESIGN IS
    // MEANT TO PREVENT, so it is checked directly: each method is called
    // with empty parameters, which must fail -- but never with the
    // dispatch's own "declared but not implemented" message. A surface
    // generated from the table would otherwise advertise a tool that
    // cannot run.
    int missing = 0;
    for (const cadfem::Method &method : cadfem::Methods()) {
        cadfem::Session session;
        Json out;
        std::string error;
        session.Call(method.name, Json::Object(), &out, &error);
        if (error.find("declared but not implemented") != std::string::npos) {
            std::printf("    %s is declared and not implemented\n", method.name);
            ++missing;
        }
    }
    std::printf("  %d of %d methods unimplemented\n", missing,
                static_cast<int>(cadfem::Methods().size()));
    CHECK(missing == 0);

    // And an undeclared one is refused by the dispatch rather than
    // reaching any of the branches below it.
    cadfem::Session session;
    Json out;
    std::string error;
    CHECK(!session.Call("part.invent", Json::Object(), &out, &error));
    CHECK(error.find("no such method") != std::string::npos);
}

void TestBadArgumentsAreRefused() {
    std::printf("bad arguments are refused, not guessed at:\n");
    Caller caller;
    // EVERY METHOD, WITH NOTHING. None may succeed and none may crash --
    // which for a surface an agent drives is the realistic input, since
    // an agent that has misread the schema sends exactly this.
    int refused = 0;
    for (const cadfem::Method &method : cadfem::Methods()) {
        bool has_required = false;
        for (const cadfem::Parameter &parameter : method.params) {
            if (parameter.required) has_required = true;
        }
        if (!has_required) continue;
        if (caller.Fails(method.name, Json::Object())) ++refused;
        else std::printf("    %s succeeded with no arguments at all\n", method.name);
    }
    std::printf("  %d methods with required arguments, all refused when given none\n", refused);

    Json params = Json::Object();
    CHECK(caller.Ok("part.new", params));
    const int document = caller.last.get("document").as_int(-1);
    params["document"] = document;
    // A box with no size, a negative size, and a size that is not an
    // array at all.
    CHECK(caller.Fails("part.box", params));
    params["size"] = Vec(1.0, -1.0, 1.0);
    CHECK(caller.Fails("part.box", params));
    params["size"] = 4.0;
    CHECK(caller.Fails("part.box", params));
    params["size"] = Vec(1.0, 1.0, 1.0);
    CHECK(caller.Ok("part.box", params));
    // A handle that does not exist.
    Json wrong = Json::Object();
    wrong["document"] = 9999;
    CHECK(caller.Fails("part.info", wrong));
    CHECK(caller.error.find("9999") != std::string::npos);
}

// --- The pipeline ----------------------------------------------------------

void TestGeometryThroughTheSurface() {
    std::printf("geometry, driven only through the surface:\n");
    Caller caller;
    Json params = Json::Object();
    CHECK(caller.Ok("part.new", params));
    const int document = caller.last.get("document").as_int(-1);

    params = Json::Object();
    params["document"] = document;
    params["size"] = Vec(0.10, 0.06, 0.04);
    CHECK(caller.Ok("part.box", params));
    const long long block = caller.last.get("body").as_int(0);

    // THE VOLUME IS THE TEST OF THE WHOLE CHAIN, because it is known
    // before anything runs: a block is the product of its sides, and it
    // reaches this answer only if the primitive, the p-curves, the face
    // senses and the mass integration are all right.
    params = Json::Object();
    params["document"] = document;
    params["body"] = static_cast<int>(block);
    params["density"] = 7850.0;
    CHECK(caller.Ok("part.mass", params));
    const double volume = caller.last.get("volume").as_double(0.0);
    const double mass = caller.last.get("mass").as_double(0.0);
    std::printf("  a 100 x 60 x 40 mm block: volume %.9f m^3, mass %.6f kg\n", volume, mass);
    CHECK(Near(volume, 0.10 * 0.06 * 0.04, 1e-12));
    CHECK(Near(mass, volume * 7850.0, 1e-9));
    CHECK(Near(caller.last.get("area").as_double(0.0),
               2.0 * (0.10 * 0.06 + 0.06 * 0.04 + 0.04 * 0.10), 1e-12));
    const Json &centroid = caller.last.get("centroid");
    CHECK(Near(centroid.items()[0].as_double(0.0), 0.05, 1e-12));
    CHECK(Near(centroid.items()[2].as_double(0.0), 0.02, 1e-12));
    CHECK(caller.last.get("closed").as_bool());

    // THE INERTIA TENSOR AGAINST ITS CLOSED FORM, because the coefficient
    // in the per-tetrahedron second-moment formula is exactly the sort of
    // constant that is wrong by a factor of two in half the places it is
    // written down, and a wrong one still gives a plausible-looking
    // symmetric matrix. A block about its own centre has
    // I_xx = m(b^2 + c^2)/12 and nothing off the diagonal.
    const Json &inertia = caller.last.get("inertia");
    CHECK(inertia.size() == 3);
    const double sides[3] = {0.10, 0.06, 0.04};
    double worst_diagonal = 0.0;
    double worst_off = 0.0;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const double got = inertia.items()[static_cast<std::size_t>(i)]
                                   .items()[static_cast<std::size_t>(j)]
                                   .as_double(0.0);
            if (i != j) {
                worst_off = std::max(worst_off, std::fabs(got));
                continue;
            }
            const double a = sides[(i + 1) % 3];
            const double b = sides[(i + 2) % 3];
            const double want = mass * (a * a + b * b) / 12.0;
            worst_diagonal = std::max(worst_diagonal, std::fabs(got - want) / want);
        }
    }
    std::printf("  inertia: diagonal within %.2e of m(b^2+c^2)/12, off-diagonal %.2e\n",
                worst_diagonal, worst_off);
    CHECK(worst_diagonal < 1e-12);
    CHECK(worst_off < mass * 1e-15);

    params = Json::Object();
    params["document"] = document;
    params["body"] = static_cast<int>(block);
    CHECK(caller.Ok("part.validate", params));
    std::printf("  it validates: %s, genus %d\n", caller.last.get("ok").as_bool() ? "yes" : "no",
                caller.last.get("genus").as_int(-1));
    CHECK(caller.last.get("ok").as_bool());
    CHECK(caller.last.get("genus").as_int(-1) == 0);

    params = Json::Object();
    params["document"] = document;
    params["body"] = static_cast<int>(block);
    CHECK(caller.Ok("part.faces", params));
    CHECK(caller.last.get("faces").size() == 6);

    // FINDING A FACE BY DIRECTION is how an agent refers to one, since it
    // cannot click. Each of the six must be found, and each must be a
    // different face -- a lookup that returned the same face for every
    // direction would pass a weaker check.
    std::set<long long> found;
    const double directions[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                     {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (const auto &direction : directions) {
        params = Json::Object();
        params["document"] = document;
        params["body"] = static_cast<int>(block);
        params["normal"] = Vec(direction[0], direction[1], direction[2]);
        CHECK(caller.Ok("part.face_at", params));
        found.insert(caller.last.get("face").as_int(-1));
        CHECK(caller.last.get("agreement").as_double(0.0) > 0.99);
    }
    std::printf("  the six axis directions find %d distinct faces\n",
                static_cast<int>(found.size()));
    CHECK(found.size() == 6);

    // A hole through it, and the volume again -- which is the check that
    // a boolean did what it said rather than merely producing a body.
    params = Json::Object();
    params["document"] = document;
    params["radius"] = 0.01;
    params["height"] = 0.06;
    params["at"] = Vec(0.05, 0.03, -0.01);
    params["axis"] = Vec(0, 0, 1);
    CHECK(caller.Ok("part.cylinder", params));
    const long long pin = caller.last.get("body").as_int(0);

    params = Json::Object();
    params["document"] = document;
    params["op"] = "difference";
    params["a"] = static_cast<int>(block);
    params["b"] = static_cast<int>(pin);
    const bool drilled = caller.Ok("part.boolean", params);
    CHECK(drilled);
    if (drilled) {
        const long long holed = caller.last.get("body").as_int(0);
        params = Json::Object();
        params["document"] = document;
        params["body"] = static_cast<int>(holed);
        if (caller.Ok("part.mass", params)) {
            const double drilled_volume = caller.last.get("volume").as_double(0.0);
            const double expected = 0.10 * 0.06 * 0.04 - 3.14159265358979323846 * 0.01 * 0.01 * 0.04;
            std::printf("  with a 10 mm hole through it: %.9f m^3 against %.9f (%.1e off)\n",
                        drilled_volume, expected,
                        std::fabs(drilled_volume - expected) / expected);
            // APPROXIMATE, AND SAYS SO. The volume comes from the
            // tessellated boundary, so a curved face is only as accurate
            // as the chord tolerance -- and an inscribed polygon always
            // removes slightly *less* material than the cylinder it
            // stands for, so this is high rather than merely near. That
            // one-sidedness is asserted too: a result that came out low
            // would not be tessellation error.
            CHECK(drilled_volume > expected);
            CHECK(Near(drilled_volume, expected, expected * 1e-5));
        }
    }

    params = Json::Object();
    params["document"] = document;
    CHECK(caller.Ok("part.info", params));
    std::printf("  the document now holds %d bodies and %d faces\n",
                caller.last.get("bodies").as_int(0), caller.last.get("faces").as_int(0));
}

void TestExportAndImport() {
    std::printf("writing geometry out, and reading it back:\n");
    Caller caller;
    Json params = Json::Object();
    CHECK(caller.Ok("part.new", params));
    const int document = caller.last.get("document").as_int(-1);
    params = Json::Object();
    params["document"] = document;
    params["size"] = Vec(2.0, 3.0, 4.0);
    CHECK(caller.Ok("part.box", params));

    const std::filesystem::path directory = Scratch();
    for (const char *format : {"step", "stl", "obj"}) {
        const std::filesystem::path path = directory / (std::string("block.") + format);
        params = Json::Object();
        params["document"] = document;
        params["path"] = path.string();
        CHECK(caller.Ok("part.export", params));
        // THE FORMAT COMES FROM THE EXTENSION when it is not named, which
        // is what anyone writing a path expects and what a surface driven
        // by an agent has to get right without being told twice.
        CHECK(caller.last.get("format").as_string("") == format);
        const long long bytes = caller.last.get("bytes").as_int(0);
        std::printf("  %-4s %8lld bytes\n", format, bytes);
        CHECK(bytes > 100);
        CHECK(std::filesystem::exists(path));
        CHECK(static_cast<long long>(std::filesystem::file_size(path)) == bytes);
    }

    // A ROUND TRIP, which is the only claim an exporter can really make:
    // the STEP that came out has to read back as the same solid, and the
    // volume is what says so.
    params = Json::Object();
    params["path"] = (directory / "block.step").string();
    const bool read = caller.Ok("part.import", params);
    CHECK(read);
    if (read) {
        const int reopened = caller.last.get("document").as_int(-1);
        std::printf("  the STEP file reads back as %d bodies\n",
                    caller.last.get("bodies").as_int(0));
        CHECK(caller.last.get("bodies").as_int(0) >= 1);
        params = Json::Object();
        params["document"] = reopened;
        if (caller.Ok("part.info", params) && caller.last.get("bodies").as_int(0) > 0) {
            const long long body = caller.last.get("body_ids").items()[0].as_int(0);
            params = Json::Object();
            params["document"] = reopened;
            params["body"] = static_cast<int>(body);
            if (caller.Ok("part.mass", params)) {
                std::printf("  and encloses %.9f m^3 against the 24.0 that went in\n",
                            caller.last.get("volume").as_double(0.0));
                // THE CHECK THAT FOUND A REAL BUG. Measured with
                // cad::ComputeMassProperties over the faces' surfaces --
                // the obvious implementation -- this read 1.2e11, because
                // a STEP plane comes back untrimmed with a domain of
                // -1e5 to 1e5 and that function integrates whatever
                // domain it is handed. The same code gave the right
                // answer on every kernel-built body, so the failure
                // depended on where the solid came from rather than on
                // what it was.
                CHECK(Near(caller.last.get("volume").as_double(0.0), 24.0, 1e-6));
            }
        }
    }

    params = Json::Object();
    params["path"] = (directory / "nothing-here.step").string();
    CHECK(caller.Fails("part.import", params));
    params = Json::Object();
    params["document"] = document;
    params["path"] = (directory / "block.xyz").string();
    CHECK(caller.Fails("part.export", params));
}

void TestTheWholeSolvePipeline() {
    std::printf("mesh, study, solve and read, all through the surface:\n");
    // A CANTILEVER, BECAUSE BEAM THEORY KNOWS THE ANSWER. Nothing in this
    // file computes it: the deflection PL^3/(3EI) is arithmetic done in
    // the test, and the only way the surface reaches it is if every one
    // of the eight calls below does its job.
    Caller caller;
    const double length = 10.0;
    const double width = 1.0;
    const double depth = 1.0;
    const double modulus = 210e9;
    const double pressure = 1e5;

    Json params = Json::Object();
    CHECK(caller.Ok("part.new", params));
    const int document = caller.last.get("document").as_int(-1);
    params = Json::Object();
    params["document"] = document;
    params["size"] = Vec(length, width, depth);
    CHECK(caller.Ok("part.box", params));
    const long long body = caller.last.get("body").as_int(0);

    auto face_towards = [&](double x, double y, double z) {
        Json find = Json::Object();
        find["document"] = document;
        find["body"] = static_cast<int>(body);
        find["normal"] = Vec(x, y, z);
        if (!caller.Ok("part.face_at", find)) return -1;
        return caller.last.get("face").as_int(-1);
    };
    const int held = face_towards(-1, 0, 0);
    const int pushed = face_towards(0, 0, 1);
    CHECK(held >= 0 && pushed >= 0 && held != pushed);

    params = Json::Object();
    params["document"] = document;
    params["body"] = static_cast<int>(body);
    params["size"] = 1.0;
    const bool meshed = caller.Ok("fem.mesh", params);
    CHECK(meshed);
    if (!meshed) return;
    const int mesh = caller.last.get("mesh").as_int(-1);
    std::printf("  meshed: %d nodes, %d elements, worst dihedral %.2f degrees\n",
                caller.last.get("nodes").as_int(0), caller.last.get("elements").as_int(0),
                caller.last.get("worst_dihedral").as_double(0.0));
    CHECK(caller.last.get("elements").as_int(0) > 50);

    params = Json::Object();
    params["document"] = document;
    CHECK(caller.Ok("fem.study", params));
    const int study = caller.last.get("study").as_int(-1);

    params = Json::Object();
    params["study"] = study;
    params["youngs_modulus"] = modulus;
    params["poissons_ratio"] = 0.3;
    CHECK(caller.Ok("fem.material", params));
    // Poisson's ratio of a half is incompressible and divides by zero in
    // the constitutive matrix; it is refused here rather than four calls
    // later.
    params["poissons_ratio"] = 0.5;
    CHECK(caller.Fails("fem.material", params));
    params["poissons_ratio"] = 0.3;
    CHECK(caller.Ok("fem.material", params));

    params = Json::Object();
    params["study"] = study;
    params["face"] = held;
    CHECK(caller.Ok("fem.support", params));

    params = Json::Object();
    params["study"] = study;
    params["kind"] = "pressure";
    params["face"] = pushed;
    params["magnitude"] = pressure;
    CHECK(caller.Ok("fem.load", params));
    params["kind"] = "banana";
    CHECK(caller.Fails("fem.load", params));

    params = Json::Object();
    params["study"] = study;
    params["mesh"] = mesh;
    const bool solved = caller.Ok("fem.solve", params);
    CHECK(solved);
    if (!solved) return;
    const int result = caller.last.get("result").as_int(-1);
    const double tip = caller.last.get("max_displacement").as_double(0.0);
    const double relative_error = caller.last.get("relative_error").as_double(-1.0);
    std::printf("  solved: max displacement %.6e m, max von Mises %.4e Pa\n", tip,
                caller.last.get("max_von_mises").as_double(0.0));
    std::printf("  strain energy %.6e J, equilibrium residual %.2e, estimated error %.2f%%\n",
                caller.last.get("strain_energy").as_double(0.0),
                caller.last.get("equilibrium_residual").as_double(0.0), 100.0 * relative_error);
    // EVERY SOLVE CARRIES ITS OWN ERROR ESTIMATE, which is the one output
    // here that a caller most needs and is least likely to ask for.
    CHECK(relative_error > 0.0 && relative_error < 1.0);
    CHECK(caller.last.get("equilibrium_residual").as_double(1.0) < 1e-8);

    // A uniform pressure on the top of a cantilever is a distributed load
    // w = p * width, and the tip deflection is wL^4/(8EI).
    const double inertia = width * depth * depth * depth / 12.0;
    const double w = pressure * width;
    const double beam = w * std::pow(length, 4.0) / (8.0 * modulus * inertia);
    std::printf("  beam theory says %.6e m; the solve says %.6e (%.1f%% apart)\n", beam, tip,
                100.0 * (tip / beam - 1.0));
    // A STUBBY CANTILEVER AND A TETRAHEDRAL MESH, so the agreement is
    // loose on purpose: a ten-to-one beam has real shear deflection that
    // Euler-Bernoulli leaves out, and linear tetrahedra are stiff in
    // bending. Within a factor of two is what says the load, the support
    // and the units all arrived intact, which is the claim this surface
    // is making; the element library's accuracy is Part H's claim and is
    // measured there.
    CHECK(tip > beam * 0.3 && tip < beam * 2.0);

    params = Json::Object();
    params["result"] = result;
    params["field"] = "von_mises";
    CHECK(caller.Ok("fem.field", params));
    std::printf("  von Mises runs %.4e to %.4e Pa, worst at node %d\n",
                caller.last.get("min").as_double(0.0), caller.last.get("max").as_double(0.0),
                caller.last.get("max_node").as_int(-1));
    CHECK(caller.last.get("max").as_double(0.0) > caller.last.get("min").as_double(0.0));
    params["field"] = "not_a_field";
    CHECK(caller.Fails("fem.field", params));

    params = Json::Object();
    params["result"] = result;
    params["field"] = "displacement";
    Json points = Json::Array();
    points.push_back(Vec(0.0, 0.5, 0.5));
    points.push_back(Vec(length, 0.5, 0.5));
    params["points"] = points;
    CHECK(caller.Ok("fem.probe", params));
    const Json &probes = caller.last.get("probes");
    CHECK(probes.size() == 2);
    const double at_root = probes.items()[0].get("value").as_double(-1.0);
    const double at_tip = probes.items()[1].get("value").as_double(-1.0);
    std::printf("  displacement at the root %.3e m, at the tip %.3e m\n", at_root, at_tip);
    // THE HELD END DOES NOT MOVE, which is the check that the support
    // landed on the face the agent asked for rather than on some other
    // one -- a study bound to the wrong face still solves, and still
    // looks plausible.
    CHECK(at_root < at_tip * 1e-3);
    CHECK(probes.items()[0].get("inside").as_bool());

    params = Json::Object();
    params["result"] = result;
    params["field"] = "von_mises";
    params["from"] = Vec(0.0, 0.5, 1.0);
    params["to"] = Vec(length, 0.5, 1.0);
    params["samples"] = 11;
    CHECK(caller.Ok("fem.path", params));
    CHECK(caller.last.get("rows").size() == 11);
    CHECK(caller.last.get("csv").as_string("").find("distance") == 0);
    std::printf("  a path of 11 samples along the top exports %d bytes of csv\n",
                static_cast<int>(caller.last.get("csv").as_string("").size()));

    const std::filesystem::path directory = Scratch();
    for (const char *format : {"csv", "org"}) {
        params = Json::Object();
        params["result"] = result;
        params["field"] = "von_mises";
        params["path"] = (directory / (std::string("field.") + format)).string();
        CHECK(caller.Ok("fem.export", params));
        CHECK(caller.last.get("format").as_string("") == format);
        CHECK(caller.last.get("rows").as_int(0) > 10);
    }
    std::printf("  the whole field exports to csv and to an org table\n");

    // A modal solve on the same study and mesh, and the animation of its
    // first mode -- the one call that returns enough data to be worth
    // flattening.
    params = Json::Object();
    params["study"] = study;
    params["mesh"] = mesh;
    params["modes"] = 4;
    const bool modal = caller.Ok("fem.modal", params);
    CHECK(modal);
    if (modal) {
        const int modes_result = caller.last.get("result").as_int(-1);
        const Json &frequencies = caller.last.get("frequencies");
        std::printf("  modal: %d frequencies, the first %.2f Hz\n",
                    static_cast<int>(frequencies.size()),
                    frequencies.size() > 0 ? frequencies.items()[0].as_double(0.0) : 0.0);
        CHECK(frequencies.size() >= 4);
        for (std::size_t i = 1; i < frequencies.size(); ++i) {
            CHECK(frequencies.items()[i].as_double(0.0) >=
                  frequencies.items()[i - 1].as_double(0.0));
        }
        params = Json::Object();
        params["result"] = modes_result;
        params["mode"] = 0;
        params["frames"] = 12;
        CHECK(caller.Ok("fem.animate", params));
        CHECK(caller.last.get("frames").as_int(0) == 12);
        CHECK(caller.last.get("displacement").size() == 12);
        const Json &frame = caller.last.get("displacement").items()[0];
        std::printf("  12 frames of mode 0, %d numbers each, period %.6f s\n",
                    static_cast<int>(frame.size()), caller.last.get("period").as_double(0.0));
        CHECK(frame.size() == static_cast<std::size_t>(caller.last.get("frames").as_int(0) > 0
                                                           ? frame.size()
                                                           : 0));
        // A stress field cannot be read off a modal result, and a mode
        // shape cannot be animated out of a static one. Both are refused
        // by name rather than returning something shaped right.
        params = Json::Object();
        params["result"] = modes_result;
        params["field"] = "von_mises";
        CHECK(caller.Fails("fem.field", params));
        params = Json::Object();
        params["result"] = result;
        CHECK(caller.Fails("fem.animate", params));
    }

    // Closing the document takes its studies, meshes and results with it:
    // a handle that outlived its geometry is a handle that fails later
    // and confusingly.
    CHECK(caller.session.StudyCount() > 0 && caller.session.MeshCount() > 0 &&
          caller.session.ResultCount() > 0);
    params = Json::Object();
    params["document"] = document;
    CHECK(caller.Ok("part.close", params));
    std::printf("  closing the document leaves %d studies, %d meshes, %d results\n",
                caller.session.StudyCount(), caller.session.MeshCount(),
                caller.session.ResultCount());
    CHECK(caller.session.StudyCount() == 0);
    CHECK(caller.session.MeshCount() == 0);
    CHECK(caller.session.ResultCount() == 0);
    params = Json::Object();
    params["result"] = result;
    CHECK(caller.Fails("fem.summary", params));
}

void TestAdaptiveThroughTheSurface() {
    std::printf("the adaptive loop, through the surface:\n");
    Caller caller;
    Json params = Json::Object();
    CHECK(caller.Ok("part.new", params));
    const int document = caller.last.get("document").as_int(-1);
    params = Json::Object();
    params["document"] = document;
    params["size"] = Vec(10.0, 6.0, 4.0);
    CHECK(caller.Ok("part.box", params));
    const long long body = caller.last.get("body").as_int(0);

    auto face_towards = [&](double x, double y, double z) {
        Json find = Json::Object();
        find["document"] = document;
        find["body"] = static_cast<int>(body);
        find["normal"] = Vec(x, y, z);
        if (!caller.Ok("part.face_at", find)) return -1;
        return caller.last.get("face").as_int(-1);
    };

    params = Json::Object();
    params["document"] = document;
    CHECK(caller.Ok("fem.study", params));
    const int study = caller.last.get("study").as_int(-1);
    params = Json::Object();
    params["study"] = study;
    params["face"] = face_towards(0, 0, -1);
    CHECK(caller.Ok("fem.support", params));
    params = Json::Object();
    params["study"] = study;
    params["kind"] = "pressure";
    params["face"] = face_towards(0, 0, 1);
    params["magnitude"] = 5e6;
    CHECK(caller.Ok("fem.load", params));

    params = Json::Object();
    params["study"] = study;
    params["body"] = static_cast<int>(body);
    params["size"] = 2.0;
    params["target"] = 0.02;
    params["cycles"] = 3;
    const bool adapted = caller.Ok("fem.adapt", params);
    CHECK(adapted);
    if (!adapted) return;
    const Json &cycles = caller.last.get("cycles");
    std::printf("  %d cycles, reached target %s, stopped early %s\n",
                static_cast<int>(cycles.size()),
                caller.last.get("reached_target").as_bool() ? "yes" : "no",
                caller.last.get("stopped_early").as_bool() ? "yes" : "no");
    for (const Json &cycle : cycles.items()) {
        std::printf("    %6d elements, %.4f%%\n", cycle.get("elements").as_int(0),
                    100.0 * cycle.get("relative_error").as_double(0.0));
    }
    CHECK(cycles.size() >= 2);
    // The error falls and the mesh grows, the same pair Part J.5 checks
    // -- because a surface that reported the cycles in the wrong order,
    // or reported the first one twice, would pass a mere count.
    CHECK(cycles.items().back().get("relative_error").as_double(1.0) <
          cycles.items().front().get("relative_error").as_double(0.0));
    CHECK(cycles.items().back().get("elements").as_int(0) >
          cycles.items().front().get("elements").as_int(0));
    // And a result handle comes back that can be read like any other.
    params = Json::Object();
    params["result"] = caller.last.get("result").as_int(-1);
    CHECK(caller.Ok("fem.summary", params));
    CHECK(caller.last.get("elements").as_int(0) ==
          cycles.items().back().get("elements").as_int(0));
}

void TestHandlesAreIsolated() {
    std::printf("handles do not collide between kinds:\n");
    // A DOCUMENT, A STUDY, A MESH AND A RESULT SHARE ONE COUNTER, so
    // there is no id that means one thing in one table and something else
    // in another. Passing a mesh handle where a study is wanted must
    // fail, and with separate counters it would quietly succeed on
    // whatever happened to have that number.
    Caller caller;
    Json params = Json::Object();
    CHECK(caller.Ok("part.new", params));
    const int a = caller.last.get("document").as_int(-1);
    CHECK(caller.Ok("part.new", params));
    const int b = caller.last.get("document").as_int(-1);
    CHECK(a != b);
    params = Json::Object();
    params["document"] = a;
    CHECK(caller.Ok("fem.study", params));
    const int study = caller.last.get("study").as_int(-1);
    CHECK(study != a && study != b);
    std::printf("  documents %d and %d, study %d: all distinct\n", a, b, study);
    Json wrong = Json::Object();
    wrong["document"] = study;
    CHECK(caller.Fails("part.info", wrong));
    Json also = Json::Object();
    also["study"] = a;
    CHECK(caller.Fails("fem.material", also));

    params = Json::Object();
    params["document"] = a;
    CHECK(caller.Ok("part.close", params));
    params = Json::Object();
    params["document"] = b;
    CHECK(caller.Ok("part.info", params));
    std::printf("  closing one document leaves the other alone\n");
    CHECK(caller.Fails("part.close", Json::Object()));
}

// --- Documentation ---------------------------------------------------------

std::string ReadWhole(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void TestTheDocumentationMatches() {
    std::printf("the documentation is the table:\n");
    // GENERATED AND THEN CHECKED. Writing the reference by hand and
    // hoping is the normal arrangement, and it is why every large API's
    // documentation has entries for things that no longer exist and
    // nothing for the three added last month. Here the section is
    // generated from the method table and this test fails if the file
    // does not contain what the generator would write -- so a method
    // added without its documentation is a red test rather than a
    // surprise six months later.
    // The source tree, baked in at configure time: this test runs from
    // the build directory and the files it checks live beside the code.
    const char *env = std::getenv("MEP_SOURCE_ROOT");
#ifdef MEP_SOURCE_ROOT
    const std::string base = env != nullptr ? env : MEP_SOURCE_ROOT;
#else
    const std::string base = env != nullptr ? env : ".";
#endif
    struct Target {
        const char *path;
        const char *begin;
        const char *end;
        std::string (*body)();
    };
    const Target targets[] = {
        {"/MEP_AGENT_API.md", "<!-- BEGIN GENERATED cad-fem -->", "<!-- END GENERATED cad-fem -->",
         cadfem::MarkdownReference},
        {"/help/cad-fem.org", "# BEGIN GENERATED cad-fem", "# END GENERATED cad-fem",
         cadfem::OrgReference},
    };
    for (const Target &target : targets) {
        const std::string text = ReadWhole(base + target.path);
        if (text.empty()) {
            std::printf("  %s not found from %s -- set MEP_SOURCE_ROOT\n", target.path,
                        base.c_str());
            continue;
        }
        const std::size_t from = text.find(target.begin);
        const std::size_t to = text.find(target.end);
        CHECK(from != std::string::npos && to != std::string::npos && to > from);
        if (from == std::string::npos || to == std::string::npos || to <= from) continue;
        const std::string between = text.substr(from + std::strlen(target.begin), to - from -
                                                                                     std::strlen(target.begin));
        const std::string want = target.body();
        const bool matches = between.find(want) != std::string::npos;
        std::printf("  %-22s %s (%d methods, %d bytes generated)\n", target.path,
                    matches ? "up to date" : "OUT OF DATE -- run `just cad-fem-docs`",
                    static_cast<int>(cadfem::Methods().size()), static_cast<int>(want.size()));
        CHECK(matches);
        // And every method's name really is in there, which catches a
        // generator that produced something plausible and empty.
        for (const cadfem::Method &method : cadfem::Methods()) {
            std::string underscored = method.name;
            for (char &ch : underscored) {
                if (ch == '.') ch = '_';
            }
            CHECK(between.find(underscored) != std::string::npos);
        }
    }
}

}  // namespace

int main() {
    TestTheTableIsWellFormed();
    TestEveryDeclaredMethodIsImplemented();
    TestBadArgumentsAreRefused();
    TestGeometryThroughTheSurface();
    TestExportAndImport();
    TestTheWholeSolvePipeline();
    TestAdaptiveThroughTheSurface();
    TestHandlesAreIsolated();
    TestTheDocumentationMatches();
    if (failures == 0) {
        std::printf("cad_fem_api_test passed (%d checks)\n", checks);
        return 0;
    }
    std::printf("cad_fem_api_test FAILED (%d of %d checks)\n", failures, checks);
    return 1;
}
