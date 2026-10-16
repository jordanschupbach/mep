#ifndef MEP_TEX_BACKEND_H
#define MEP_TEX_BACKEND_H

#include <string>
#include <vector>

// The TeX switch's command table (plans/TEX_ENGINE_PLAN.md): how each
// engine a compile can run is spelled, in one place for both callers --
// the editor (kBuiltinTex's mep.tex_compile, through mep.tex_command) and
// mep-mepml's PDF export (TexCompile below).
//
// `builtin` is mep's own engine, the mep-tex helper beside the caller.
// `system` is one of tectonic, pdflatex, xelatex, lualatex or latexmk, or
// any argv typed as one string, where `%f` is the .tex file and `%o` the
// output directory (with no `%f`, the file goes last).

struct TexCommand {
    std::vector<std::string> argv;  // what to run (argv[0] the executable)
    std::string exe;                // what has to be on PATH for it to run
    bool reruns = false;            // reruns itself for cross-references
    bool leaves_files = false;      // writes .log/.aux beside the PDF unasked
};

/**
 * @brief The command that compiles one .tex file with a configured engine.
 * @param engine "builtin" or "system".
 * @param system_command The system engine's name or argv string (ignored for builtin).
 * @param builtin_tool The mep-tex path to run for builtin.
 * @param tex The .tex file (absolute).
 * @param outdir Where the PDF goes.
 * @param keep_logs Whether the log should survive (tectonic deletes it unless asked).
 * @return The command.
 */
TexCommand TexCompileCommand(const std::string &engine, const std::string &system_command,
                             const std::string &builtin_tool, const std::string &tex, const std::string &outdir,
                             bool keep_logs);

/** @brief The system commands the settings panel offers by name. */
const std::vector<std::string> &TexSystemCommandNames();

/**
 * @brief Whether a TeX log asks for another pass (changed labels, a rerun request).
 * @param log_text The log's contents.
 * @return True when another pass would change the output.
 */
bool TexLogWantsRerun(const std::string &log_text);

/**
 * @brief The line of a compile's output worth showing as its error.
 * @param lines The compile's stdout and stderr lines, in order.
 * @return tectonic's first real `error:`, else a TeX `! ...` message, else a
 * -file-line-error `file:12: ...`, else the last non-blank line; empty if none.
 */
std::string TexErrorLine(const std::vector<std::string> &lines);

/**
 * @brief Compiles a .tex file synchronously with a configured engine (mep-mepml's PDF export).
 * @param engine "builtin" or "system".
 * @param system_command As for TexCompileCommand.
 * @param builtin_tool As for TexCompileCommand.
 * @param tex The .tex file (absolute).
 * @param outdir Where the PDF goes.
 * @param cwd The directory to run in (where relative \\input and pictures resolve).
 * @param err On failure, the reason.
 * @return True if outdir/<stem>.pdf was written. Aux files a bare engine leaves are removed.
 */
bool TexCompile(const std::string &engine, const std::string &system_command, const std::string &builtin_tool,
                const std::string &tex, const std::string &outdir, const std::string &cwd, std::string *err);

#endif
