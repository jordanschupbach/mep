#include "model3d_blend_import.h"

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
#define MEP_BLEND_IMPORT_POSIX 1
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

// Bundled Blender Python script: loads the given .blend file and exports it
// as glTF binary. Written to a temp file rather than passed via
// `--python-expr`, since that flag only accepts a single expression and
// this needs a real (multi-statement) script.
constexpr const char *kBlenderExportScript = R"PY(
import bpy
import sys

argv = sys.argv[sys.argv.index("--") + 1:]
in_path, out_path = argv[0], argv[1]

bpy.ops.wm.open_mainfile(filepath=in_path)
bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB')
)PY";

#if MEP_BLEND_IMPORT_POSIX
std::string MakeTempPath(const std::string &suffix) {
    std::string tmpl = "/tmp/mep-model3d-XXXXXX" + suffix;
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = mkstemps(buf.data(), static_cast<int>(suffix.size()));
    if (fd < 0) return "";
    close(fd);
    return std::string(buf.data());
}
#endif

}  // namespace

bool IsBlendPath(const std::string &path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == "blend";
}

bool BlenderAvailable() {
#if MEP_BLEND_IMPORT_POSIX
    const char *path_env = getenv("PATH");
    if (!path_env) return false;
    std::stringstream ss{std::string(path_env)};
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) continue;
        std::string candidate = dir + "/blender";
        if (access(candidate.c_str(), X_OK) == 0) return true;
    }
    return false;
#else
    return false;
#endif
}

bool PrepareBlendConversionJob(const std::string &blend_path, std::vector<std::string> *out_argv,
                                std::string *out_script_path, std::string *out_glb_path, std::string *error) {
#if MEP_BLEND_IMPORT_POSIX
    if (!BlenderAvailable()) {
        if (error) *error = "Blender not found on PATH -- install Blender, or export to .gltf/.obj from Blender first";
        return false;
    }
    std::string script_path = MakeTempPath(".py");
    std::string glb_path = MakeTempPath(".glb");
    if (script_path.empty() || glb_path.empty()) {
        if (error) *error = "could not create a temp file for Blender conversion";
        return false;
    }
    {
        std::ofstream script_out(script_path);
        script_out << kBlenderExportScript;
    }
    *out_argv = {"blender", "--background", "--python", script_path, "--", blend_path, glb_path};
    *out_script_path = script_path;
    *out_glb_path = glb_path;
    return true;
#else
    (void)blend_path;
    (void)out_argv;
    (void)out_script_path;
    (void)out_glb_path;
    if (error) *error = "Blender import is not supported on this build";
    return false;
#endif
}

bool FinishBlendConversionJob(const std::string &script_path, const std::string &glb_path, int exit_code,
                               const std::string &child_output, std::string *error) {
    remove(script_path.c_str());
    std::ifstream check(glb_path, std::ios::binary | std::ios::ate);
    bool glb_written = check.is_open() && check.tellg() > 0;
    check.close();

    if (exit_code != 0 || !glb_written) {
        remove(glb_path.c_str());
        if (error) {
            *error = "Blender conversion failed";
            if (!child_output.empty()) {
                // Blender's own startup banner/addon spam can be long; the
                // actual error is almost always near the end, so keep only
                // the tail.
                size_t tail_start = child_output.size() > 2000 ? child_output.size() - 2000 : 0;
                *error += ": " + child_output.substr(tail_start);
            }
        }
        return false;
    }
    return true;
}

bool ConvertBlendToGltf(const std::string &blend_path, std::string *out_glb_path, std::string *error) {
#if MEP_BLEND_IMPORT_POSIX
    std::vector<std::string> argv_strs;
    std::string script_path, glb_path;
    if (!PrepareBlendConversionJob(blend_path, &argv_strs, &script_path, &glb_path, error)) return false;

    std::vector<char *> argv;
    argv.reserve(argv_strs.size() + 1);
    for (auto &s : argv_strs) argv.push_back(s.data());
    argv.push_back(nullptr);

    int stderr_pipe[2];
    if (pipe(stderr_pipe) != 0) {
        if (error) *error = "pipe() failed";
        remove(script_path.c_str());
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (error) *error = "fork() failed";
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        remove(script_path.c_str());
        return false;
    }
    if (pid == 0) {
        // Child: fold stdout+stderr into the pipe so a conversion failure's
        // message can be surfaced to the caller, then exec Blender. Plain
        // execvp (no shell), so blend_path/script_path/glb_path need no
        // escaping regardless of what characters they contain.
        dup2(stderr_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        execvp("blender", argv.data());
        _exit(127);
    }
    close(stderr_pipe[1]);

    std::string child_output;
    char chunk[4096];
    ssize_t n;
    while ((n = read(stderr_pipe[0], chunk, sizeof(chunk))) > 0) {
        child_output.append(chunk, static_cast<size_t>(n));
    }
    close(stderr_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    int exit_code = (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;

    if (!FinishBlendConversionJob(script_path, glb_path, exit_code, child_output, error)) return false;
    *out_glb_path = glb_path;
    return true;
#else
    (void)blend_path;
    (void)out_glb_path;
    if (error) *error = "Blender import is not supported on this build";
    return false;
#endif
}
