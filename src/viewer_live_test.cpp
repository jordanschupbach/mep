// mep-viewer-live-test: the viewer, driven inside a real running mep
// (plans/CAD_FEM_PLAN.md Part L).
//
// WHAT THIS COVERS THAT A WINDOWLESS TEST CANNOT. view_scene_test proves
// the scene is built right; this one proves the *pane* is wired to it --
// the script runs, its callback is called when the time moves, the
// camera frames what the script did not pin, and the whole thing goes
// through the same agent-RPC surface an agent would use. It is the test
// that would catch a callback registered against the wrong buffer, a
// time that never reaches the scene, or a camera that a script's partial
// specification silently blanks.
//
// Usage: mep-viewer-live-test /path/to/mep [scratch directory]

#include "agent_rpc_test_harness.h"

#include <cmath>

namespace {

// A script that needs no kernel at all: a triangle whose height is the
// time. Deliberately not a solve -- this test is about the wiring, and a
// mesher or a solver failing here would look like a viewer bug.
const char *kScript =
    "view.time_range(0, 2)\n"
    "view.on_frame(function(t)\n"
    "  local h = 1.0 + t\n"
    "  view.mesh{positions = {0,0,0,  2,0,0,  1,0,h}, indices = {1,2,3},\n"
    "            color = {200, 120, 80, 255}, name = 'wedge'}\n"
    "  view.label({1, 0, h}, 'TIP')\n"
    "  view.point({0, 0, 0}, 0.2)\n"
    "end)\n";

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: mep-viewer-live-test /path/to/mep [scratch dir]\n");
        return 2;
    }
    const char *mep_path = argv[1];
    const std::string out_dir =
        argc > 2 ? std::string(argv[2])
                 : "/tmp/mep-viewer-live-" + std::to_string(static_cast<long>(getpid()));
    std::filesystem::create_directories(out_dir);
    const std::string script_path = out_dir + "/scene.lua";
    {
        std::ofstream out(script_path, std::ios::binary);
        CHECK(static_cast<bool>(out));
        out << kScript;
    }

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

    // --- A viewer, with the script bound -----------------------------------
    const Json opened = call("view.new", obj({{"script", Json(script_path)}}));
    const int buffer_id = opened.get("buffer_id").as_int(-1);
    CHECK_CTX(buffer_id >= 0, "view.new gave no buffer");
    CHECK_CTX(opened.get("ran").as_bool(false), "the script did not run");
    std::printf("viewer in buffer %d: %s\n", buffer_id,
                opened.get("summary").as_string("").c_str());

    Json info = call("view.info", obj({{"buffer_id", Json(buffer_id)}}));
    CHECK_CTX(info.get("message").as_string("").empty(), info.get("message").as_string(""));
    // The callback ran once on load, so there is a scene before anyone
    // has touched the slider. Two items -- the triangle and the cross
    // marking the origin -- and one label, which is not an item: a label
    // is text pinned to a point, with no geometry of its own.
    CHECK(info.get("items").as_int(0) == 2);
    CHECK(info.get("triangles").as_int(0) == 1);
    CHECK(info.get("labels").as_int(0) == 1);
    CHECK(info.get("animated").as_bool(false));
    CHECK(std::fabs(info.get("to").as_double(0.0) - 2.0) < 1e-9);
    std::printf("  on load: %d items, %d triangles, %d labels, t in [%g, %g]\n",
                info.get("items").as_int(0), info.get("triangles").as_int(0),
                info.get("labels").as_int(0), info.get("from").as_double(0.0),
                info.get("to").as_double(0.0));

    // THE CAMERA FRAMED WHAT THE SCRIPT BUILT. The script names no
    // camera at all, so the distance must have come from the scene --
    // not from the ten-unit default a session starts with.
    const double distance = info.get("distance").as_double(0.0);
    std::printf("  the camera fitted itself to a distance of %.4f\n", distance);
    CHECK(distance > 0.5 && distance < 9.0);

    // --- The time reaches the scene ----------------------------------------
    //
    // The triangle's apex is at z = 1 + t, so the scene's own height is
    // a direct readout of whether the callback ran with the time it was
    // given. Nothing else in this test could make it change.
    auto tip_height = [&](double at) {
        call("view.time", obj({{"buffer_id", Json(buffer_id)}, {"time", Json(at)}}));
        const Json state = call("view.info", obj({{"buffer_id", Json(buffer_id)}}));
        CHECK_CTX(state.get("message").as_string("").empty(),
                  state.get("message").as_string(""));
        return state;
    };
    const Json at_zero = tip_height(0.0);
    const Json at_two = tip_height(2.0);
    CHECK(std::fabs(at_zero.get("time").as_double(-1.0)) < 1e-9);
    CHECK(std::fabs(at_two.get("time").as_double(-1.0) - 2.0) < 1e-9);
    // Rebuilt each time, so the counts hold rather than accumulating --
    // which is the contract that lets on_frame be written as "what is
    // here now".
    CHECK(at_two.get("items").as_int(0) == 2);
    CHECK(at_two.get("labels").as_int(0) == 1);
    std::printf("  scrubbed to t = 0 and t = 2; the scene is rebuilt, not appended to\n");

    // Out of range is clamped rather than refused: a slider cannot go
    // past its own ends and neither should the method behind it.
    const Json clamped = tip_height(99.0);
    CHECK(std::fabs(clamped.get("time").as_double(-1.0) - 2.0) < 1e-9);
    const Json clamped_low = tip_height(-99.0);
    CHECK(std::fabs(clamped_low.get("time").as_double(-1.0)) < 1e-9);
    std::printf("  t = 99 clamps to 2 and t = -99 to 0\n");

    // --- Playing -------------------------------------------------------------
    call("view.time", obj({{"buffer_id", Json(buffer_id)}, {"playing", Json(true)}}));
    Json playing = call("view.info", obj({{"buffer_id", Json(buffer_id)}}));
    CHECK(playing.get("playing").as_bool(false));
    // It really advances, which is the draw loop's tick doing its job.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const Json later = call("view.info", obj({{"buffer_id", Json(buffer_id)}}));
    std::printf("  playing: t went from 0 to %.4f in 400 ms\n",
                later.get("time").as_double(0.0));
    CHECK(later.get("time").as_double(0.0) > 0.0);
    call("view.time", obj({{"buffer_id", Json(buffer_id)}, {"playing", Json(false)}}));

    // --- The camera moves through the same methods the mouse uses -----------
    const Json before = call("view.info", obj({{"buffer_id", Json(buffer_id)}}));
    call("view.camera", obj({{"buffer_id", Json(buffer_id)}, {"yaw", Json(30.0)},
                             {"zoom", Json(2.0)}}));
    const Json after = call("view.info", obj({{"buffer_id", Json(buffer_id)}}));
    CHECK(std::fabs(after.get("yaw").as_double(0.0) - before.get("yaw").as_double(0.0) - 30.0) <
          1e-6);
    CHECK(after.get("distance").as_double(0.0) < before.get("distance").as_double(0.0) * 0.6);
    std::printf("  yaw %+.1f -> %+.1f, distance %.3f -> %.3f\n", before.get("yaw").as_double(0.0),
                after.get("yaw").as_double(0.0), before.get("distance").as_double(0.0),
                after.get("distance").as_double(0.0));

    // --- A script with an error says so rather than going quiet -------------
    {
        std::ofstream out(script_path, std::ios::binary);
        out << "view.time_range(0, 1)\n"
               "view.on_frame(function(t) error('deliberate') end)\n";
    }
    call("view.run", obj({{"buffer_id", Json(buffer_id)}}));
    const Json broken = call("view.info", obj({{"buffer_id", Json(buffer_id)}}));
    std::printf("  a script that raises: %s\n", broken.get("message").as_string("").c_str());
    CHECK(!broken.get("message").as_string("").empty());
    CHECK(broken.get("message").as_string("").find("deliberate") != std::string::npos);

    // --- Saving the script rebuilds the viewer -----------------------------
    //
    // THE LIVE-CODING LOOP, and the one regression worth guarding here.
    // The rebuild reads the file back, and the first version of it ran
    // while the editor's output stream was still open -- so it read
    // whatever had reached the disk, usually nothing. An empty Lua chunk
    // loads and runs perfectly happily, so the viewer reported no error:
    // it went blank, lost its timeline, and came back the moment
    // anything ran the script again. Nothing but an end-to-end save
    // would have caught it.
    {
        // `file.open` reports nothing, so the buffer is found by its
        // name afterwards -- which is also the check that it opened.
        call("file.open", obj({{"path", Json(script_path)}}));
        int script_buffer = -1;
        const Json listed = call("buffer.list", Json::Object());
        for (const Json &entry : listed.items()) {
            if (entry.get("filename").as_string("") == script_path) {
                script_buffer = entry.get("id").as_int(-1);
            }
        }
        CHECK_CTX(script_buffer >= 0, "the script did not open in a buffer: " + listed.dump());
        Json lines = Json::Array();
        lines.push_back(Json(std::string("view.time_range(0, 5)")));
        lines.push_back(Json(std::string("view.caption('EDITED')")));
        lines.push_back(Json(std::string("view.on_frame(function(t)")));
        lines.push_back(Json(std::string(
            "  view.mesh{positions = {0,0,0, 1,0,0, 0,1,0, 0,0,1}, indices = {1,2,3, 1,2,4}}")));
        lines.push_back(Json(std::string("  view.label({0,0,1}, 'AFTER')")));
        lines.push_back(Json(std::string("end)")));
        call("buffer.setLines", obj({{"buffer_id", Json(script_buffer)}, {"lines", lines}}));
        call("command.run", obj({{"cmd", Json(std::string("w"))}}));

        const Json rebuilt = call("view.info", obj({{"buffer_id", Json(buffer_id)}}));
        std::printf("  after saving the script: %s\n",
                    rebuilt.get("summary").as_string("").c_str());
        CHECK_CTX(rebuilt.get("message").as_string("").empty(),
                  rebuilt.get("message").as_string(""));
        // The new script's scene, not the old one's and not an empty
        // one: one item of two triangles, its own label, its own caption
        // and its own time range.
        CHECK(rebuilt.get("animated").as_bool(false));
        CHECK(rebuilt.get("items").as_int(-1) == 1);
        CHECK(rebuilt.get("triangles").as_int(-1) == 2);
        CHECK(rebuilt.get("labels").as_int(-1) == 1);
        CHECK(rebuilt.get("caption").as_string("") == "EDITED");
        CHECK(std::fabs(rebuilt.get("to").as_double(0.0) - 5.0) < 1e-9);
    }

    call("command.run", obj({{"cmd", Json(std::string("qa!"))}}));
    close(fd);
    int status = 0;
    waitpid(pid, &status, 0);
    std::printf("viewer_live_test passed\n");
    std::filesystem::remove_all(out_dir);
    return 0;
}
