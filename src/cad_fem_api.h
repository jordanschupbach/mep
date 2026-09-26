#ifndef MEP_CAD_FEM_API_H
#define MEP_CAD_FEM_API_H

#include "cad_doc.h"
#include "cad_fem_methods.h"
#include "cad_topology.h"
#include "fem_adapt.h"
#include "fem_mesh.h"
#include "fem_modal.h"
#include "fem_result.h"
#include "fem_static.h"
#include "fem_study.h"

#include <map>
#include <string>
#include <vector>

class Json;

// The CAD and FEM agent surface (plans/CAD_FEM_PLAN.md Part K).
//
// The method table itself lives in cad_fem_methods.h, which compiles
// without any of the kernel; this header is the session that implements
// it.
//
// ONE IMPLEMENTATION AND THREE ADAPTERS, which is the whole reason this
// file exists rather than the bindings being written three times. Part K
// asks for Lua functions, agent-RPC methods and MCP tools covering the
// same operations, and the obvious way to build that is three
// hand-maintained lists calling into the kernel -- which is three lists
// to keep in step, and they do not stay in step. Instead there is one
// table of methods here, with one dispatch, and each surface is a loop
// over that table:
//
//   * `src/lua_env.cpp` turns every entry into `mep.part_*` / `mep.fem_*`,
//     converting Lua tables to Json and back;
//   * `src/agent_rpc.cpp` dispatches `part.*` / `fem.*` straight to it;
//   * `src/mcp_bridge.cpp` generates its tool declarations from the same
//     table, including the parameter documentation;
//   * and `MEP_AGENT_API.md` is checked against it by the test, so the
//     documentation cannot silently fall behind the code.
//
// WHY THE STATE IS HERE AND NOT IN THE EDITOR. A CAD document, a study, a
// mesh and a result are values, and every operation on them is a pure
// function that already exists. What the agent surface adds is a place to
// keep them between calls, which is a handle table and nothing more. Put
// in the editor it would be unreachable from a headless CLI (K.5) and
// from the tests; put here it is a plain object that anything can own.
namespace cadfem {

// A document, its evaluated model, and what has been built on it.
class Session {
public:
    Session();
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    // Dispatches one call. Returns false with a message rather than
    // throwing: every surface above has to turn a failure into its own
    // shape of error, and an exception crossing a Lua or socket boundary
    // is not that.
    bool Call(const std::string &method, const Json &params, Json *out, std::string *error);

    // What a handle actually holds, for a caller that needs the values
    // rather than a JSON rendering of them.
    //
    // ADDED FOR THE VIEWER (Part L), and only for it. Everything else on
    // this surface deliberately speaks JSON, because everything else is
    // an *adapter* -- Lua, the socket, MCP -- and a shared JSON shape is
    // what keeps the three of them identical. The viewer is not an
    // adapter: it draws the model, and serialising a hundred thousand
    // nodes into JSON so that it can parse them back would be absurd.
    // Read-only pointers, valid until the handle is closed.
    struct ResultView {
        const fem::AnalysisModel *model = nullptr;
        const fem::StaticResult *statics = nullptr;
        const fem::ModalResult *modes = nullptr;
        bool has_modes = false;
    };
    bool LookUpResult(int handle, ResultView *out) const;
    const cad::Model *LookUpDocument(int handle) const;

    // How many of each thing is open, for tests and for `*.list`.
    int DocumentCount() const;
    int StudyCount() const;
    int MeshCount() const;
    int ResultCount() const;

private:
    struct Document {
        cad::Model model;
        std::string title;
        std::string path;
    };
    struct StudyRecord {
        int document = 0;
        fem::Study study;
    };
    struct MeshRecord {
        int document = 0;
        fem::VolumeMesh mesh;
        double worst_dihedral = 0.0;
    };
    struct ResultRecord {
        int document = 0;
        int study = 0;
        int mesh = 0;
        fem::AnalysisModel model;
        fem::StaticResult result;
        fem::ModalResult modes;
        bool has_modes = false;
    };

    std::map<int, Document> documents_;
    std::map<int, StudyRecord> studies_;
    std::map<int, MeshRecord> meshes_;
    std::map<int, ResultRecord> results_;
    int next_id_ = 1;

    Document *Doc(const Json &params, std::string *error);
    StudyRecord *StudyOf(const Json &params, std::string *error);
    MeshRecord *MeshOf(const Json &params, std::string *error);
    ResultRecord *ResultOf(const Json &params, std::string *error);
};

// A field by the name a caller typed: the display names
// (`fem::ScalarFieldName`) and the compact aliases (`von_mises`,
// `stress_yy`) alike. Exposed because the viewer takes a field name from
// a script and has to resolve it the same way `fem.field` does -- two
// resolvers would mean two sets of accepted spellings.
fem::ScalarField FieldByName(const std::string &name, bool *ok);

// THE session. One per process, shared by every surface in it.
//
// There used to be two: `agent_rpc.cpp` held one and `lua_env.cpp` held
// another, so a document built by `mep.part_new` in Lua was invisible to
// `part.list` over the socket and the reverse. The comment on the first
// of them said exactly what the intent was -- "ONE PER PROCESS, NOT ONE
// PER CONNECTION... a document an agent built is a thing a human in the
// same editor should be able to mesh" -- and the second singleton
// silently made it untrue. Part L's viewer is what forced the issue: its
// script runs in Lua and the handles it builds have to be the ones the
// scene builder resolves.
Session &SharedSession();

}  // namespace cadfem

#endif
