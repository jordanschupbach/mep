/* Tcl's own parser over the spans mep-tcl-lsp parsed, for tools/tcl_parse_check.py.
 *
 *   mep-tcl-lsp --parse-report FILE | tcl-parse-oracle FILE
 *
 * Reads the report's `KIND START END ...` lines and answers each in the same
 * shape from libtcl itself: `script` spans with Tcl_ParseCommand in a loop (as
 * Tcl evaluates a script), `cmd` spans (the inside of [...]) the way
 * tclParse.c's ParseTokens does, `expr` spans with Tcl_ParseExpr. Each span is
 * copied into its own NUL-terminated string, as a proc body or a braced
 * argument is when Tcl compiles it. Expression errors have no position in
 * Tcl's API: their message (which quotes the place) is compared instead.
 * Build: cc tools/tcl_parse_oracle.c -I$TCL/include -L$TCL/lib -ltcl8.6 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tcl.h>

static void put_msg(const char *m) {
    for (; *m; m++) {
        if (*m == '\n') fputs("\\n", stdout);
        else if (*m == '\\') fputs("\\\\", stdout);
        else putchar(*m);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)size + 1);
    if (fread(text, 1, (size_t)size, f) != (size_t)size) return 2;
    text[size] = 0;
    fclose(f);
    Tcl_FindExecutable(argv[0]);
    Tcl_Interp *interp = Tcl_CreateInterp();
    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        char kind[16];
        int start, end;
        if (sscanf(line, "%15s %d %d", kind, &start, &end) != 3 || start < 0 || end > size || start > end) continue;
        int n = end - start;
        char *buf = malloc((size_t)n + 1);
        memcpy(buf, text + start, (size_t)n);
        buf[n] = 0;
        Tcl_Parse parse;
        int failed = 0, pos = -1;
        Tcl_ResetResult(interp);
        if (strcmp(kind, "expr") == 0) {
            if (Tcl_ParseExpr(interp, buf, n, &parse) != TCL_OK) failed = 1;
            else Tcl_FreeParse(&parse);
        } else {
            int nested = strcmp(kind, "cmd") == 0;
            const char *p = buf;
            int left = n;
            for (;;) {
                if (Tcl_ParseCommand(interp, p, left, nested, &parse) != TCL_OK) {
                    failed = 1;
                    pos = (int)(parse.term - buf);
                    break;
                }
                const char *next = parse.commandStart + parse.commandSize;
                int closed = nested && parse.term < buf + n && *parse.term == ']' && !parse.incomplete;
                Tcl_FreeParse(&parse);
                left -= (int)(next - p);
                p = next;
                if (closed) break;
                if (left <= 0) {
                    if (nested) {
                        Tcl_SetObjResult(interp, Tcl_NewStringObj("missing close-bracket", -1));
                        failed = 1;
                        pos = -1;
                    }
                    break;
                }
            }
        }
        printf("%s %d %d", kind, start, end);
        if (failed) {
            printf(" error %d ", pos < 0 ? (strcmp(kind, "cmd") == 0 ? start - 1 : -1) : start + pos);
            put_msg(Tcl_GetStringResult(interp));
        } else {
            fputs(" ok", stdout);
        }
        putchar('\n');
        free(buf);
    }
    return 0;
}
