#ifndef MEP_LUA_ENV_H
#define MEP_LUA_ENV_H

#include <string>
#include <utility>
#include <vector>

struct lua_State;
class Editor;
class Json;

// Owns the embedded Lua state and the `mep.*` API bindings exposed to
// scripts. Errors from Lua code are reported through the editor's status
// line rather than propagated as C++ exceptions.
class LuaEnv {
public:
    explicit LuaEnv(Editor *editor);
    ~LuaEnv();

    LuaEnv(const LuaEnv &) = delete;
    LuaEnv &operator=(const LuaEnv &) = delete;

    bool DoString(const std::string &code);
    bool DoFile(const std::string &path);

    // Invokes a zero-argument function previously registered with
    // luaL_ref (mep.command / mep.map store their callback this way).
    void CallRef(int ref);
    // Same, but passing one string/integer argument (job stdout/stderr
    // lines, exit codes, ...). No-op if ref is LUA_NOREF (0 or -1).
    void CallRefWithString(int ref, const std::string &arg);
    void CallRefWithInt(int ref, long long arg);
    // Pushes a real Lua boolean, not 0/1 as a number -- 0 is truthy in
    // Lua, so `if result then` on an int-as-bool would always fire.
    void CallRefWithBool(int ref, bool arg);
    // Marshals a Json value to a Lua table/array/scalar (Part V Phase 20
    // LSP) and calls ref with it as the single argument.
    void CallRefWithJson(int ref, const Json &arg);
    // Calls ref(arg) and reads back an array of strings (Part V Phase 22
    // completion sources). Returns false (out untouched) on error/if ref
    // returned something else.
    bool CallRefWithStringForStrings(int ref, const std::string &arg, std::vector<std::string> *out);
    // Calls ref(arg_bool) and returns its boolean result (false if ref is
    // unset, errors, or returns a non-boolean) -- Phase 23's Insert-mode
    // Tab/Shift-Tab snippet-jump hook: the Lua side decides whether a
    // snippet is active and, if so, jumps and returns true so the C++ key
    // handler (editor.cpp HandleInsertInput) knows to swallow the keypress
    // instead of falling through to ordinary Tab handling.
    bool CallRefWithBoolForBool(int ref, bool arg);
    // Releases a registered callback so it can be garbage-collected --
    // call once a callback (e.g. a finished job's on_exit) will never
    // fire again.
    void UnrefFunction(int ref);

    // Calls a zero-argument ref and reads back an array of {text=, hl=}
    // widget-segment tables (NVIM_PARITY_PLAN.md Part II Phase 11
    // statusline). `hl` may be omitted per-segment. Returns false (and
    // leaves `out` untouched) if `ref` is 0/none or the call errors.
    bool CallRefForWidgets(int ref, std::vector<std::pair<std::string, std::string>> *out);

    // mep.on_frame(fn): registers fn to run once per frame. Used to build
    // debounced buffer-changed/buffer-saved consumers in Lua (polling
    // mep.buffer_change_epoch()/buffer_save_epoch()) without a synchronous
    // callback fired from inside the edit/save call stack. main.cpp calls
    // RunFrameHooks() once per frame; a no-op (empty vector) when nothing
    // has registered.
    void RegisterFrameHook(int ref);
    void RunFrameHooks();

    lua_State *State() const { return L_; }

private:
    lua_State *L_ = nullptr;
    Editor *editor_ = nullptr;
    std::vector<int> frame_hook_refs_;
};

#endif  // MEP_LUA_ENV_H
