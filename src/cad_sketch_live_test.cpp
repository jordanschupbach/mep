// mep-cad-sketch-live-test: builds a CAD sketch inside a real, running
// mep by driving its agent socket, exactly as an external agent would
// (plans/CAD_FEM_PLAN.md Part D.5).
//
// What this covers that mep-cad-sketch-test cannot. That test exercises
// cad_sketch.h and cad_constraint.h directly and proves the geometry is
// right. This one goes through the whole stack the interactive tool is
// built on -- JSON-RPC over the socket, the dispatch in agent_rpc.cpp,
// the Editor::CadSketch* methods, the solver, and back out as JSON -- and
// so is the test that would catch an argument transposed in a binding, a
// session not being refreshed after a change, or a failure reported as a
// success. The keyboard path in Mode::CadSketch calls those same Editor
// methods, which is why testing this surface tests both.
//
// Needs a live display, same requirement as mep-agent-rpc-test and for
// the same reason: it launches the real `mep` binary.
//
// Two sections, each against its own freshly spawned mep:
//
//   OVER THE SOCKET -- the sketch.* methods alone, building a plate with
//   two holes and then changing one of its dimensions.
//   KEYBOARD AND MOUSE -- Mode::CadSketch's own input: the rectangle tool
//   driven by real clicks, a constraint applied with a keystroke, and a
//   dimension typed into the prompt. That the two agree is the point --
//   both end in the same Editor::CadSketch* calls.
//
// Usage: mep-cad-sketch-live-test /path/to/mep

#include "agent_rpc_test_harness.h"

// Drives a live mep's sketch.* methods the way an agent would: build a
// plate with two holes entirely over the socket, then check that what
// comes back is the plate that was asked for.
void TestOverTheSocket(const char *mep_path) {
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) { execl(mep_path, mep_path, "--no-session", nullptr); _exit(127); }

    const std::string socket_path = MepAgentSocketDir() + "/" + std::to_string(static_cast<long>(pid)) + ".sock";
    int fd = -1;
    for (int i = 0; i < 100 && fd < 0; i++) {
        if (std::filesystem::exists(socket_path)) fd = ConnectOnce(socket_path);
        if (fd < 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK_CTX(fd >= 0, "mep never bound its agent socket within 10s");
    std::string buf;
    int next_id = 1;
    auto call = [&](const std::string &method, const Json &params) {
        Json r = Call(fd, next_id++, method, params, &buf);
        CHECK_CTX(r.contains("result"), method + " failed: " + r.dump());
        return r.get("result");
    };
    auto obj = [](std::initializer_list<std::pair<std::string, Json>> fields) {
        Json j = Json::Object();
        for (const auto &f : fields) j[f.first] = f.second;
        return j;
    };

    const int sk = call("sketch.new", Json::Object()).get("buffer_id").as_int(-1);
    CHECK(sk >= 0);
    std::printf("  sketch buffer %d\n", sk);

    // A plate, roughly placed: the rectangle helper already constrains it
    // horizontal/vertical, so only the two dimensions are left.
    Json rect = call("sketch.addRectangle", obj({{"buffer_id", Json(sk)}, {"x0", Json(0.0)}, {"y0", Json(0.0)},
                                                 {"x1", Json(9.3)}, {"y1", Json(5.2)}}));
    CHECK(rect.get("entity_ids").items().size() == 4);

    Json listing = call("sketch.list", obj({{"buffer_id", Json(sk)}}));
    // The origin is point id 0 and fixed; the rectangle's corners follow.
    std::vector<int> corner;
    for (const Json &p : listing.get("points").items()) {
        if (!p.get("fixed").as_bool(false)) corner.push_back(p.get("id").as_int(-1));
    }
    CHECK_CTX(corner.size() == 4, "expected 4 free corners, got " + std::to_string(corner.size()));

    auto ids = [](std::initializer_list<int> v) {
        Json a = Json::Array();
        for (int x : v) a.push_back(Json(x));
        return a;
    };
    // Pin the lower-left corner to the origin, then dimension the sides.
    call("sketch.constrain", obj({{"buffer_id", Json(sk)}, {"kind", Json(std::string("coincident"))},
                                  {"points", ids({0, corner[0]})}}));
    call("sketch.constrain", obj({{"buffer_id", Json(sk)}, {"kind", Json(std::string("horizontal_distance"))},
                                  {"points", ids({corner[0], corner[1]})}, {"value", Json(10.0)}}));
    call("sketch.constrain", obj({{"buffer_id", Json(sk)}, {"kind", Json(std::string("vertical_distance"))},
                                  {"points", ids({corner[1], corner[2]})}, {"value", Json(6.0)}}));

    // Two holes, equal, dimensioned from the corner.
    const int hole_a = call("sketch.addCircle", obj({{"buffer_id", Json(sk)}, {"cx", Json(2.4)},
                                                     {"cy", Json(2.9)}, {"radius", Json(0.8)}}))
                           .get("entity_id").as_int(-1);
    const int hole_b = call("sketch.addCircle", obj({{"buffer_id", Json(sk)}, {"cx", Json(7.7)},
                                                     {"cy", Json(3.1)}, {"radius", Json(1.2)}}))
                           .get("entity_id").as_int(-1);
    listing = call("sketch.list", obj({{"buffer_id", Json(sk)}}));
    int centre_a = -1, centre_b = -1;
    for (const Json &e : listing.get("entities").items()) {
        if (e.get("id").as_int(-1) == hole_a) centre_a = e.get("points").items()[0].as_int(-1);
        if (e.get("id").as_int(-1) == hole_b) centre_b = e.get("points").items()[0].as_int(-1);
    }
    CHECK(centre_a >= 0 && centre_b >= 0);
    call("sketch.constrain", obj({{"buffer_id", Json(sk)}, {"kind", Json(std::string("equal"))},
                                  {"entities", ids({hole_a, hole_b})}}));
    call("sketch.constrain", obj({{"buffer_id", Json(sk)}, {"kind", Json(std::string("radius"))},
                                  {"entities", ids({hole_a})}, {"value", Json(1.0)}}));
    call("sketch.constrain", obj({{"buffer_id", Json(sk)}, {"kind", Json(std::string("horizontal_distance"))},
                                  {"points", ids({corner[0], centre_a})}, {"value", Json(2.5)}}));
    call("sketch.constrain", obj({{"buffer_id", Json(sk)}, {"kind", Json(std::string("vertical_distance"))},
                                  {"points", ids({corner[0], centre_a})}, {"value", Json(3.0)}}));
    call("sketch.constrain", obj({{"buffer_id", Json(sk)}, {"kind", Json(std::string("horizontal_distance"))},
                                  {"points", ids({centre_a, centre_b})}, {"value", Json(5.0)}}));
    call("sketch.constrain", obj({{"buffer_id", Json(sk)}, {"kind", Json(std::string("vertical_distance"))},
                                  {"points", ids({centre_a, centre_b})}, {"value", Json(0.0)}}));

    Json d = call("sketch.solve", obj({{"buffer_id", Json(sk)}}));
    std::printf("  solve: status=%s dof=%d residual=%.3e\n", d.get("status").as_string().c_str(),
                d.get("degrees_of_freedom").as_int(-1), d.get("residual_norm").as_double(0));
    CHECK_CTX(d.get("status").as_string() == "solved", "expected solved, got " + d.dump());
    CHECK(d.get("degrees_of_freedom").as_int(-1) == 0);

    // The geometry is where it was asked to be.
    listing = call("sketch.list", obj({{"buffer_id", Json(sk)}}));
    auto point_of = [&](int id, double *x, double *y) {
        for (const Json &p : listing.get("points").items()) {
            if (p.get("id").as_int(-1) == id) { *x = p.get("x").as_double(0); *y = p.get("y").as_double(0); return true; }
        }
        return false;
    };
    double ax = 0, ay = 0, bx = 0, by = 0;
    CHECK(point_of(centre_a, &ax, &ay) && point_of(centre_b, &bx, &by));
    std::printf("  hole centres (%.6f, %.6f) and (%.6f, %.6f)\n", ax, ay, bx, by);
    CHECK(std::fabs(ax - 2.5) < 1e-6 && std::fabs(ay - 3.0) < 1e-6);
    CHECK(std::fabs(bx - 7.5) < 1e-6 && std::fabs(by - 3.0) < 1e-6);

    Json profiles = call("sketch.profiles", obj({{"buffer_id", Json(sk)}}));
    double plate = 0.0;
    int holes = 0;
    for (const Json &p : profiles.items()) {
        if (p.get("holes").as_int(0) == 2) { plate = p.get("area").as_double(0); holes = 2; }
    }
    const double exact = 60.0 - 2.0 * 3.14159265358979323846;
    std::printf("  plate area %.6f (exactly %.6f), %d holes\n", plate, exact, holes);
    CHECK(holes == 2);
    CHECK(std::fabs(plate - exact) < 5e-3);

    // A parametric change, the way a real edit happens: widen the plate
    // and confirm everything downstream follows.
    int width_constraint = -1;
    for (const Json &c : listing.get("constraints").items()) {
        if (c.get("kind").as_string() == "horizontal_distance" && std::fabs(c.get("value").as_double(0) - 10.0) < 1e-9) {
            width_constraint = c.get("id").as_int(-1);
        }
    }
    CHECK(width_constraint >= 0);
    call("sketch.setConstraintValue", obj({{"buffer_id", Json(sk)}, {"constraint_id", Json(width_constraint)},
                                           {"value", Json(14.0)}}));
    profiles = call("sketch.profiles", obj({{"buffer_id", Json(sk)}}));
    double widened = 0.0;
    for (const Json &p : profiles.items()) {
        if (p.get("holes").as_int(0) == 2) widened = p.get("area").as_double(0);
    }
    const double widened_exact = 14.0 * 6.0 - 2.0 * 3.14159265358979323846;
    std::printf("  after widening to 14: area %.6f (exactly %.6f)\n", widened, widened_exact);
    CHECK(std::fabs(widened - widened_exact) < 5e-3);

    // A conflict is reported as one, not as a failure to converge.
    call("sketch.constrain", obj({{"buffer_id", Json(sk)}, {"kind", Json(std::string("horizontal_distance"))},
                                  {"points", ids({centre_a, centre_b})}, {"value", Json(9.0)}}));
    d = call("sketch.solve", obj({{"buffer_id", Json(sk)}}));
    std::printf("  after a contradictory dimension: status=%s, %zu conflicting\n",
                d.get("status").as_string().c_str(), d.get("conflicting").items().size());
    CHECK_CTX(d.get("status").as_string() == "conflicting", "expected conflicting, got " + d.dump());
    CHECK(!d.get("conflicting").items().empty());

    // A constraint that does not fit its arguments is refused, not ignored.
    Json bad = Call(fd, next_id++, "sketch.constrain",
                    obj({{"buffer_id", Json(sk)}, {"kind", Json(std::string("radius"))},
                         {"entities", ids({corner[0]})}, {"value", Json(1.0)}}), &buf);
    CHECK_CTX(bad.contains("error"), "a radius on a point should be refused: " + bad.dump());
    std::printf("  a radius applied to a point is refused: %s\n",
                bad.get("error").get("message").as_string().c_str());

    Json quit = Call(fd, next_id++, "command.run", obj({{"cmd", Json(std::string("qa!"))}}), &buf);
    CHECK(quit.contains("result"));
    close(fd);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}


void TestKeyboardAndMouse(const char *mep_path) {
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) { execl(mep_path, mep_path, "--no-session", nullptr); _exit(127); }
    const std::string socket_path = MepAgentSocketDir() + "/" + std::to_string(static_cast<long>(pid)) + ".sock";
    int fd = -1;
    for (int i = 0; i < 100 && fd < 0; i++) {
        if (std::filesystem::exists(socket_path)) fd = ConnectOnce(socket_path);
        if (fd < 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK_CTX(fd >= 0, "no socket");
    std::string buf;
    int id = 1;
    auto call = [&](const std::string &m, const Json &p) {
        Json r = Call(fd, id++, m, p, &buf);
        CHECK_CTX(r.contains("result"), m + ": " + r.dump());
        return r.get("result");
    };
    auto obj = [](std::initializer_list<std::pair<std::string, Json>> f) {
        Json j = Json::Object();
        for (const auto &e : f) j[e.first] = e.second;
        return j;
    };
    auto ids = [](const std::vector<int> &v) {
        Json a = Json::Array();
        for (int x : v) a.push_back(Json(x));
        return a;
    };
    auto key = [&](const std::string &k) {
        call("ui.key_press", obj({{"key", Json(k)}}));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    };
    auto click = [&](int px, int py) {
        call("ui.mouse_click", obj({{"x", Json(px)}, {"y", Json(py)}, {"button", Json(std::string("left"))}}));
        std::this_thread::sleep_for(std::chrono::milliseconds(220));
    };

    const int sk = call("sketch.new", Json::Object()).get("buffer_id").as_int(-1);
    std::printf("sketch buffer %d\n", sk);
    std::this_thread::sleep_for(std::chrono::milliseconds(900));

    // --- The mouse: '4' picks the rectangle tool, two clicks draw one ---
    key("4");
    click(700, 400);
    click(1100, 650);
    Json listing = call("sketch.list", obj({{"buffer_id", Json(sk)}}));
    std::printf("mouse drew: %zu points, %zu entities, %zu constraints\n",
                listing.get("points").items().size(), listing.get("entities").items().size(),
                listing.get("constraints").items().size());
    CHECK_CTX(listing.get("entities").items().size() == 4, "the rectangle tool should have made 4 lines");
    CHECK_CTX(listing.get("constraints").items().size() == 4, "and 4 horizontal/vertical constraints");
    std::vector<int> lines;
    for (const Json &e : listing.get("entities").items()) lines.push_back(e.get("id").as_int(-1));

    // The corners came out where the clicks were, in sketch units. The
    // mapping is 48 px per unit about the pane centre, so a 400x250 pixel
    // drag is 8.33 x 5.21 units -- checked to confirm the mouse path
    // really went through the tool rather than landing somewhere by luck.
    double width = 0.0;
    double height = 0.0;
    {
        double lo_x = 1e9, hi_x = -1e9, lo_y = 1e9, hi_y = -1e9;
        for (const Json &p : listing.get("points").items()) {
            if (p.get("fixed").as_bool(false)) continue;  // the origin
            lo_x = std::min(lo_x, p.get("x").as_double(0));
            hi_x = std::max(hi_x, p.get("x").as_double(0));
            lo_y = std::min(lo_y, p.get("y").as_double(0));
            hi_y = std::max(hi_y, p.get("y").as_double(0));
        }
        width = hi_x - lo_x;
        height = hi_y - lo_y;
    }
    std::printf("rectangle is %.3f x %.3f sketch units (400 x 250 px at 48 px/unit)\n", width, height);
    CHECK_CTX(std::fabs(width - 400.0 / 48.0) < 0.3, "width should match the drag");
    CHECK_CTX(std::fabs(height - 250.0 / 48.0) < 0.3, "height should match the drag");

    // --- The keyboard: select two edges, press 'e' ----------------------
    key("1");
    call("sketch.select", obj({{"buffer_id", Json(sk)}, {"entities", ids({lines[0], lines[1]})}}));
    key("e");
    listing = call("sketch.list", obj({{"buffer_id", Json(sk)}}));
    int equals = 0;
    for (const Json &c : listing.get("constraints").items()) {
        if (c.get("kind").as_string() == "equal") ++equals;
    }
    std::printf("'e' on two selected edges -> %d equal constraint(s)\n", equals);
    CHECK_CTX(equals == 1, "the 'e' key should have applied one equal constraint");
    // And the selection was consumed, as applying a constraint should.
    Json sel = call("sketch.selection", obj({{"buffer_id", Json(sk)}}));
    CHECK_CTX(sel.get("entities").items().empty(), "applying a constraint should clear the selection");

    // --- A dimension, through Mode::Prompt -------------------------------
    std::vector<int> corners;
    for (const Json &p : listing.get("points").items()) {
        if (!p.get("fixed").as_bool(false)) corners.push_back(p.get("id").as_int(-1));
    }
    CHECK(corners.size() >= 2);
    call("sketch.select", obj({{"buffer_id", Json(sk)}, {"points", ids({corners[0], corners[1]})}}));
    // 'd' opens Mode::Prompt pre-filled with what the selection currently
    // measures. That the prompt really took the keystrokes is checked by
    // the value that comes back, not by asking for the mode: typing into
    // the sketch pane instead of into a prompt would have applied some
    // other constraint, or none.
    key("d");
    for (int i = 0; i < 12; ++i) key("BackSpace");
    call("ui.type_text", obj({{"text", Json(std::string("7.5"))}}));
    key("Return");
    listing = call("sketch.list", obj({{"buffer_id", Json(sk)}}));
    double dimension = -1.0;
    for (const Json &c : listing.get("constraints").items()) {
        if (c.get("kind").as_string() == "distance") dimension = c.get("value").as_double(0);
    }
    std::printf("typed dimension came back as %g\n", dimension);
    CHECK_CTX(std::fabs(dimension - 7.5) < 1e-9, "the typed dimension should be 7.5");

    Json d = call("sketch.solve", obj({{"buffer_id", Json(sk)}}));
    std::printf("solve: %s, %d dof, residual %.3e\n", d.get("status").as_string().c_str(),
                d.get("degrees_of_freedom").as_int(-1), d.get("residual_norm").as_double(0));
    CHECK(d.get("residual_norm").as_double(1) < 1e-8);
    // The dimension really took effect on the geometry.
    listing = call("sketch.list", obj({{"buffer_id", Json(sk)}}));
    double ax = 0, ay = 0, bx = 0, by = 0;
    for (const Json &p : listing.get("points").items()) {
        if (p.get("id").as_int(-1) == corners[0]) { ax = p.get("x").as_double(0); ay = p.get("y").as_double(0); }
        if (p.get("id").as_int(-1) == corners[1]) { bx = p.get("x").as_double(0); by = p.get("y").as_double(0); }
    }
    const double measured = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
    std::printf("the two dimensioned corners are %.9f apart\n", measured);
    CHECK(std::fabs(measured - 7.5) < 1e-7);

    Call(fd, id++, "command.run", obj({{"cmd", Json(std::string("qa!"))}}), &buf);
    close(fd);
    int status = 0;
    waitpid(pid, &status, 0);
}

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 2) {
        std::fprintf(stderr, "usage: mep-cad-sketch-live-test /path/to/mep\n");
        return 2;
    }
    std::printf("-- over the socket --\n");
    TestOverTheSocket(argv[1]);
    std::printf("-- keyboard and mouse --\n");
    TestKeyboardAndMouse(argv[1]);
    std::printf("cad_sketch_live_test passed\n");
    return 0;
}
