#ifndef MEP_FORTRAN_LSP_MODULES_H
#define MEP_FORTRAN_LSP_MODULES_H

// Where mep's Fortran language server finds the source of a module that a
// file USEs but does not define (fortran_lsp.h's FortranLspModuleSource).
// In order:
//   1. the workspace: Fortran files under the root the client opened
//      (hidden directories and build output skipped), and the files the
//      client has open;
//   2. the directories in MEP_FORTRAN_PATH (colon-separated);
//   3. the -I directories in FFLAGS and FCFLAGS;
//   4. the directory of the file doing the USE.
// A module name (case-insensitive) maps to the file holding its
// `module NAME` statement. Parsed modules are cached by path, modification
// time and size, so a large generated module is read once.

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "fortran_lsp.h"

class FortranModuleIndex {
public:
    /** @brief Indexes the Fortran files under @p root (the workspace). */
    void SetRoot(const std::string &root);
    /** @brief A file the client has open: its analysis answers for the modules it defines. */
    void SetOpen(const std::string &path, std::shared_ptr<const FortranLspDocument> doc);
    void Close(const std::string &path);
    /** @brief A workspace file changed on disk (saved): re-read which modules it defines. */
    void Refresh(const std::string &path);

    /** @brief The analysis of the file defining @p module, searched as above from @p from_path. */
    std::shared_ptr<const FortranLspDocument> Find(const std::string &module, const std::string &from_path);
    /** @brief Every module name the index knows about from @p from_path. */
    std::vector<std::string> Names(const std::string &from_path);
    /** @brief The directories searched after the workspace, in order, for a file at @p from_path. */
    std::vector<std::string> SearchDirs(const std::string &from_path) const;

    /** @brief Whether a module @p doc imported has changed since (edited, saved, opened). */
    bool Stale(const FortranLspDocument &doc);
    /** @brief Analyses @p lines (a file at @p path), resolving its USEs through this index. */
    std::shared_ptr<const FortranLspDocument> Analyze(const std::vector<std::string> &lines, const std::string &path);

    size_t workspace_files() const { return workspace_files_; }

private:
    struct FileModules {
        int64_t mtime = 0;
        uint64_t size = 0;
        std::vector<std::string> names;
    };
    struct Parsed {
        int64_t mtime = 0;
        uint64_t size = 0;
        std::shared_ptr<const FortranLspDocument> doc;
    };
    const std::vector<std::string> &ModulesOfFile(const std::string &path);
    std::shared_ptr<const FortranLspDocument> Load(const std::string &path);
    bool Defines(const std::shared_ptr<const FortranLspDocument> &doc, const std::string &module) const;

    std::string root_;
    std::map<std::string, std::string> workspace_;  // module -> path
    std::set<std::string> workspace_paths_;
    size_t workspace_files_ = 0;
    std::map<std::string, std::shared_ptr<const FortranLspDocument>> open_;  // path -> analysis
    std::map<std::string, FileModules> file_modules_;
    std::map<std::string, Parsed> parsed_;
    std::set<std::string> loading_;  // paths being analysed (guards USE cycles)
};

/** @brief Whether @p name has a Fortran source suffix (.f90, .F90, .f, .for, ...). */
bool FortranSourceName(const std::string &name);

#endif  // MEP_FORTRAN_LSP_MODULES_H
