#ifndef MEP_APL_LSP_INDEX_H
#define MEP_APL_LSP_INDEX_H

// The other APL files mep's APL language server answers from: every APL
// source under the workspace root the client opened (hidden directories
// and build output skipped), the directories in MEP_APL_PATH
// (colon-separated; where an installed binding such as datamunge's
// generated namespace lives), and the files the client has open. Their
// definitions -- functions, operators, namespaces and namespace members --
// feed hover, completion, signature help and go-to-definition across files.
// Parsed files are cached by path, modification time and size.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "apl_lsp.h"

class AplWorkspaceIndex {
public:
    /** @brief Indexes the APL files under @p root (the workspace) and MEP_APL_PATH. */
    void SetRoot(const std::string &root);
    /** @brief A file the client has open: its analysis wins over the copy on disk. */
    void SetOpen(const std::string &path, std::shared_ptr<const AplLspDocument> doc);
    void Close(const std::string &path);
    /** @brief A file saved: read it again next time. */
    void Refresh(const std::string &path);

    /** @brief Every indexed file's analysis except @p exclude_path's. */
    AplLspWorkspace Workspace(const std::string &exclude_path);

    size_t file_count() const { return files_.size(); }

private:
    struct Parsed {
        int64_t mtime = 0;
        uint64_t size = 0;
        std::shared_ptr<const AplLspDocument> doc;
    };
    std::shared_ptr<const AplLspDocument> Load(const std::string &path);
    void Scan(const std::string &dir, size_t *budget, int depth);

    std::string root_;
    std::vector<std::string> files_;
    std::map<std::string, std::shared_ptr<const AplLspDocument>> open_;
    std::map<std::string, Parsed> parsed_;
};

/** @brief Whether @p name has an APL source suffix (.apl .apls .aplf .aplo .apln .aplc .apli .dyalog). */
bool AplSourceName(const std::string &name);

/** @brief Splits text into lines (dropping \r). */
std::vector<std::string> AplSplitLines(const std::string &text);

#endif  // MEP_APL_LSP_INDEX_H
