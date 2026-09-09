// Stage B / B1 walking-skeleton smoke test: opens a real GLFW+OpenGL 3.3
// core window, loads GL function pointers via gfx::gl::LoadGLFunctions,
// clears to a distinct color for a fixed number of frames, then closes
// cleanly. Deliberately standalone -- not wired into gfx::Backends or
// main.cpp -- so the windowing/GL-context foundation can be validated on
// its own before the rest of the native backend (input/text/2D/3D) is
// built on top of it. See PLAN "Stage B" step B1.
//
// Exits 0 on success (GL functions loaded, ran the full frame count
// without a GL error), non-zero otherwise. Run with MEP_SMOKE_FRAMES=N
// in the environment to override the default frame count (useful for a
// human to eyeball the window instead of it closing immediately).

// Without this, glfw3.h includes the platform's own <GL/gl.h>, whose
// GL_* macros textually collide with (and mangle) the same-named
// constexpr constants gfx/gl_loader.h declares for its own hand-rolled,
// loader-agnostic GL binding.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstdlib>

#include "gfx/gl_loader.h"

int main() {
    if (glfwInit() == GLFW_FALSE) {
        std::fprintf(stderr, "smoke: glfwInit failed\n");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    GLFWwindow *window = glfwCreateWindow(640, 480, "mep gfx native smoke test", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "smoke: glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    if (!gfx::gl::LoadGLFunctions(reinterpret_cast<gfx::gl::GLProcAddressFn>(&glfwGetProcAddress))) {
        std::fprintf(stderr, "smoke: LoadGLFunctions failed\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    const unsigned char *vendor = gfx::gl::GetString(gfx::gl::GL_VENDOR);
    const unsigned char *renderer = gfx::gl::GetString(gfx::gl::GL_RENDERER);
    const unsigned char *version = gfx::gl::GetString(gfx::gl::GL_VERSION);
    std::printf("smoke: GL_VENDOR=%s\n", vendor != nullptr ? reinterpret_cast<const char *>(vendor) : "?");
    std::printf("smoke: GL_RENDERER=%s\n", renderer != nullptr ? reinterpret_cast<const char *>(renderer) : "?");
    std::printf("smoke: GL_VERSION=%s\n", version != nullptr ? reinterpret_cast<const char *>(version) : "?");

    int frame_count = 120;
    if (const char *env = std::getenv("MEP_SMOKE_FRAMES"); env != nullptr) {
        frame_count = std::atoi(env);
    }

    for (int frame = 0; frame < frame_count && glfwWindowShouldClose(window) == GLFW_FALSE; frame++) {
        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        gfx::gl::Viewport(0, 0, width, height);
        // A slowly-shifting clear color, so a human watching the window
        // can see it's actually alive frame to frame, not a static image.
        float t = static_cast<float>(frame) / static_cast<float>(frame_count);
        gfx::gl::ClearColor(0.1f + 0.2f * t, 0.15f, 0.35f - 0.2f * t, 1.0f);
        gfx::gl::Clear(gfx::gl::GL_COLOR_BUFFER_BIT);

        gfx::gl::GLenum err = gfx::gl::GetError();
        if (err != gfx::gl::GL_NO_ERROR) {
            std::fprintf(stderr, "smoke: GL error 0x%04x at frame %d\n", err, frame);
            glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    std::printf("smoke: ran %d frames cleanly, closing\n", frame_count);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
