// mep-bench: the headless benchmark binary for mep's own libraries
// (plans/PERFORMANCE_PLAN.md). Every benchmark lives in a src/bench/*.cpp
// file and registers itself with MEP_BENCH; this file only runs them.
// `mep-bench --list` names them, `--filter` picks some; tools/bench.py
// runs this alongside the older per-subsystem bench binaries as one suite.

#include "bench_util.h"

#include "gfx/backend.h"
#include "gfx/backend_native.h"

int main(int argc, char **argv) {
    // Installed but never opened: a windowless Editor still reaches the
    // gfx facade (fonts, clipboard), exactly as in mep-editor-fold-test.
    gfx::SetBackends(gfx::ToBackends(gfx::CreateNativeBackendSet()));
    mep::bench::Session session(argc, argv, "mep_bench");
    return session.Run();
}
