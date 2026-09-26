#ifndef MEP_CAD_FEM_METHODS_H
#define MEP_CAD_FEM_METHODS_H

#include <string>
#include <vector>

// The CAD and FEM method table (plans/CAD_FEM_PLAN.md Part K).
//
// SEPARATE FROM `cad_fem_api.h` BECAUSE THE MCP BRIDGE MUST STAY
// DEPENDENCY-FREE. `src/mcp_bridge.cpp` is deliberately a plain external
// client of mep's agent socket -- it links none of mep_core, just JSON
// and POSIX sockets -- and it generates its tool declarations from this
// table. Had the table lived beside the `Session` that implements the
// methods, declaring a tool would have meant linking the entire B-rep
// kernel and finite-element stack into a process whose whole job is to
// forward JSON down a socket.
//
// So this header is data and nothing else: names, summaries, parameters
// and their types. It compiles in isolation, and the four surfaces that
// read it -- Lua, agent RPC, MCP and the documentation check -- each pay
// only for what they actually need.
namespace cadfem {

struct Parameter {
    const char *name = "";
    // "number", "string", "bool", "array", "object". Named rather than
    // enumerated because this string goes straight into the MCP tool
    // schema and the generated documentation.
    const char *type = "";
    bool required = false;
    const char *summary = "";
};

struct Method {
    const char *name = "";  // "part.box"
    const char *summary = "";
    std::vector<Parameter> params;
    // What the reply object carries, for the docs and the tool schema.
    const char *returns = "";
    // Whether the call changes anything. Carried here rather than
    // recomputed per surface because the MCP bridge uses it to decide
    // whether to announce that the agent is working, and a list that
    // disagreed with the one beside it would mark a read as a write.
    bool read_only = false;
};

// Every method, in a stable order. The single source of truth for the
// Lua bindings, the agent-RPC dispatch, the MCP tool declarations and
// the documentation.
const std::vector<Method> &Methods();
const Method *FindMethod(const std::string &name);

// The reference section for MEP_AGENT_API.md, generated from the table.
//
// GENERATED AND THEN *CHECKED*, which is the difference between
// documentation that is accurate and documentation that was accurate
// once. `mep-cad-fem-api-test` compares the file against this and fails
// if they differ, naming the recipe that regenerates it -- so a method
// added without its documentation is a red test rather than a surprise
// six months later. Writing the prose by hand and hoping is the normal
// arrangement and is why every large API's reference has entries for
// things that no longer exist.
std::string MarkdownReference();

// The same in Org, for help/cad-fem.org and the docs chapter.
std::string OrgReference();

}  // namespace cadfem

#endif
