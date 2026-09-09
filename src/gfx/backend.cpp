#include "gfx/backend.h"

#include <cstdio>
#include <cstdlib>

namespace gfx {

namespace {
Backends g_backends;
bool g_backends_set = false;
}  // namespace

void SetBackends(const Backends &backends) {
    g_backends = backends;
    g_backends_set = true;
}

const Backends &GetBackends() {
    // A plain assert() here was tried first and stripped by NDEBUG in the
    // Release build (mep's default CMAKE_BUILD_TYPE) -- every gfx:: call
    // then silently dereferenced a null backend pointer and segfaulted
    // with no diagnostic. This check runs in every build configuration on
    // purpose: it should never fire (main() installs a backend before
    // anything else), but if it ever does, a clear message beats a
    // silent crash.
    if (!g_backends_set) {
        std::fputs("gfx::GetBackends() called before gfx::SetBackends() -- no backend installed\n", stderr);
        std::abort();
    }
    return g_backends;
}

}  // namespace gfx
