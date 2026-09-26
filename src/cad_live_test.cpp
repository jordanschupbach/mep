// mep-cad-live-test: builds a CAD part inside a real, running mep by
// driving its agent socket, exactly as an external agent would
// (plans/CAD_FEM_PLAN.md Part F.5).
//
// What this covers that the windowless tests cannot. Those exercise the
// kernel directly and prove the geometry is right. This one goes through
// the whole stack the pane is built on -- JSON-RPC over the socket, the
// dispatch in agent_rpc.cpp, the Editor::Cad* methods, the feature tree,
// the exporters, and back out as JSON -- and so is the test that would
// catch an argument transposed in a binding, a session not rebuilt after
// a change, or a failure reported as a success. Mode::Cad's keys call
// those same Editor methods, which is why testing this surface tests
// both.
//
// Usage: mep-cad-live-test /path/to/mep

#include "agent_rpc_test_harness.h"

#include <cmath>

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: mep-cad-live-test /path/to/mep\n");
        return 2;
    }
    const char *mep_path = argv[1];
    const std::string out_dir = "/tmp/mep-cad-live-" + std::to_string(static_cast<long>(getpid()));
    std::filesystem::create_directories(out_dir);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        execl(mep_path, mep_path, "--no-session", nullptr);
        _exit(127);
    }

    const std::string socket_path =
        MepAgentSocketDir() + "/" + std::to_string(static_cast<long>(pid)) + ".sock";
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

    // --- A sketch, then a part built on it -------------------------------
    const int sketch = call("sketch.new", Json::Object()).get("buffer_id").as_int(-1);
    CHECK(sketch >= 0);
    auto line = [&](double x0, double y0, double x1, double y1) {
        call("sketch.addLine", obj({{"buffer_id", Json(sketch)},
                                    {"x0", Json(x0)},
                                    {"y0", Json(y0)},
                                    {"x1", Json(x1)},
                                    {"y1", Json(y1)}}));
    };
    line(0.0, 0.0, 10.0, 0.0);
    line(10.0, 0.0, 10.0, 6.0);
    line(10.0, 6.0, 0.0, 6.0);
    line(0.0, 6.0, 0.0, 0.0);

    const int part = call("cad.new", Json::Object()).get("buffer_id").as_int(-1);
    CHECK(part >= 0);
    const int sketch_feature =
        call("cad.addSketch", obj({{"buffer_id", Json(part)},
                                   {"sketch_buffer_id", Json(sketch)},
                                   {"name", Json(std::string("outline"))}}))
            .get("feature")
            .as_int(-1);
    CHECK(sketch_feature >= 0);
    const int extrude = call("cad.addExtrude", obj({{"buffer_id", Json(part)},
                                                    {"sketch", Json(sketch_feature)},
                                                    {"distance", Json(3.0)},
                                                    {"name", Json(std::string("plate"))}}))
                            .get("feature")
                            .as_int(-1);
    CHECK(extrude >= 0);

    Json info = call("cad.info", obj({{"buffer_id", Json(part)}}));
    std::printf("after the extrude: %s\n", info.get("summary").as_string("").c_str());
    CHECK_CTX(std::fabs(info.get("volume").as_double(0.0) - 180.0) < 1e-6,
              "a 10 x 6 plate 3 thick should be 180, not " +
                  std::to_string(info.get("volume").as_double(0.0)));
    CHECK(info.get("triangles").as_int(0) == 12);
    CHECK(info.get("tree").size() == 2);
    CHECK(!info.get("imported").as_bool(true));

    // --- Change a dimension, which is the whole point of a feature tree ---
    call("cad.set", obj({{"buffer_id", Json(part)},
                         {"feature", Json(extrude)},
                         {"field", Json(std::string("distance"))},
                         {"value", Json(7.0)}}));
    info = call("cad.info", obj({{"buffer_id", Json(part)}}));
    std::printf("after thickening to 7: %s\n", info.get("summary").as_string("").c_str());
    CHECK_CTX(std::fabs(info.get("volume").as_double(0.0) - 420.0) < 1e-6,
              "thickening to 7 should give 420, not " +
                  std::to_string(info.get("volume").as_double(0.0)));

    // --- Suppressing it takes the body away and unsuppressing brings it back
    call("cad.suppress",
         obj({{"buffer_id", Json(part)}, {"feature", Json(extrude)}, {"suppressed", Json(true)}}));
    info = call("cad.info", obj({{"buffer_id", Json(part)}}));
    CHECK(info.get("triangles").as_int(-1) == 0);
    CHECK(info.get("tree").items()[1].get("suppressed").as_bool(false));
    call("cad.suppress",
         obj({{"buffer_id", Json(part)}, {"feature", Json(extrude)}, {"suppressed", Json(false)}}));
    info = call("cad.info", obj({{"buffer_id", Json(part)}}));
    CHECK(info.get("triangles").as_int(0) == 12);
    std::printf("suppressed and restored: %s\n", info.get("summary").as_string("").c_str());

    // --- Every export, and each one has to be readable ---------------------
    for (const char *format : {"mepcad", "step", "stl", "obj", "gltf", "svg"}) {
        const std::string path = out_dir + "/part." + format;
        const Json result = call("cad.export", obj({{"buffer_id", Json(part)},
                                                    {"format", Json(std::string(format))},
                                                    {"path", Json(path)}}));
        CHECK_CTX(result.get("ok").as_bool(false), std::string("export ") + format + " failed");
        CHECK_CTX(std::filesystem::exists(path), std::string("export ") + format + " wrote nothing");
        CHECK_CTX(std::filesystem::file_size(path) > 40,
                  std::string("export ") + format + " wrote almost nothing");
        std::printf("  %-7s %zu bytes\n", format, std::filesystem::file_size(path));
    }
    CHECK(!call("cad.export", obj({{"buffer_id", Json(part)},
                                   {"format", Json(std::string("nonsense"))},
                                   {"path", Json(out_dir + "/x")}}))
               .get("ok")
               .as_bool(true));

    // --- Reopen the .mepcad: the tree comes back, parametric ---------------
    const Json reopened =
        call("cad.open", obj({{"path", Json(out_dir + "/part.mepcad")}}));
    CHECK_CTX(reopened.get("ok").as_bool(false), "reopening the .mepcad failed");
    const int reopened_buffer = reopened.get("buffer_id").as_int(-1);
    CHECK(reopened_buffer >= 0 && reopened_buffer != part);
    Json again = call("cad.info", obj({{"buffer_id", Json(reopened_buffer)}}));
    std::printf("reopened: %s\n", again.get("summary").as_string("").c_str());
    CHECK(!again.get("imported").as_bool(true));
    CHECK(again.get("tree").size() == 2);
    CHECK_CTX(std::fabs(again.get("volume").as_double(0.0) - 420.0) < 1e-6,
              "the reopened part should still be 420");
    // ...and it is still a tree, not a lump: change the dimension again.
    int reopened_extrude = -1;
    for (const Json &feature : again.get("tree").items()) {
        if (feature.get("name").as_string("") == "plate") reopened_extrude = feature.get("id").as_int(-1);
    }
    CHECK(reopened_extrude >= 0);
    call("cad.set", obj({{"buffer_id", Json(reopened_buffer)},
                         {"feature", Json(reopened_extrude)},
                         {"field", Json(std::string("distance"))},
                         {"value", Json(1.5)}}));
    again = call("cad.info", obj({{"buffer_id", Json(reopened_buffer)}}));
    CHECK_CTX(std::fabs(again.get("volume").as_double(0.0) - 90.0) < 1e-6,
              "the reopened part is not parametric: changing its distance did nothing");
    std::printf("the reopened part is still parametric: %s\n",
                again.get("summary").as_string("").c_str());

    // --- Reopen the STEP: geometry, and it says so -------------------------
    const Json imported = call("cad.open", obj({{"path", Json(out_dir + "/part.step")}}));
    CHECK_CTX(imported.get("ok").as_bool(false), "reopening the .step failed");
    const Json imported_info =
        call("cad.info", obj({{"buffer_id", Json(imported.get("buffer_id").as_int(-1))}}));
    std::printf("reopened the STEP: %s\n", imported_info.get("summary").as_string("").c_str());
    CHECK(imported_info.get("imported").as_bool(false));
    CHECK(imported_info.get("tree").size() == 0);
    CHECK_CTX(std::fabs(imported_info.get("volume").as_double(0.0) - 420.0) < 1e-6,
              "the STEP should carry the 420 the part was when it was written");

    // --- The camera and the view are drivable too --------------------------
    call("cad.camera", obj({{"buffer_id", Json(part)}, {"yaw", Json(30.0)}, {"pitch", Json(10.0)}}));
    call("cad.camera", obj({{"buffer_id", Json(part)}, {"zoom", Json(1.5)}}));
    call("cad.camera", obj({{"buffer_id", Json(part)}, {"frame_all", Json(true)}}));
    call("cad.view", obj({{"buffer_id", Json(part)}, {"view", Json(std::string("wireframe"))}}));

    // --- Removing the extrude leaves the sketch ----------------------------
    CHECK(call("cad.remove", obj({{"buffer_id", Json(part)}, {"feature", Json(extrude)}}))
              .get("ok")
              .as_bool(false));
    info = call("cad.info", obj({{"buffer_id", Json(part)}}));
    CHECK(info.get("tree").size() == 1);
    CHECK(info.get("triangles").as_int(-1) == 0);

    Call(fd, next_id++, "command.run", obj({{"cmd", Json(std::string("qa!"))}}), &buf);
    int status = 0;
    for (int i = 0; i < 100; i++) {
        if (waitpid(pid, &status, WNOHANG) == pid) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    close(fd);
    std::filesystem::remove_all(out_dir);
    std::printf("cad_live_test passed\n");
    return 0;
}
