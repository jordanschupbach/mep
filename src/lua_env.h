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
    /**
     * @brief Constructs a fresh Lua state, opens the standard libraries, and installs the mep.* API
     * table along with the bare-global helper functions the built-in Lua chunks rely on.
     * @param editor The editor instance Lua callbacks and bindings will operate on.
     */
    explicit LuaEnv(Editor *editor);
    /**
     * @brief Closes the owned Lua state, if one was created.
     */
    ~LuaEnv();

    /**
     * @brief Deleted -- LuaEnv owns a unique lua_State and must not be copied.
     */
    LuaEnv(const LuaEnv &) = delete;
    /**
     * @brief Deleted -- LuaEnv owns a unique lua_State and must not be copied.
     */
    LuaEnv &operator=(const LuaEnv &) = delete;

    /**
     * @brief Compiles and runs a chunk of Lua source, reporting any error through the editor's status line.
     * @param code Lua source code to execute.
     * @return True on success, false if compilation or execution raised an error.
     */
    bool DoString(const std::string &code);
    /**
     * @brief Loads and runs a Lua script file, reporting any error through the editor's status line.
     * @param path Filesystem path of the Lua script to execute.
     * @return True on success, false if loading or execution raised an error.
     */
    bool DoFile(const std::string &path);

    /**
     * @brief Invokes a zero-argument Lua function previously registered with luaL_ref (mep.command /
     * mep.map store their callback this way), reporting any error through the editor's status line.
     * @param ref Registry reference of the function to call.
     */
    void CallRef(int ref);
    /**
     * @brief Invokes ref with a single string argument (job stdout/stderr lines, exit codes, ...),
     * reporting any error through the editor's status line. No-op if ref is LUA_NOREF (0 or -1).
     * @param ref Registry reference of the function to call.
     * @param arg String argument to pass to the Lua function.
     */
    void CallRefWithString(int ref, const std::string &arg);
    /**
     * @brief Invokes ref with a single integer argument (job stdout/stderr lines, exit codes, ...),
     * reporting any error through the editor's status line. No-op if ref is LUA_NOREF (0 or -1).
     * @param ref Registry reference of the function to call.
     * @param arg Integer argument to pass to the Lua function.
     */
    void CallRefWithInt(int ref, long long arg);
    /**
     * @brief Invokes ref with a single boolean argument, pushing a real Lua boolean rather than 0/1 as
     * a number -- 0 is truthy in Lua, so `if result then` on an int-as-bool would always fire. Reports
     * any error through the editor's status line.
     * @param ref Registry reference of the function to call.
     * @param arg Boolean argument to pass to the Lua function.
     */
    void CallRefWithBool(int ref, bool arg);
    /**
     * @brief Marshals a Json value to a Lua table/array/scalar (Part V Phase 20 LSP) and invokes ref
     * with it as the single argument, reporting any error through the editor's status line.
     * @param ref Registry reference of the function to call.
     * @param arg Json value to convert and pass to the Lua function.
     */
    void CallRefWithJson(int ref, const Json &arg);
    /**
     * @brief Invokes ref with a string argument and reads back its return value as an array of strings
     * (Part V Phase 22 completion sources).
     * @param ref Registry reference of the function to call.
     * @param arg String argument to pass to the Lua function.
     * @param out Receives the strings from the returned Lua array; left untouched on error or if ref
     * returned something other than an array.
     * @return True on success, false (out untouched) on error/if ref returned something else.
     */
    bool CallRefWithStringForStrings(int ref, const std::string &arg, std::vector<std::string> *out);
    /**
     * @brief Invokes ref with a string argument and reads back its return value as a list of completion
     * candidates (Phase 22 follow-up: kind/detail/doc alongside each item's text). Each returned array
     * element may be a plain string (kinds/details/docs get a matching "" entry, for a custom completion
     * source predating this) or a table {text=, kind=, detail=, doc=} -- four parallel out vectors rather
     * than a struct so this header doesn't need editor.h's CompletionCandidate type; Editor::
     * UpdateCompletionPopup zips them back together.
     * @param ref Registry reference of the function to call.
     * @param arg String argument to pass to the Lua function.
     * @param texts Receives each candidate's text.
     * @param kinds Receives each candidate's kind.
     * @param details Receives each candidate's detail.
     * @param docs Receives each candidate's doc.
     * @return True on success, false (vectors untouched) on error/if ref returned something else.
     */
    bool CallRefWithStringForCompletionItems(int ref, const std::string &arg, std::vector<std::string> *texts,
                                              std::vector<std::string> *kinds, std::vector<std::string> *details,
                                              std::vector<std::string> *docs);
    /**
     * @brief Invokes ref with a string argument and reads back a {detail=, doc=} table from its return
     * value (Phase 22 completion-resolve hook).
     * @param ref Registry reference of the function to call.
     * @param arg String argument to pass to the Lua function.
     * @param detail Receives the returned table's "detail" field; untouched on failure.
     * @param doc Receives the returned table's "doc" field; untouched on failure.
     * @return False if ref is unset, errors, or returns nil/anything else -- "no info yet", distinct
     * from "info is the empty string", which callers should treat identically (skip rendering it).
     */
    bool CallRefWithStringForDetailDoc(int ref, const std::string &arg, std::string *detail, std::string *doc);
    /**
     * @brief Invokes ref with a single boolean argument and returns its boolean result -- Phase 23's
     * Insert-mode Tab/Shift-Tab snippet-jump hook: the Lua side decides whether a snippet is active and,
     * if so, jumps and returns true so the C++ key handler (editor.cpp HandleInsertInput) knows to
     * swallow the keypress instead of falling through to ordinary Tab handling.
     * @param ref Registry reference of the function to call.
     * @param arg Boolean argument to pass to the Lua function.
     * @return The boolean value returned by ref, or false if ref is unset, errors, or returns a
     * non-boolean.
     */
    bool CallRefWithBoolForBool(int ref, bool arg);
    /**
     * @brief Releases a registered callback so it can be garbage-collected -- call once a callback
     * (e.g. a finished job's on_exit) will never fire again.
     * @param ref Registry reference to release.
     */
    void UnrefFunction(int ref);

    /**
     * @brief Invokes a zero-argument ref and reads back its return value as an array of {text=, hl=}
     * widget-segment tables (NVIM_PARITY_PLAN.md Part II Phase 11 statusline). `hl` may be omitted
     * per-segment.
     * @param ref Registry reference of the function to call.
     * @param out Receives each segment's text/hl pair; left untouched if `ref` is 0/none or the call
     * errors.
     * @return False if `ref` is 0/none or the call errors, true otherwise.
     */
    bool CallRefForWidgets(int ref, std::vector<std::pair<std::string, std::string>> *out);

    /**
     * @brief Registers ref (mep.on_frame(fn)) to be invoked once per frame. Used to build debounced
     * buffer-changed/buffer-saved consumers in Lua (polling mep.buffer_change_epoch()/
     * buffer_save_epoch()) without a synchronous callback fired from inside the edit/save call stack.
     * @param ref Registry reference of the zero-argument function to run each frame.
     */
    void RegisterFrameHook(int ref);
    /**
     * @brief Invokes every frame hook previously registered with RegisterFrameHook. main.cpp calls this
     * once per frame; a no-op (empty vector) when nothing has registered.
     */
    void RunFrameHooks();

    /**
     * @brief Returns the underlying Lua state.
     * @return The raw lua_State pointer owned by this LuaEnv.
     */
    lua_State *State() const { return L_; }

    /**
     * @brief Reads the current value of the Lua global mep.org_todo_keywords (default {'TODO','DOING',
     * 'DONE'}, user-configurable) -- native org C++ (Editor::OrgClockIn/OrgClockTable/etc., editor.cpp)
     * needs this same config ParseOrgHeadline's Lua-facing bindings already read (see lua_env.cpp's own
     * ReadOrgTodoKeywords), but has no lua_State access of its own to read it directly.
     * @return The configured list of org TODO keywords.
     */
    std::vector<std::string> GetOrgTodoKeywords() const;

private:
    lua_State *L_ = nullptr;
    Editor *editor_ = nullptr;
    std::vector<int> frame_hook_refs_;
};

#endif  // MEP_LUA_ENV_H
