// mep-cad-fem-api-live-test: drives Part K's CAD and FEM surface inside a
// real, running mep -- over the agent socket as an external agent would,
// and through Lua as a script in the editor would.
//
// WHAT THIS COVERS THAT mep-cad-fem-api-test CANNOT. That one exercises
// `cadfem::Session` directly and proves the methods do what they say.
// This one goes through the parts Part K actually added: the prefix
// routing in agent_rpc.cpp, the generated closures in lua_env.cpp, the
// JSON-to-Lua conversion in both directions, and the fact that all of it
// is linked into the editor at all. It is the test that would catch a
// binding generated under the wrong name, a session that is per-call
// instead of per-process, or a surface that works in the test harness
// and is simply absent from the shipped binary.
//
// Usage: mep-cad-fem-api-live-test /path/to/mep

#include "agent_rpc_test_harness.h"

#include "cad_fem_methods.h"

#include <cmath>

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: mep-cad-fem-api-live-test /path/to/mep\n");
        return 2;
    }
    const char *mep_path = argv[1];

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
    auto fails = [&](const std::string &method, const Json &params) {
        Json r = Call(fd, next_id++, method, params, &buf);
        return r.contains("error");
    };
    auto obj = [](std::initializer_list<std::pair<std::string, Json>> fields) {
        Json j = Json::Object();
        for (const auto &f : fields) j[f.first] = f.second;
        return j;
    };
    auto vec = [](double x, double y, double z) {
        Json j = Json::Array();
        j.push_back(x);
        j.push_back(y);
        j.push_back(z);
        return j;
    };

    // --- Over the socket ----------------------------------------------------
    std::printf("part.* and fem.* over the agent socket:\n");
    const int document = call("part.new", obj({{"title", "live"}})).get("document").as_int(-1);
    CHECK(document > 0);
    const Json block =
        call("part.box", obj({{"document", document}, {"size", vec(10, 6, 4)}}));
    const int body = block.get("body").as_int(-1);
    CHECK(body >= 0);

    const Json mass =
        call("part.mass", obj({{"document", document}, {"body", body}, {"density", 7850.0}}));
    std::printf("  a 10x6x4 block weighs %.1f kg and encloses %.6f\n",
                mass.get("mass").as_double(0.0), mass.get("volume").as_double(0.0));
    CHECK(std::fabs(mass.get("volume").as_double(0.0) - 240.0) < 1e-9);

    // THE SESSION SURVIVES BETWEEN CALLS, which is the single thing the
    // socket surface adds over the in-process one and the thing a
    // per-call session would break while every individual call still
    // worked.
    const Json info = call("part.info", obj({{"document", document}}));
    CHECK(info.get("bodies").as_int(0) == 1);
    CHECK(info.get("faces").as_int(0) == 6);

    const int base =
        call("part.face_at", obj({{"document", document}, {"body", body}, {"normal", vec(0, 0, -1)}}))
            .get("face")
            .as_int(-1);
    const int top =
        call("part.face_at", obj({{"document", document}, {"body", body}, {"normal", vec(0, 0, 1)}}))
            .get("face")
            .as_int(-1);
    CHECK(base >= 0 && top >= 0 && base != top);

    const Json mesh =
        call("fem.mesh", obj({{"document", document}, {"body", body}, {"size", 2.0}}));
    const int mesh_id = mesh.get("mesh").as_int(-1);
    std::printf("  meshed to %d nodes and %d elements\n", mesh.get("nodes").as_int(0),
                mesh.get("elements").as_int(0));
    CHECK(mesh_id > 0);

    const int study = call("fem.study", obj({{"document", document}})).get("study").as_int(-1);
    call("fem.support", obj({{"study", study}, {"face", base}}));
    call("fem.load", obj({{"study", study},
                          {"kind", std::string("pressure")},
                          {"face", top},
                          {"magnitude", 5e6}}));
    const Json solved = call("fem.solve", obj({{"study", study}, {"mesh", mesh_id}}));
    std::printf("  solved: %.4e m, %.4e Pa, estimated error %.2f%%\n",
                solved.get("max_displacement").as_double(0.0),
                solved.get("max_von_mises").as_double(0.0),
                100.0 * solved.get("relative_error").as_double(0.0));
    CHECK(solved.get("max_displacement").as_double(0.0) > 0.0);
    CHECK(solved.get("relative_error").as_double(0.0) > 0.0);
    const int result = solved.get("result").as_int(-1);

    const Json field =
        call("fem.field", obj({{"result", result}, {"field", std::string("von_mises")}}));
    CHECK(field.get("max").as_double(0.0) > field.get("min").as_double(0.0));

    // An error comes back as a JSON-RPC error rather than as a result
    // with a flag in it, which is what a client is entitled to expect and
    // is easy to get wrong when forwarding from a boolean-returning
    // dispatch.
    CHECK(fails("fem.field", obj({{"result", 9999}})));
    CHECK(fails("part.box", obj({{"document", document}})));
    CHECK(fails("part.nonexistent", Json::Object()));
    std::printf("  a bad handle, a missing argument and an unknown method all come back"
                " as errors\n");

    // --- EVERY DECLARED METHOD IS ROUTED --------------------------------------
    //
    // The prefix routing in agent_rpc.cpp means a method reaches the
    // dispatch without being listed there, and this is what proves it:
    // each one is called with no parameters and must come back with a
    // *parameter* complaint, never "unknown method". A method that the
    // socket could not see at all would be indistinguishable from one
    // that merely failed, without this.
    int unreachable = 0;
    for (const cadfem::Method &method : cadfem::Methods()) {
        Json r = Call(fd, next_id++, method.name, Json::Object(), &buf);
        const std::string message =
            r.contains("error") ? r.get("error").get("message").as_string("") : "";
        if (message.find("no such method") != std::string::npos) {
            std::printf("    %s is not routed over the socket\n", method.name);
            ++unreachable;
        }
    }
    std::printf("  %d of %d methods unreachable over the socket\n", unreachable,
                static_cast<int>(cadfem::Methods().size()));
    CHECK(unreachable == 0);

    // --- Through Lua -----------------------------------------------------------
    std::printf("mep.part_* and mep.fem_* from Lua, in the same editor:\n");
    const int scratch = call("buffer.create", obj({{"name", std::string("cadfem")}}))
                            .get("buffer_id")
                            .as_int(-1);
    CHECK(scratch >= 0);
    auto lua = [&](const std::string &chunk) {
        call("command.run", obj({{"cmd", "lua " + chunk}}));
        const Json lines = call("buffer.getLines", obj({{"buffer_id", scratch}}));
        std::string joined;
        for (const Json &line : lines.get("lines").items()) joined += line.as_string("");
        return joined;
    };
    auto report = [&](const std::string &expression) {
        return "local ok, err = " + expression +
               " mep.buffer_set_lines(" + std::to_string(scratch) +
               ", {ok and mep_ai_json_encode(ok) or ('ERR ' .. tostring(err))})";
    };

    // A DIFFERENT DOCUMENT IN THE SAME PROCESS. Lua gets its own session
    // rather than sharing the socket's, which is deliberate -- a script
    // in the editor and an agent on the socket should not tread on each
    // other's handles -- and this is where that would show up as a
    // document number that unexpectedly already exists.
    std::string out = lua(report("mep.part_new{title='from lua'}"));
    std::printf("  mep.part_new -> %s\n", out.c_str());
    CHECK(out.find("document") != std::string::npos);
    CHECK(out.find("ERR") == std::string::npos);

    out = lua(report("mep.part_box{document=1, size={2,3,4}}"));
    std::printf("  mep.part_box -> %s\n", out.c_str());
    CHECK(out.find("body") != std::string::npos);

    out = lua(report("mep.part_mass{document=1, body=0}"));
    std::printf("  mep.part_mass -> %s\n", out.c_str());
    CHECK(out.find("volume") != std::string::npos);
    // 2 x 3 x 4, computed in the editor's own Lua and read back here.
    CHECK(out.find("24") != std::string::npos);

    // NIL PLUS A MESSAGE ON FAILURE, this file's convention for an
    // operation that can legitimately fail on its input -- not a raised
    // error, which would need every caller to wrap the call in pcall.
    out = lua(report("mep.part_box{document=99}"));
    std::printf("  a bad handle from Lua -> %s\n", out.c_str());
    CHECK(out.find("ERR") == 0);

    // And every method is bound under the name the documentation states.
    std::string missing;
    for (const cadfem::Method &method : cadfem::Methods()) {
        std::string name = method.name;
        for (char &ch : name) {
            if (ch == '.') ch = '_';
        }
        const std::string chunk = "mep.buffer_set_lines(" + std::to_string(scratch) +
                                  ", {type(mep." + name + ")})";
        if (lua(chunk) != "function") missing += " " + name;
    }
    std::printf("  every method bound in Lua:%s\n", missing.empty() ? " yes" : missing.c_str());
    CHECK(missing.empty());

    // --- And headless, with no editor at all (Part K.5) -----------------------
    //
    // The same methods again, from the command line. Run here rather than
    // in the windowless test because it needs the built `mep` binary,
    // which only a live test is given the path to -- and because the
    // point of it is that the *shipped* binary does this, not that a
    // library call works.
    std::printf("the same pipeline headless, through `mep --cad-fem`:\n");
    const std::string dir = "/tmp/mep-cad-fem-cli-" + std::to_string(static_cast<long>(getpid()));
    std::filesystem::create_directories(dir);
    const std::string script_path = dir + "/study.json";
    {
        std::ofstream script(script_path);
        script << R"([
  {"method": "part.new", "as": "doc"},
  {"method": "part.box", "as": "block", "params": {"document": "$doc.document", "size": [10, 6, 4]}},
  {"method": "part.face_at", "as": "base", "params": {"document": "$doc.document", "body": "$block.body", "normal": [0, 0, -1]}},
  {"method": "part.face_at", "as": "top", "params": {"document": "$doc.document", "body": "$block.body", "normal": [0, 0, 1]}},
  {"method": "fem.mesh", "as": "mesh", "params": {"document": "$doc.document", "body": "$block.body", "size": 2.0}},
  {"method": "fem.study", "as": "study", "params": {"document": "$doc.document"}},
  {"method": "fem.support", "params": {"study": "$study.study", "face": "$base.face"}},
  {"method": "fem.load", "params": {"study": "$study.study", "kind": "pressure", "face": "$top.face", "magnitude": 5000000}},
  {"method": "fem.solve", "as": "result", "params": {"study": "$study.study", "mesh": "$mesh.mesh"}}
])";
    }
    const std::string out_path = dir + "/out.json";
    const std::string command = std::string(mep_path) + " --cad-fem " + script_path + " " +
                                out_path + " >/dev/null 2>&1";
    CHECK_CTX(std::system(command.c_str()) == 0, "mep --cad-fem exited non-zero");
    std::string produced;
    {
        std::ifstream file(out_path);
        produced.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }
    Json summary;
    CHECK(Json::Parse(produced, &summary));
    CHECK(summary.get("ok").as_bool(false));
    CHECK(summary.get("calls").as_int(0) == 9);
    const Json &last = summary.get("results").items().back().get("result");
    std::printf("  9 calls with no display: %.4e m, %.4e Pa, error %.2f%%\n",
                last.get("max_displacement").as_double(0.0),
                last.get("max_von_mises").as_double(0.0),
                100.0 * last.get("relative_error").as_double(0.0));
    // THE SAME ANSWER AS OVER THE SOCKET, to the last digit: the same
    // methods on the same geometry with no editor in the picture. A
    // headless path that quietly used different defaults would still look
    // plausible on its own.
    CHECK(std::fabs(last.get("max_displacement").as_double(0.0) -
                    solved.get("max_displacement").as_double(0.0)) < 1e-15);
    CHECK(std::fabs(last.get("max_von_mises").as_double(0.0) -
                    solved.get("max_von_mises").as_double(0.0)) < 1e-6);

    // A script that fails says which call and why, and still writes what
    // it got -- a CI job needs the partial results to diagnose from.
    const std::string bad_path = dir + "/bad.json";
    {
        std::ofstream script(bad_path);
        script << R"([{"method": "part.new"}, {"method": "part.box", "params": {"document": 1}}])";
    }
    const std::string bad = std::string(mep_path) + " --cad-fem " + bad_path + " " + dir +
                            "/bad-out.json >/dev/null 2>&1";
    CHECK(std::system(bad.c_str()) != 0);
    {
        std::ifstream file(dir + "/bad-out.json");
        produced.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }
    CHECK(Json::Parse(produced, &summary));
    CHECK(!summary.get("ok").as_bool(true));
    CHECK(summary.get("failed").as_int(-1) == 1);
    CHECK(summary.get("results").size() == 1);
    std::printf("  a bad script fails at call %d (%s) and keeps the %d results before it\n",
                summary.get("failed").as_int(-1), summary.get("method").as_string("").c_str(),
                static_cast<int>(summary.get("results").size()));
    std::filesystem::remove_all(dir);

    Call(fd, next_id++, "command.run", obj({{"cmd", std::string("qa!")}}), &buf);
    int status = 0;
    for (int i = 0; i < 100; i++) {
        if (waitpid(pid, &status, WNOHANG) == pid) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    close(fd);
    std::printf("cad_fem_api_live_test passed\n");
    return 0;
}
