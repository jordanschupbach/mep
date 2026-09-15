// Demonstrates the per-frame cost of mep's mep.on_frame hook mechanism
// (LuaEnv::RunFrameHooks, lua_env.cpp) at increasing hook counts -- the
// shape of the bug fixed alongside this benchmark: every mep.on_frame(fn)
// call used to register a permanent per-frame Lua call with no way to
// ever unregister it, so a repeatable user action (starting an AI agent
// from the Todo panel, kBuiltinActivityBar's mep.activity_todo_start_agent)
// silently left two more closures being invoked on literally every frame
// for the rest of the process, once their one-shot job was done. Over a
// long session of repeatedly using that feature, this is exactly the
// kind of cumulative, easy-to-miss slowdown that shows up as "the editor
// feels clunkier than it used to" rather than any single obviously-slow
// operation.
//
// Mirrors LuaEnv::RunFrameHooks's exact call shape (lua_rawgeti +
// lua_pcall(0, 1, 0), a truthy return value drops the hook) directly
// against a bare lua_State rather than the real LuaEnv/Editor: linking
// editor.cpp here would pull in the whole gfx-backed application for no
// benefit, since what's actually being measured is the Lua call boundary
// itself (the cost driver), not any editor-specific behavior.

#include "bench_util.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cstdio>
#include <string>
#include <vector>

using mep::bench::RecordResult;
using mep::bench::Timer;

namespace {

constexpr const char *kHistoryPath = "bench_results/history.jsonl";
constexpr const char *kBinary = "lua_frame_hook_bench";

// One frame's worth of hook dispatch -- see LuaEnv::RunFrameHooks's own
// comment (lua_env.cpp) for why this is index-based rather than a
// range-for: a hook re-entrantly registering a new one (mep.on_frame
// chaining, the real leak's own shape) push_back's into `refs` mid-loop,
// which a cached iterator/pointer wouldn't survive.
void RunFrameHooksOnce(lua_State *L, std::vector<int> &refs) {
    size_t count = refs.size();
    size_t write = 0;
    for (size_t i = 0; i < count; i++) {
        int ref = refs[i];
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        lua_pcall(L, 0, 1, 0);
        bool done = lua_toboolean(L, -1);
        lua_pop(L, 1);
        if (done) {
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
        } else {
            refs[write++] = ref;
        }
    }
    for (size_t i = count; i < refs.size(); i++) refs[write++] = refs[i];
    refs.resize(write);
}

int RegisterHook(lua_State *L, const char *src) {
    luaL_loadstring(L, src);
    lua_pcall(L, 0, 1, 0);  // the chunk itself returns the closure
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

/**
 * @brief Steady-state per-frame cost of N permanently-registered no-op hooks -- the pre-fix shape, where
 * a hook whose one-shot condition had already fired kept being called every single frame forever (its own
 * body short-circuiting immediately, but the Lua call itself is not free). Run for 200 simulated frames
 * and record the total.
 */
void BenchSteadyStateNoOpHooks() {
    for (int n : {10, 100, 1000, 5000}) {
        lua_State *L = luaL_newstate();
        luaL_openlibs(L);
        std::vector<int> refs;
        refs.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; i++) refs.push_back(RegisterHook(L, "return function() return false end"));
        constexpr int kFrames = 200;
        Timer t;
        for (int f = 0; f < kFrames; f++) RunFrameHooksOnce(L, refs);
        double ms = t.ElapsedMs();
        RecordResult(kBinary, kHistoryPath, {"steady_state_permanent_noop_hooks_x" + std::to_string(n), kFrames, ms});
        lua_close(L);
    }
}

/**
 * @brief Cost of N one-shot hooks that self-remove on their first call (the post-fix shape) -- same hook
 * counts as above, but each only ever runs once. The fix's whole point is visible here: a second dispatch
 * pass right after is essentially free, unlike the steady-state case above which pays the same cost every
 * single frame for as long as the process runs.
 */
void BenchOneShotSelfRemovingHooks() {
    for (int n : {10, 100, 1000, 5000}) {
        lua_State *L = luaL_newstate();
        luaL_openlibs(L);
        std::vector<int> refs;
        refs.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; i++) refs.push_back(RegisterHook(L, "return function() return true end"));
        Timer t;
        RunFrameHooksOnce(L, refs);  // every hook fires once and is dropped
        double ms = t.ElapsedMs();
        RecordResult(kBinary, kHistoryPath, {"one_shot_self_removing_hooks_x" + std::to_string(n), n, ms});
        Timer t2;
        RunFrameHooksOnce(L, refs);  // refs is now empty -- should cost ~nothing
        double ms2 = t2.ElapsedMs();
        RecordResult(kBinary, kHistoryPath, {"post_removal_empty_pass_after_x" + std::to_string(n), 1, ms2});
        lua_close(L);
    }
}

}  // namespace

int main() {
    BenchSteadyStateNoOpHooks();
    BenchOneShotSelfRemovingHooks();
    return 0;
}
