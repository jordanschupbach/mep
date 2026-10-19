// Prints a tree-sitter grammar's trees in a form two grammars can be diffed
// on (tools/ts_grammar_diff/grammar_diff.py builds this once per grammar,
// with -DTS_LANGUAGE=tree_sitter_<name>, linked to that grammar's parser).
//
//   dump < files.txt   For each file named on stdin: "@@FILE <path>", then
//                      every named node, pre-order, one per line as
//                      "<depth> <field or -> <type> <start byte> <end byte>"
//                      (a missing node's type is MISSING), then
//                      "@@ERRORS <count of ERROR and MISSING nodes>".
//   dump --symbols     The grammar's named node types and field names, one
//                      per line as "node <type>" / "field <name>", sorted.
//
// With DUMP_TRACE set, "@@PARSING <path>" goes to stderr before each parse.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tree_sitter/api.h>

#ifndef TS_LANGUAGE
#error "build with -DTS_LANGUAGE=tree_sitter_<name>"
#endif

const TSLanguage *TS_LANGUAGE(void);

// Pre-order, iteratively: error recovery on a large damaged file can nest
// deeply enough that a recursive walk overflows the stack.
static void walk(TSTreeCursor *cursor, int *errors) {
    int depth = 0;
    for (;;) {
        const TSNode node = ts_tree_cursor_current_node(cursor);
        const int missing = ts_node_is_missing(node);
        if (ts_node_is_named(node) || missing) {
            const char *field = ts_tree_cursor_current_field_name(cursor);
            if (missing || ts_node_is_error(node)) (*errors)++;
            printf("%d %s %s %u %u\n", depth, field ? field : "-", missing ? "MISSING" : ts_node_type(node),
                   ts_node_start_byte(node), ts_node_end_byte(node));
        }
        if (ts_tree_cursor_goto_first_child(cursor)) {
            depth++;
            continue;
        }
        while (!ts_tree_cursor_goto_next_sibling(cursor)) {
            if (!ts_tree_cursor_goto_parent(cursor)) return;
            depth--;
        }
    }
}

static int compare_strings(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int print_symbols(const TSLanguage *language) {
    const uint32_t symbol_count = ts_language_symbol_count(language);
    const uint32_t field_count = ts_language_field_count(language);
    const char **lines = calloc(symbol_count + field_count + 1, sizeof(char *));
    size_t n = 0;
    for (uint32_t i = 0; i < symbol_count; i++) {
        if (ts_language_symbol_type(language, (TSSymbol)i) != TSSymbolTypeRegular) continue;
        const char *name = ts_language_symbol_name(language, (TSSymbol)i);
        char *line = malloc(strlen(name) + 6);
        sprintf(line, "node %s", name);
        lines[n++] = line;
    }
    for (uint32_t i = 1; i <= field_count; i++) {
        const char *name = ts_language_field_name_for_id(language, (TSFieldId)i);
        char *line = malloc(strlen(name) + 7);
        sprintf(line, "field %s", name);
        lines[n++] = line;
    }
    qsort(lines, n, sizeof(char *), compare_strings);
    for (size_t i = 0; i < n; i++) {
        // Aliases can name one node type from several symbols.
        if (i == 0 || strcmp(lines[i], lines[i - 1]) != 0) puts(lines[i]);
    }
    return 0;
}

int main(int argc, char **argv) {
    const TSLanguage *language = TS_LANGUAGE();
    if (argc > 1 && strcmp(argv[1], "--symbols") == 0) return print_symbols(language);

    TSParser *parser = ts_parser_new();
    if (!ts_parser_set_language(parser, language)) {
        fprintf(stderr, "dump: the grammar's ABI version is not supported by this runtime\n");
        return 2;
    }
    const int trace = getenv("DUMP_TRACE") != NULL;
    char path[8192];
    while (fgets(path, sizeof path, stdin)) {
        path[strcspn(path, "\r\n")] = 0;
        if (!path[0]) continue;
        FILE *file = fopen(path, "rb");
        if (!file) {
            fprintf(stderr, "dump: cannot open %s\n", path);
            continue;
        }
        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        rewind(file);
        char *text = malloc(size > 0 ? (size_t)size : 1);
        const size_t got = size > 0 ? fread(text, 1, (size_t)size, file) : 0;
        fclose(file);
        // The fuzzer's breadcrumb: stderr is unbuffered, so after a crash or
        // a hang the last line names the file that caused it.
        if (trace) fprintf(stderr, "@@PARSING %s\n", path);
        TSTree *tree = ts_parser_parse_string(parser, NULL, text, (uint32_t)got);
        printf("@@FILE %s\n", path);
        int errors = 0;
        TSTreeCursor cursor = ts_tree_cursor_new(ts_tree_root_node(tree));
        walk(&cursor, &errors);
        ts_tree_cursor_delete(&cursor);
        printf("@@ERRORS %d\n", errors);
        ts_tree_delete(tree);
        free(text);
    }
    ts_parser_delete(parser);
    return 0;
}
