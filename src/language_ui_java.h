// Java language UI mode (<leader>uu / <leader>lu / :MepLanguageUi in a .java
// or .jsh buffer), kBuiltinLanguageUi's Java consumer, built on
// kBuiltinLanguageUiCommon (mep.language_ui_layout) in the Fortran and C#
// modes' shape. Included by main.cpp only; BuiltinLanguageUiJava() is the Lua
// chunk (help/java-mode.org is the user's page).
//
// The console is jshell's engine driven by the editor: console_source below
// is a small Java program over the jdk.jshell API -- the JShell state, its
// remote execution VM and SourceCodeAnalysis that the `jshell` tool itself is
// built on -- so inputs split into snippets as jshell splits them, values
// print as jshell prints them (`x ==> 5`), and jshell's /vars /methods
// /types /imports /list /drop /reset /open /env /history commands work. The
// launcher (in the Lua) compiles it once per JDK into ~/.cache/mep/java-ui/
// and runs it under rlwrap when there is one.
//
//   - Class path: the `CMD:` header's -cp/-classpath/--class-path, then
//     $MEP_JAVA_CLASSPATH, then $CLASSPATH. jshell takes it at startup, so
//     when the header's changes (checked on every send) the console restarts
//     its engine with the new one and replays the session (/env).
//   - Variables panel: /vars, /methods and /types as sections (with the
//     imports and the class path), rewritten after every input. Clicking a
//     row shows its Help.
//   - Help: /help NAME (gh) -- a session variable's type for `x.method`, the
//     session's own declarations, then Javadoc read by `mep-java-lsp --doc`
//     from the workspace, the file's package root, the source path
//     ($MEP_JAVA_SOURCEPATH, -sourcepath in the CMD: header) and class-path
//     directories (java_lsp_javadoc.h), else the JDK's own (src.zip, through
//     SourceCodeAnalysis.documentation).
//
// <leader>re sends the Visual selection (padded to its lines), else the line
// at the cursor, which the console extends while jshell finds it incomplete
// (a whole statement, method or class, with the file's imports brought
// along); messages point at the source file's lines. <leader>rr runs the
// CMD: header's command, else a file with a main method by single-file
// source launch (`java File.java`, with the class path), else /opens the
// file into the session.
#pragma once
#include <string>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif

inline const char* BuiltinLanguageUiJava() {
    static const std::string chunk = std::string(R"MEPLUA(
mep.opt = mep.opt or {}
-- The java command that runs the console (a `// REPL:` comment replaces it).
mep.opt.java_ui_java = mep.opt.java_ui_java or 'java'
mep.opt.java_ui_rlwrap = mep.opt.java_ui_rlwrap ~= false
mep.opt.java_ui_info_share = mep.opt.java_ui_info_share or 0.34
mep.opt.java_ui_bottom_share = mep.opt.java_ui_bottom_share or 0.4
mep.opt.java_ui_bottom_info_share = mep.opt.java_ui_bottom_info_share or 0.45
for _, e in ipairs({'java', 'jsh'}) do
  mep.language_ui_extensions[e] = mep.language_ui_extensions[e] or 'java'
end

-- The console (see the header comment), written into each session directory.
local console_source = [===[
)MEPLUA") + R"MEPJAVA(// mep's Java console: jshell's engine (the jdk.jshell API -- the JShell
// state, its remote execution VM and SourceCodeAnalysis that the `jshell`
// tool is built on), driven by mep's Java UI mode (language_ui_java.h).
// Inputs are split into snippets exactly as jshell splits them; jshell's
// commands /vars /methods /types /imports /list /drop /reset /open /env
// /history /exit work as in jshell. After each input the session's
// variables, methods, types and imports are written to files mep's panels
// poll. Code sent from a source file keeps that file's line numbers in its
// messages. /help NAME reads Javadoc: mep-java-lsp --doc (the workspace and
// the source path), then the JDK's own (src.zip, through jshell).
//
// Usage: java MepJavaConsole --dir SESSION_DIR [--class-path CP]
//        [--sourcepath SP] [--lsp mep-java-lsp]
import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.PrintStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.stream.Collectors;

import jdk.jshell.DeclarationSnippet;
import jdk.jshell.Diag;
import jdk.jshell.EvalException;
import jdk.jshell.ExpressionSnippet;
import jdk.jshell.ImportSnippet;
import jdk.jshell.JShell;
import jdk.jshell.JShellException;
import jdk.jshell.MethodSnippet;
import jdk.jshell.Snippet;
import jdk.jshell.SnippetEvent;
import jdk.jshell.SourceCodeAnalysis;
import jdk.jshell.TypeDeclSnippet;
import jdk.jshell.UnresolvedReferenceException;
import jdk.jshell.VarSnippet;

public class MepJavaConsole {
    static final String[] DEFAULT_IMPORTS = {
        "java.io.*", "java.math.*", "java.net.*", "java.nio.file.*", "java.util.*", "java.util.concurrent.*",
        "java.util.function.*", "java.util.prefs.*", "java.util.regex.*", "java.util.stream.*",
    };
    static final PrintStream OUT = new PrintStream(new java.io.FileOutputStream(java.io.FileDescriptor.out), true, StandardCharsets.UTF_8);

    static Path dir;
    static String headerClassPath = "";
    static String sourcePath = "";
    static String lsp = "";
    static JShell shell;
    static SourceCodeAnalysis sca;
    static volatile boolean terminated;
    static final List<String> history = new ArrayList<>();
    // The source of every snippet the session kept, in order: what a restart replays (as jshell's /env does).
    static final List<String> kept = new ArrayList<>();
    static int inputCount;

    // --- class path -------------------------------------------------------------

    // The class path: the CMD: header's, then $MEP_JAVA_CLASSPATH, then $CLASSPATH;
    // relative entries against the working directory (as the Run button's command
    // sees them); `dir/*` expanded to the jars in dir, as `java` does.
    static List<String> classPath() {
        Set<String> out = new LinkedHashSet<>();
        for (String list : new String[] {headerClassPath, env("MEP_JAVA_CLASSPATH"), env("CLASSPATH")}) {
            for (String e : list.split(File.pathSeparator)) {
                e = e.trim();
                if (e.isEmpty() || e.equals(".")) continue;
                if (e.endsWith("*")) {
                    File d = absolute(e.substring(0, e.length() - 1));
                    File[] jars = d.listFiles((f, n) -> n.endsWith(".jar") || n.endsWith(".JAR"));
                    if (jars != null) {
                        java.util.Arrays.sort(jars);
                        for (File j : jars) out.add(j.getPath());
                    }
                    continue;
                }
                out.add(absolute(e).getPath());
            }
        }
        return new ArrayList<>(out);
    }

    static File absolute(String p) {
        File f = new File(p);
        return f.isAbsolute() ? f : new File(System.getProperty("user.dir"), p);
    }

    static String env(String name) {
        String v = System.getenv(name);
        return v == null ? "" : v;
    }

    // --- the engine ---------------------------------------------------------------

    static void start() {
        terminated = false;
        // The class path as the jshell tool passes --class-path: to the compiler and the remote VM.
        JShell.Builder b = JShell.builder().out(OUT).err(OUT).in(System.in);
        List<String> cp = classPath();
        if (!cp.isEmpty()) {
            String joined = String.join(File.pathSeparator, cp);
            b.compilerOptions("--class-path", joined).remoteVMOptions("--class-path", joined);
        }
        shell = b.build();
        shell.onShutdown(s -> terminated = true);
        sca = shell.sourceCodeAnalysis();
        for (String imp : DEFAULT_IMPORTS) shell.eval("import " + imp + ";");
    }

    // A fresh engine (with the current class path), replaying what the old one kept.
    static void restart(String why) {
        if (shell != null) {
            try {
                shell.close();
            } catch (Exception ignored) {
            }
        }
        start();
        int ok = 0;
        for (String src : kept) {
            boolean good = false;
            for (SnippetEvent e : shell.eval(src)) {
                if (e.causeSnippet() == null && e.status().isActive() && e.exception() == null) good = true;
            }
            if (good) ok++;
        }
        OUT.println("|  " + why + (kept.isEmpty() ? "" : "; replayed " + ok + " of " + kept.size() + " snippets"));
        writePanels();
    }

    static boolean isDefaultImport(String src) {
        for (String imp : DEFAULT_IMPORTS) {
            if (src.trim().equals("import " + imp + ";")) return true;
        }
        return false;
    }

    // --- evaluation ---------------------------------------------------------------

    // Evaluates `text`, snippet by snippet. `origin` names the source file (null for
    // typed input) and `firstLine` is text's first line in it, for messages.
    static void evaluate(String text, String origin, int firstLine) {
        if (terminated) restart("The state engine had terminated: restarted");
        String rest = text;
        while (!rest.isBlank()) {
            SourceCodeAnalysis.CompletionInfo ci = sca.analyzeCompletion(rest);
            if (ci.completeness() == SourceCodeAnalysis.Completeness.EMPTY) break;
            int consumed = text.length() - rest.length();
            int lead = 0;
            while (lead < rest.length() && Character.isWhitespace(rest.charAt(lead))) lead++;
            int line = firstLine + countLines(text, consumed + lead);
            String src = ci.source() == null ? rest : ci.source();
            String remaining = ci.remaining() == null ? "" : ci.remaining();
            if (ci.completeness() == SourceCodeAnalysis.Completeness.DEFINITELY_INCOMPLETE
                    || ci.completeness() == SourceCodeAnalysis.Completeness.CONSIDERED_INCOMPLETE) {
                src = rest;
                remaining = "";
            }
            evalSnippet(src, origin, line);
            rest = remaining;
        }
        writePanels();
    }

    static int countLines(String s, int upTo) {
        int n = 0;
        for (int i = 0; i < upTo && i < s.length(); i++) if (s.charAt(i) == '\n') n++;
        return n;
    }

    static void evalSnippet(String src, String origin, int line) {
        // `line` is the snippet's first non-blank line; src may start with blank lines.
        int lead = 0;
        while (lead < src.length() && Character.isWhitespace(src.charAt(lead))) lead++;
        line -= countLines(src, lead);
        List<SnippetEvent> events;
        try {
            events = shell.eval(src);
        } catch (IllegalStateException e) {
            terminated = true;
            OUT.println("|  The state engine terminated; the next input restarts it");
            return;
        }
        for (SnippetEvent e : events) {
            if (e.causeSnippet() != null) continue;  // a dependent's update
            Snippet sn = e.snippet();
            if (e.status() == Snippet.Status.REJECTED) {
                printDiagnostics(sn, src, origin, line);
                continue;
            }
            printDiagnostics(sn, src, origin, line);  // warnings
            JShellException ex = e.exception();
            if (e.status().isActive() && !(sn instanceof ImportSnippet i && isDefaultImport(i.source()))) kept.add(sn.source());
            if (ex != null) {
                printException(ex, origin);
                continue;
            }
            feedback(e, sn);
        }
    }

    static void feedback(SnippetEvent e, Snippet sn) {
        String verb = e.previousStatus() == Snippet.Status.NONEXISTENT ? "created" : "replaced";
        if (e.previousStatus() != Snippet.Status.NONEXISTENT && e.isSignatureChange()) verb = "replaced";
        else if (e.previousStatus() != Snippet.Status.NONEXISTENT) verb = "modified";
        String unresolved = "";
        if (sn instanceof DeclarationSnippet ds) {
            List<String> deps = shell.unresolvedDependencies(ds).collect(Collectors.toList());
            if (!deps.isEmpty()) unresolved = ", however, it cannot be used until " + String.join(", ", deps) + " is declared";
        }
        switch (sn.kind()) {
            case VAR -> {
                VarSnippet v = (VarSnippet) sn;
                String value = e.value();
                if (value == null || !e.status().isDefined()) OUT.println("|  " + verb + " variable " + v.name() + " : " + v.typeName() + unresolved);
                else OUT.println(v.name() + " ==> " + value);
            }
            case EXPRESSION -> {
                ExpressionSnippet x = (ExpressionSnippet) sn;
                OUT.println(x.name() + " ==> " + e.value());
            }
            case METHOD -> {
                MethodSnippet m = (MethodSnippet) sn;
                OUT.println("|  " + verb + " method " + m.name() + "(" + m.parameterTypes() + ")" + unresolved);
            }
            case TYPE_DECL -> {
                TypeDeclSnippet t = (TypeDeclSnippet) sn;
                OUT.println("|  " + verb + " " + typeKind(t) + " " + t.name() + unresolved);
            }
            default -> {
            }
        }
    }

    static String typeKind(TypeDeclSnippet t) {
        return switch (t.subKind()) {
            case INTERFACE_SUBKIND -> "interface";
            case ENUM_SUBKIND -> "enum";
            case RECORD_SUBKIND -> "record";
            case ANNOTATION_TYPE_SUBKIND -> "annotation interface";
            default -> "class";
        };
    }

    static void printDiagnostics(Snippet sn, String src, String origin, int line) {
        for (Diag d : shell.diagnostics(sn).collect(Collectors.toList())) {
            String msg = d.getMessage(Locale.ROOT);
            if (!d.isError() && msg.contains("not permitted in top-level declarations")) continue;
            int pos = (int) Math.max(0, Math.min(src.length(), d.getStartPosition()));
            int lineStart = src.lastIndexOf('\n', Math.max(0, pos - 1)) + 1;
            if (pos == 0) lineStart = 0;
            int lineEnd = src.indexOf('\n', pos);
            if (lineEnd < 0) lineEnd = src.length();
            int col = pos - lineStart;
            String text = src.substring(lineStart, lineEnd);
            String kind = d.isError() ? "Error" : "Warning";
            if (origin != null) {
                OUT.println(origin + ":" + (line + countLines(src, pos)) + ":" + (col + 1) + ": " + kind.toLowerCase(Locale.ROOT) + ": " + firstLine(msg));
                for (String m : msg.split("\n", -1)) {
)MEPJAVA" + R"MEPJAVA(                    if (!m.equals(firstLine(msg)) && !m.trim().equals("location: class")) OUT.println("    " + m.trim());
                }
            } else {
                OUT.println("|  " + kind + ":");
                for (String m : msg.split("\n")) if (!m.trim().equals("location: class")) OUT.println("|  " + m);
            }
            OUT.println("|  " + text);
            OUT.println("|  " + " ".repeat(Math.max(0, col)) + "^");
        }
    }

    static String firstLine(String s) {
        int nl = s.indexOf('\n');
        return nl < 0 ? s : s.substring(0, nl);
    }

    static void printException(JShellException ex, String origin) {
        if (ex instanceof UnresolvedReferenceException u) {
            DeclarationSnippet s = u.getSnippet();
            List<String> deps = shell.unresolvedDependencies(s).collect(Collectors.toList());
            OUT.println("|  attempted to use " + s.subKind().toString().toLowerCase(Locale.ROOT).replace("_subkind", "") + " "
                    + s.name() + " which cannot be used until " + String.join(", ", deps) + " is declared");
            return;
        }
        Throwable t = ex;
        String prefix = "|  Exception ";
        while (t != null) {
            String cls = t instanceof EvalException ee ? ee.getExceptionClassName() : t.getClass().getName();
            OUT.println(prefix + cls + (t.getMessage() == null ? "" : ": " + t.getMessage()));
            for (StackTraceElement f : t.getStackTrace()) {
                String file = f.getFileName();
                // jshell's own frames: #id:line, as jshell prints them.
                if (f.getClassName().startsWith("REPL.") || (file != null && file.startsWith("#"))) {
                    String method = f.getMethodName().isEmpty() || f.getMethodName().startsWith("do_it$") ? "" : f.getMethodName() + " ";
                    OUT.println("|        at " + method + "(" + (file == null ? "" : file) + ":" + f.getLineNumber() + ")");
                } else {
                    OUT.println("|        at " + f);
                }
            }
            t = t.getCause();
            prefix = "|  Caused by: ";
        }
    }

    // --- source files -------------------------------------------------------------

    static List<String> readLines(String path) throws IOException {
        String text = Files.readString(Paths.get(path), StandardCharsets.UTF_8);
        if (text.endsWith("\n")) text = text.substring(0, text.length() - 1);
        return new ArrayList<>(List.of(text.split("\n", -1)));
    }

    static final Pattern IMPORT = Pattern.compile("^\\s*import\\s+(static\\s+)?[\\w.]+(\\.\\*)?\\s*;\\s*(//.*)?$");
    static final Pattern PACKAGE = Pattern.compile("^\\s*package\\s+[\\w.]+\\s*;.*$");

    // The file's import lines, evaluated quietly when the session lacks them.
    static void bringImports(List<String> lines) {
        Set<String> have = shell.imports().map(i -> i.source().trim()).collect(Collectors.toSet());
        for (String l : lines) {
            if (!IMPORT.matcher(l).matches()) continue;
            String src = l.trim().replaceAll("\\s*//.*$", "");
            if (have.contains(src)) continue;
            for (SnippetEvent e : shell.eval(src)) if (e.status().isActive()) kept.add(src);
        }
    }

    // Blanks a file's package and import lines (jshell has no packages; imports go first).
    static String asSnippets(List<String> lines) {
        StringBuilder sb = new StringBuilder();
        for (String l : lines) {
            sb.append(PACKAGE.matcher(l).matches() || IMPORT.matcher(l).matches() ? "" : l).append('\n');
        }
        return sb.toString();
    }

    // /evalat ROW PATH NAME: the snippet at line ROW of PATH (a copy of NAME): the
    // line, extended down while jshell considers it incomplete -- a statement, a
    // whole method or class with the Javadoc above it.
    static void evalAt(int row, String path, String name) throws IOException {
        List<String> lines = readLines(path);
        if (row < 1 || row > lines.size()) {
            OUT.println("|  nothing at line " + row);
            return;
        }
        int first = row - 1;
        // On a declaration's Javadoc or annotations: start there and take the declaration too.
        while (first < lines.size() && lines.get(first).isBlank()) first++;
        if (first >= lines.size()) {
            OUT.println("|  nothing to evaluate at line " + row);
            return;
        }
        // Inside a comment block: back to its start.
        for (int k = first; k >= 0; k--) {
            String t = lines.get(k).trim();
            if (t.startsWith("/*")) {
                first = k;
                break;
            }
            if (!(t.startsWith("*") || t.endsWith("*/"))) break;
        }
        // On a declaration: its annotations and the Javadoc above come along.
        for (int k = first - 1; k >= 0; k--) {
            String t = lines.get(k).trim();
            if (t.startsWith("@")) {
                first = k;
                continue;
            }
            if (t.endsWith("*/")) {
                int j = k;
                while (j >= 0 && !lines.get(j).trim().startsWith("/*")) j--;
                if (j >= 0 && lines.get(j).trim().startsWith("/**")) first = j;
            }
            break;
        }
        if (PACKAGE.matcher(lines.get(first)).matches()) {
            OUT.println("|  a package declaration: <leader>rr runs the file");
            return;
        }
        bringImports(lines);
        if (IMPORT.matcher(lines.get(first)).matches()) {
            OUT.println("# " + baseName(name) + ":" + (first + 1));
            evaluate(lines.get(first), name, first + 1);
            return;
        }
        int last = first;
        StringBuilder sb = new StringBuilder(lines.get(first));
        for (;;) {
            SourceCodeAnalysis.Completeness c = sca.analyzeCompletion(sb.toString()).completeness();
            boolean incomplete = c == SourceCodeAnalysis.Completeness.DEFINITELY_INCOMPLETE
                    || c == SourceCodeAnalysis.Completeness.CONSIDERED_INCOMPLETE
                    || sb.toString().trim().startsWith("@") && !sb.toString().contains("(") && !sb.toString().contains("{");
            boolean commentOnly = sb.toString().trim().startsWith("/*") && sb.toString().trim().endsWith("*/");
            if ((!incomplete && !commentOnly) || last + 1 >= lines.size()) break;
            last++;
            sb.append('\n').append(lines.get(last));
        }
        // `if (x) {...}` followed by `else`: take the else too.
        while (last + 1 < lines.size() && lines.get(last + 1).trim().startsWith("else")) {
            int k = last + 1;
            StringBuilder more = new StringBuilder(sb).append('\n').append(lines.get(k));
            while (k + 1 < lines.size()) {
                SourceCodeAnalysis.Completeness c = sca.analyzeCompletion(more.toString()).completeness();
                if (c != SourceCodeAnalysis.Completeness.DEFINITELY_INCOMPLETE && c != SourceCodeAnalysis.Completeness.CONSIDERED_INCOMPLETE) break;
                k++;
                more.append('\n').append(lines.get(k));
            }
            sb = more;
            last = k;
        }
        String chunk = sb.toString();
        OUT.println("# " + baseName(name) + ":" + (first + 1) + (last > first ? "-" + (last + 1) : ""));
        record(chunk);
        evaluate(chunk, name, first + 1);
    }

    static String baseName(String p) {
        return Paths.get(p).getFileName().toString();
    }

    static String cmdHeader(List<String> lines) {
        Pattern p = Pattern.compile("^\\s*[^\\w\\s]+\\s*[Cc][Mm][Dd]:\\s*(.*?)\\s*$");
        for (String l : lines) {
            Matcher m = p.matcher(l);
            if (m.matches()) {
                String cmd = m.group(1).replaceAll("\\s*\\*/$", "").replaceAll("\\s*-->$", "");
                if (!cmd.isEmpty()) return cmd;
            }
        }
        return null;
    }

    static final Pattern MAIN = Pattern.compile("\\bstatic\\s+(public\\s+)?void\\s+main\\s*\\(|(^|\\s)void\\s+main\\s*\\(\\s*\\)");

    // /run PATH NAME: the CMD: header's command, else a file with a main method by
    // single-file source launch (`java File.java`), else the file into the session (/open).
    static void run(String path, String name) throws IOException, InterruptedException {
        List<String> lines = readLines(path);
        record("/run " + name);
        String cmd = cmdHeader(lines);
        if (cmd != null) {
            OUT.println("$ " + cmd);
            exec(List.of("sh", "-c", cmd));
            return;
        }
        boolean hasMain = false;
        for (String l : lines) if (MAIN.matcher(l).find()) hasMain = true;
        if (!hasMain || name.endsWith(".jsh")) {
            open(path, name);
            return;
        }
        Path runDir = dir.resolve("run");
        Files.createDirectories(runDir);
        Path copy = runDir.resolve(baseName(name).endsWith(".java") ? baseName(name) : baseName(name) + ".java");
        Files.copy(Paths.get(path), copy, StandardCopyOption.REPLACE_EXISTING);
        List<String> argv = new ArrayList<>();
        argv.add(javaCommand());
        List<String> cp = classPath();
        if (!cp.isEmpty()) {
            argv.add("--class-path");
            argv.add(String.join(File.pathSeparator, cp));
        }
        argv.add(copy.toString());
        OUT.println("$ java " + (cp.isEmpty() ? "" : "--class-path " + String.join(File.pathSeparator, cp) + " ") + baseName(name));
        exec(argv);
    }

    static String javaCommand() {
        return ProcessHandle.current().info().command().orElse("java");
    }

    static void exec(List<String> argv) throws IOException, InterruptedException {
        Process p = new ProcessBuilder(argv).inheritIO().start();
        int code = p.waitFor();
        if (code != 0) OUT.println("[exit status " + code + "]");
    }

    // /open PATH [NAME]: every snippet of the file into the session, at its own lines.
    static void open(String path, String name) throws IOException {
        List<String> lines = readLines(path);
        bringImports(lines);
        OUT.println("# /open " + baseName(name));
        evaluate(asSnippets(lines), name, 1);
    }

    // --- panels -------------------------------------------------------------------

    static String oneLine(String s, int max) {
        if (s == null) return "";
        s = s.replace('\t', ' ').replace('\r', ' ').replace('\n', ' ');
        return s.length() > max ? s.substring(0, max - 3) + "..." : s;
    }

    static void writeAtomic(String name, String text) {
        try {
            Path p = dir.resolve(name), tmp = dir.resolve(name + ".tmp");
            Files.writeString(tmp, text, StandardCharsets.UTF_8);
            Files.move(tmp, p, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
        } catch (IOException ignored) {
        }
    }

    static List<String> varsRows() {
        List<String> rows = new ArrayList<>();
        shell.variables().filter(v -> shell.status(v).isActive()).forEach(v -> {
            String value;
            try {
                value = shell.status(v).isDefined() ? shell.varValue(v) : "(not defined)";
            } catch (Exception e) {
                value = "(unavailable)";
            }
            rows.add(oneLine(v.name(), 80) + "\t" + oneLine(v.typeName(), 80) + "\t" + oneLine(value, 200));
        });
        return rows;
    }

    static List<String> methodRows() {
        List<String> rows = new ArrayList<>();
        shell.methods().filter(m -> shell.status(m).isActive()).forEach(m -> {
            String sig = m.signature();
            int close = sig.lastIndexOf(')');
            String ret = close >= 0 ? sig.substring(close + 1) : "";
            rows.add(m.name() + "\t" + ret + " " + m.name() + "(" + m.parameterTypes() + ")");
        });
        return rows;
    }

)MEPJAVA" + R"MEPJAVA(    static List<String> typeRows() {
        List<String> rows = new ArrayList<>();
        shell.types().filter(t -> shell.status(t).isActive()).forEach(t -> rows.add(t.name() + "\t" + typeKind(t) + " " + t.name()));
        return rows;
    }

    static List<String> importRows() {
        List<String> rows = new ArrayList<>();
        shell.imports().filter(i -> shell.status(i).isActive()).forEach(i -> rows.add(i.source().trim()));
        return rows;
    }

    static String join(List<String> rows) {
        return rows.isEmpty() ? "" : String.join("\n", rows) + "\n";
    }

    static void writePanels() {
        try {
            if (terminated) return;
            writeAtomic("vars.txt", join(varsRows()));
            writeAtomic("methods.txt", join(methodRows()));
            writeAtomic("types.txt", join(typeRows()));
            writeAtomic("imports.txt", join(importRows()));
            writeAtomic("classpath.txt", join(classPath()));
        } catch (Exception e) {
            OUT.println("(panels: " + e.getMessage() + ")");
        }
    }

    static void record(String text) {
        String t = text.strip();
        if (t.isEmpty()) return;
        history.add(t);
        if (history.size() > 200) history.remove(0);
        writeAtomic("history.txt", String.join("\u001e", history));
    }

    // --- help ---------------------------------------------------------------------

    // /help NAME [FILE]: Javadoc in the Help tab.
    static void help(String query, String file) {
        query = query.trim().replaceAll("[;(]+$", "");
        String text;
        try {
            text = helpText(query, file);
        } catch (Exception e) {
            text = query + ": " + e + "\n";
        }
        writeAtomic("help.txt", text);
        OUT.println("# " + query + ": in the Help tab");
    }

    static String helpText(String query, String file) throws Exception {
        StringBuilder out = new StringBuilder();
        // A session variable's type: `df.filter` asks for DataFrame.filter.
        String name = query;
        String[] parts = query.split("[.#]", 2);
        String varLine = null;
        for (VarSnippet v : shell.variables().filter(v -> shell.status(v).isActive()).collect(Collectors.toList())) {
            if (!v.name().equals(parts[0])) continue;
            String type = v.typeName().replaceAll("<.*>", "");
            String value;
            try {
                value = shell.varValue(v);
            } catch (Exception e) {
                value = "(unavailable)";
            }
            varLine = v.typeName() + " " + v.name() + " ==> " + oneLine(value, 400);
            name = parts.length > 1 ? type + "." + parts[1] : type;
        }
        if (varLine != null && parts.length == 1) out.append("```java\n").append(varLine).append("\n```\n\nvariable of this session\n\n");
        // The session's own methods and types: their source, Javadoc included.
        String simple = name.contains(".") ? name.substring(name.lastIndexOf('.') + 1) : name;
        for (Snippet s : shell.snippets().filter(s -> shell.status(s).isActive()).collect(Collectors.toList())) {
            String n = s instanceof MethodSnippet m ? m.name() : s instanceof TypeDeclSnippet t ? t.name() : null;
            if (n != null && (n.equals(name) || (n.equals(simple) && !name.contains(".")))) {
                out.append("```java\n").append(s.source().strip()).append("\n```\n\ndefined in this session\n\n");
            }
        }
        // The workspace and the source path, through mep's Java server.
        String doc = lspDoc(name, file);
        if (doc != null) out.append(doc).append("\n\n");
        // The JDK's (and the class path's) own Javadoc, through jshell.
        if (doc == null) {
            String jdk = jshellDoc(name);
            if (jdk != null) out.append(jdk);
        }
        if (out.length() == 0) {
            return query + ": no documentation found -- not in this session, the workspace, the source path"
                    + (sourcePath.isEmpty() ? "" : " (" + sourcePath + ")") + " or the JDK.\n";
        }
        return out.toString();
    }

    static String lspDoc(String name, String file) {
        if (lsp.isEmpty()) return null;
        List<String> argv = new ArrayList<>(List.of(lsp, "--doc", name));
        Set<String> roots = new LinkedHashSet<>();
        for (String p : sourcePath.split(File.pathSeparator)) if (!p.isBlank()) roots.add(absolute(p.trim()).getPath());
        for (String cp : classPath()) if (new File(cp).isDirectory()) roots.add(cp);
        if (!roots.isEmpty()) {
            argv.add("--sourcepath");
            argv.add(String.join(File.pathSeparator, roots));
        }
        if (file != null && !file.isEmpty()) {
            argv.add("--file");
            argv.add(file);
        }
        try {
            Process p = new ProcessBuilder(argv).redirectErrorStream(false).redirectError(ProcessBuilder.Redirect.DISCARD).start();
            String text = new String(p.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
            if (!p.waitFor(20, TimeUnit.SECONDS)) {
                p.destroy();
                return null;
            }
            if (p.exitValue() == 0 && !text.isBlank()) return text.strip();
            if (p.exitValue() > 1) lsp = "";  // not a --doc capable build: stop asking
        } catch (IOException | InterruptedException e) {
            lsp = "";
        }
        return null;
    }

    static String jshellDoc(String name) {
        StringBuilder out = new StringBuilder();
        Set<String> seen = new LinkedHashSet<>();
        for (String code : new String[] {name + "(", name}) {
            List<SourceCodeAnalysis.Documentation> docs;
            try {
                docs = sca.documentation(code, code.length(), true);
            } catch (Exception e) {
                continue;
            }
            for (SourceCodeAnalysis.Documentation d : docs) {
                if (!seen.add(d.signature())) continue;
                if (out.length() > 0) out.append("\n\n---\n\n");
                out.append("```java\n").append(d.signature()).append("\n```");
                if (d.javadoc() != null && !d.javadoc().isBlank()) out.append("\n\n").append(renderJavadoc(d.javadoc()));
                if (seen.size() >= 12) break;
            }
            if (out.length() > 0) break;
        }
        return out.length() == 0 ? null : out.toString() + "\n";
    }

    // Javadoc (as jshell returns it: comment text with inline tags and HTML) as Markdown.
    static String renderJavadoc(String doc) {
        doc = doc.replaceAll("(?m)^ ", "");
        String[] blocks = doc.split("\n(?=\\s*@\\w)");
        StringBuilder out = new StringBuilder(inline(blocks[0]).strip());
        StringBuilder params = new StringBuilder(), throwsList = new StringBuilder();
        String returns = null;
        for (int i = 1; i < blocks.length; i++) {
            String b = blocks[i].strip().replaceAll("\\s+", " ");
            Matcher m = Pattern.compile("^@(\\w+)\\s*(\\S*)\\s*(.*)$").matcher(b);
            if (!m.matches()) continue;
            switch (m.group(1)) {
                case "param" -> params.append("\n- `").append(m.group(2)).append("` -- ").append(inline(m.group(3)).strip());
                case "return" -> returns = inline(m.group(2) + " " + m.group(3)).strip();
                case "throws", "exception" -> throwsList.append("\n- `").append(m.group(2)).append("` -- ").append(inline(m.group(3)).strip());
                default -> {
                }
            }
        }
        if (params.length() > 0) out.append("\n\n**Parameters:**").append(params);
        if (returns != null) out.append("\n\n**Returns:** ").append(returns);
        if (throwsList.length() > 0) out.append("\n\n**Throws:**").append(throwsList);
        return out.toString();
    }

    static String inline(String s) {
        s = s.replaceAll("\\{@code\\s+([^{}]*)\\}", "`$1`");
        s = s.replaceAll("\\{@(?:link|linkplain)\\s+([^\\s}]+)\\s+([^}]+)\\}", "$2");
        s = s.replaceAll("\\{@(?:link|linkplain)\\s+#?([^\\s}]+)\\}", "`$1`");
        s = s.replaceAll("\\{@\\w+\\s*([^}]*)\\}", "$1");
        s = s.replaceAll("(?i)<p>\\s*", "\n\n").replaceAll("(?i)</?(code|tt)>", "`").replaceAll("(?i)<li>", "\n- ");
        s = s.replaceAll("(?i)</?(b|strong)>", "**").replaceAll("(?i)</?(i|em)>", "*");
        s = s.replaceAll("<[^>]+>", "");
        s = s.replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&").replace("&quot;", "\"").replace("&nbsp;", " ");
        return s.replaceAll("\n{3,}", "\n\n");
    }

    // --- commands -----------------------------------------------------------------

    static final String USAGE = String.join("\n",
            "|  mep's Java console -- jshell's engine; type Java snippets. Commands:",
            "|    /vars /methods /types /imports   what the session declared",
            "|    /list                            the snippets kept      /history   the inputs",
            "|    /drop NAME|ID                    drop a declaration     /reset     start empty",
            "|    /open FILE                       load a file's snippets",
            "|    /run FILE                        run it (CMD: header, java File.java, or /open)",
            "|    /env --class-path CP             restart with this class path (snippets replayed)",
            "|    /help NAME                       Javadoc in the Help tab (/doc NAME too)",
            "|    /exit                            leave");

    static boolean command(String line) throws Exception {
        String t = line.trim();
        String cmd = t.split("\\s+", 2)[0];
        String arg = t.length() > cmd.length() ? t.substring(cmd.length()).trim() : "";
        switch (cmd) {
            case "/exit", "/quit", "/q" -> System.exit(0);
            case "/help", "/doc", "/h" -> {
                if (arg.isEmpty() || arg.startsWith("/")) {
                    OUT.println(USAGE);
                } else {
                    String[] p = arg.split("\\s+", 2);
                    help(p[0], p.length > 1 ? p[1] : null);
                }
            }
            case "/?" -> OUT.println(USAGE);
            case "/vars" -> {
                for (String r : varsRows()) {
                    String[] f = r.split("\t", 3);
                    OUT.println("|    " + f[1] + " " + f[0] + " = " + (f.length > 2 ? f[2] : ""));
                }
            }
            case "/methods" -> {
                for (String r : methodRows()) OUT.println("|    " + r.split("\t", 2)[1]);
            }
            case "/types" -> {
                for (String r : typeRows()) OUT.println("|    " + r.split("\t", 2)[1]);
            }
            case "/imports" -> {
                for (String r : importRows()) OUT.println("|    " + r);
            }
            case "/list" -> shell.snippets().filter(s -> shell.status(s).isActive() && !(s instanceof ImportSnippet i && isDefaultImport(i.source())))
                    .forEach(s -> OUT.println(String.format("%4s : %s", s.id(), s.source().strip().replace("\n", "\n       "))));
            case "/history" -> {
                for (String h : history) OUT.println(h);
            }
            case "/drop" -> {
                List<Snippet> hits = shell.snippets().filter(s -> shell.status(s).isActive() && (s.id().equals(arg)
                        || (s instanceof DeclarationSnippet d && d.name().equals(arg)))).collect(Collectors.toList());
                if (hits.isEmpty()) OUT.println("|  No such snippet: " + arg);
                for (Snippet s : hits) {
                    kept.remove(s.source());
                    shell.drop(s);
                    OUT.println("|  dropped " + (s instanceof DeclarationSnippet d ? d.name() : s.id()));
                }
                writePanels();
            }
            case "/reset" -> {
                if (shell != null) shell.close();
)MEPJAVA" + R"MEPJAVA(                start();
                kept.clear();
                history.clear();
                writeAtomic("history.txt", "");
                writePanels();
                OUT.println("|  Resetting state.");
            }
            case "/env" -> {
                Matcher m = Pattern.compile("(?:--class-path|-cp|-classpath)(?:\\s+|=)(\\S*)").matcher(arg);
                Matcher s = Pattern.compile("(?:--source-path|-sourcepath|--sourcepath)(?:\\s+|=)(\\S*)").matcher(arg);
                if (s.find()) sourcePath = s.group(1).replaceAll("^\"|\"$", "");
                if (m.find()) {
                    headerClassPath = m.group(1).replaceAll("^\"|\"$", "");
                    restart("Restarted with class path " + String.join(File.pathSeparator, classPath()));
                } else if (!arg.isEmpty() && !s.find(0)) {
                    OUT.println("|  /env: only --class-path and --source-path are supported");
                } else {
                    OUT.println("|  class path: " + String.join(File.pathSeparator, classPath()));
                    OUT.println("|  source path: " + sourcePath);
                }
            }
            case "/open" -> {
                String[] p = arg.split("\\s+", 2);
                record("/open " + arg);
                open(p[0], p.length > 1 ? p[1] : p[0]);
            }
            case "/run" -> {
                String[] p = arg.split("\\s+", 2);
                run(p[0], p.length > 1 ? p[1] : p[0]);
            }
            case "/eval" -> {
                // /eval PATH NAME: PATH is padded with blank lines to NAME's line numbers.
                String[] p = arg.split("\\s+", 2);
                String text = Files.readString(Paths.get(p[0]), StandardCharsets.UTF_8);
                record(text);
                evaluate(text, p.length > 1 ? p[1] : p[0], 1);
            }
            case "/evalat" -> {
                String[] p = arg.split("\\s+", 3);
                evalAt(Integer.parseInt(p[0]), p[1], p.length > 2 ? p[2] : p[1]);
            }
            default -> {
                return false;
            }
        }
        return true;
    }

    public static void main(String[] argv) throws Exception {
        for (int i = 0; i < argv.length; i++) {
            String a = argv[i];
            String v = i + 1 < argv.length ? argv[i + 1] : "";
            switch (a) {
                case "--dir" -> { dir = Paths.get(v).toAbsolutePath(); i++; }
                case "--class-path" -> { headerClassPath = v; i++; }
                case "--sourcepath" -> { sourcePath = v; i++; }
                case "--lsp" -> { lsp = v; i++; }
                default -> { }
            }
        }
        if (dir == null) dir = Files.createTempDirectory("mep-java-ui");
        Files.createDirectories(dir);
        for (String f : new String[] {"vars.txt", "methods.txt", "types.txt", "imports.txt", "history.txt", "help.txt", "classpath.txt"}) {
            if (!Files.exists(dir.resolve(f))) writeAtomic(f, "");
        }
        start();
        writePanels();
        try {
            sun.misc.Signal.handle(new sun.misc.Signal("INT"), sig -> {
                if (shell != null) shell.stop();
            });
        } catch (Throwable ignored) {
        }
        boolean tty = System.console() != null;
        OUT.println("|  Java " + Runtime.version() + " -- mep's Java console (jshell's engine). <leader>re sends the line or selection, "
                + "<leader>rr runs the file, gh / /help NAME shows Javadoc, /? lists commands.");
        BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
        StringBuilder pending = new StringBuilder();
        while (true) {
            if (tty || System.getenv("MEP_JAVA_UI_PROMPT") != null) {
                OUT.print(pending.length() == 0 ? "jshell> " : "   ...> ");
                OUT.flush();
            }
            String line = in.readLine();
            if (line == null) break;
            try {
                if (pending.length() == 0) {
                    if (line.isBlank()) continue;
                    if (line.trim().startsWith("/") && !line.trim().startsWith("/*") && !line.trim().startsWith("//")) {
                        if (!command(line)) OUT.println("|  Unknown command: " + line.trim() + " (/? lists them)");
                        continue;
                    }
                }
                pending.append(pending.length() == 0 ? "" : "\n").append(line);
                SourceCodeAnalysis.Completeness c = sca.analyzeCompletion(pending.toString()).completeness();
                if ((c == SourceCodeAnalysis.Completeness.DEFINITELY_INCOMPLETE || c == SourceCodeAnalysis.Completeness.CONSIDERED_INCOMPLETE)
                        && !(line.isBlank() && c == SourceCodeAnalysis.Completeness.CONSIDERED_INCOMPLETE)) {
                    continue;
                }
                String text = pending.toString();
                pending.setLength(0);
                inputCount++;
                record(text);
                evaluate(text, null, 1);
            } catch (Exception e) {
                pending.setLength(0);
                OUT.println("|  mep console error: " + e);
            }
        }
        if (shell != null) shell.close();
        System.exit(0);
    }
}
)MEPJAVA" + R"MEPLUA(]===]

-- The console's launcher: compiles the program above once per JDK (and again
-- whenever mep ships a different one) into the cache, then runs it, under
-- rlwrap when there is one. A `// REPL:` override (or mep.opt.java_ui_java)
-- names the java command; anything but plain `java` runs the program by
-- single-file source launch instead (no javac needed).
local launcher = [==[
src=$1 sess=$2 use_rlwrap=$3 java_cmd=$4
shift 4
if [ "$java_cmd" = java ]; then
  if ! command -v java >/dev/null 2>&1; then
    echo "mep's Java console needs a JDK (java with the jdk.jshell module) on PATH."
    exit 1
  fi
  if command -v javac >/dev/null 2>&1; then
    ver=$(java -version 2>&1 | head -n 1 | tr -c 'A-Za-z0-9._-' '_')
    root=${XDG_CACHE_HOME:-$HOME/.cache}/mep/java-ui/$ver
    mkdir -p "$root" || exit 1
    if [ ! -f "$root/classes/MepJavaConsole.class" ] || ! cmp -s "$src" "$root/MepJavaConsole.java"; then
      rm -rf "$root/classes.new"
      if javac -nowarn -XDignore.symbol.file -d "$root/classes.new" "$src" >"$root/build.log" 2>&1; then
        rm -rf "$root/classes" && mv "$root/classes.new" "$root/classes" && cp "$src" "$root/MepJavaConsole.java"
      else
        echo "Compiling mep's Java console failed ($root/build.log):"
        tail -n 20 "$root/build.log"
        exit 1
      fi
    fi
    if [ "$use_rlwrap" = 1 ] && command -v rlwrap >/dev/null 2>&1; then
      exec rlwrap -H "$root/history" java -cp "$root/classes" MepJavaConsole "$@"
    fi
    exec java -cp "$root/classes" MepJavaConsole "$@"
  fi
fi
if [ "$use_rlwrap" = 1 ] && command -v rlwrap >/dev/null 2>&1; then
  exec rlwrap sh -c "$java_cmd \"\$@\"" mep-java "$src" "$@"
fi
exec sh -c "$java_cmd \"\$@\"" mep-java "$src" "$@"
]==]

-- tab id -> session state; the sidebars are single global instances that
-- always render whichever tab is active, as in the other modes.
local state, sidebars = {}, nil

local function ensure_sidebars()
  if sidebars then return end
  sidebars = {
    variables = mep.sidebar_create('Variables', 'right', 46),
    history = mep.sidebar_create('History', 'right', 46),
    help = mep.sidebar_create('Help', 'right', 46),
  }
end

local function send(st, text)
  if not st or not mep.is_terminal_buffer(st.console_buf) then return false end
  local info = mep.terminal_info and mep.terminal_info(st.console_buf)
  if info and info.exited then
    mep.notify('Java console exited; :MepJavaRestart starts a fresh one', 'warn')
    return false
  end
  mep.terminal_write(st.console_buf, text .. '\n')
  return true
end

-- The `CMD:` header (the Run button's, main.cpp's mep_run_button_cmd_override)
-- and the class path and source path it names: -cp/-classpath/--class-path,
-- -sourcepath/--source-path (also as --opt=value).
function mep.java_ui_header(lines)
  local cmd
  for _, line in ipairs(lines) do
    local c = line:match('^%s*[^%w%s]+%s*[Cc][Mm][Dd]:%s*(.-)%s*$')
    if c then
      c = c:gsub('%s*%*/$', ''):gsub('%s*%-%->$', '')
      if c ~= '' then cmd = c break end
    end
  end
  local cp, sp = '', ''
  if cmd then
    local words = {}
    for w in cmd:gmatch('%S+') do words[#words + 1] = w:gsub('^["\']', ''):gsub('["\']$', '') end
    for i, w in ipairs(words) do
      local key, val = w:match('^(%-%-?[%w-]+)=(.*)$')
      key = key or w
      val = val or words[i + 1]
      if val and (key == '-cp' or key == '-classpath' or key == '--class-path') then cp = val end
      if val and (key == '-sourcepath' or key == '--source-path') then sp = val end
    end
  end
  return cmd, cp, sp
end

local function trim_value(v)
  return (v or ''):gsub('\\t', '\t')
end

local function row_widgets(st, text, empty, make)
  local widgets = {}
  for line in (text or ''):gmatch('[^\n]+') do
    local w = make(line)
    if w then widgets[#widgets + 1] = w end
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = empty} end
  return widgets
end

local function help_click(st, name)
  return function()
    send(st, '/help ' .. name)
    if st.help_buf then mep.reveal_buffer(st.help_buf, true) end
  end
end

local function render_all()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local vars = row_widgets(st, st and st.vars_text, '(no variables yet -- int n = 3;)', function(line)
    local name, ty, value = line:match('^([^\t]*)\t([^\t]*)\t(.*)$')
    if not name then return nil end
    return {id = 'v' .. name, text = ty .. ' ' .. name .. ' = ' .. trim_value(value), wrap = true, wrap_indent = 2,
      tooltip = 'help ' .. name, on_click = st and help_click(st, name)}
  end)
  local methods = row_widgets(st, st and st.methods_text, '(no methods yet)', function(line)
    local name, sig = line:match('^([^\t]*)\t(.*)$')
    if not name then return nil end
    return {id = 'm' .. sig, text = sig, tooltip = 'help ' .. name, on_click = st and help_click(st, name)}
  end)
  local types = row_widgets(st, st and st.types_text, '(no classes, records, enums or interfaces yet)', function(line)
    local name, desc = line:match('^([^\t]*)\t(.*)$')
    if not name then return nil end
    return {id = 't' .. name, text = desc, tooltip = 'help ' .. name, on_click = st and help_click(st, name)}
  end)
  local imports = row_widgets(st, st and st.imports_text, '(none)', function(line)
    return {id = 'i' .. line, text = line}
  end)
  local cp = row_widgets(st, st and st.classpath_text, '(empty: a CMD: header with -cp, $MEP_JAVA_CLASSPATH or $CLASSPATH sets it)',
    function(line) return {id = 'c' .. line, text = line, wrap = true} end)
  mep.sidebar_set_sections(sidebars.variables, {
    {id = 'vars', title = 'Variables (/vars)', collapsed = false, widgets = vars},
    {id = 'methods', title = 'Methods (/methods)', collapsed = false, widgets = methods},
    {id = 'types', title = 'Types (/types)', collapsed = false, widgets = types},
    {id = 'imports', title = 'Imports', collapsed = true, widgets = imports},
    {id = 'classpath', title = 'Class path', collapsed = true, widgets = cp},
  })
  local widgets, history = {}, (st and st.history) or {}
  for i = #history, math.max(1, #history - 199), -1 do
    local code = history[i]
    widgets[#widgets + 1] = {id = 'h' .. i, text = code, wrap = true,
      on_click = function()
        if code:match('^/') then send(st, code) else
          mep.language_ui_write_file(st.eval_path, code .. '\n')
          send(st, '/eval ' .. st.eval_path)
        end
      end}
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = '(inputs appear here; clicking one runs it again)'} end
  mep.sidebar_set_sections(sidebars.history, {{id = 'history', title = '', collapsed = false, widgets = widgets}})
  mep.language_ui_render_textbox(sidebars.help, st and st.help_text,
    '(gh over a name, or /help NAME in the console: Javadoc from the workspace, the source path or the JDK)')
end

-- A snapshot of the live source buffer (unsaved edits included).
local function snapshot(st)
  local lines = mep.buffer_get_lines(st.src_buf) or {}
  st.submission = (st.submission or 0) + 1
  local path = st.dir .. '/source_' .. st.submission .. '.' .. st.ext
  mep.language_ui_write_file(path, table.concat(lines, '\n') .. '\n')
  return path, lines
end

-- jshell takes its class path at startup: when the CMD: header's changed, the
-- console restarts its engine with the new one (/env, snippets replayed).
local function sync_header(st, lines)
  local _, cp, sp = mep.java_ui_header(lines)
  local args = {}
  if sp ~= st.source_path then args[#args + 1] = '--source-path ' .. (sp == '' and '""' or sp) end
  if cp ~= st.class_path then args[#args + 1] = '--class-path ' .. (cp == '' and '""' or cp) end
  st.class_path, st.source_path = cp, sp
  if #args > 0 then send(st, '/env ' .. table.concat(args, ' ')) end
end

-- <leader>re: the Visual selection if one is live (padded to its source
-- lines), else the line at the cursor -- extended by the console while
-- jshell considers it incomplete (a whole statement, method or class).
function mep.java_ui_eval_block()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then
    mep.notify('No Java console in this tab -- <leader>uu opens one', 'warn')
    return
  end
  if mep.current_buffer() ~= st.src_buf then mep.notify('Send code from the source pane', 'warn') return end
  local lines = mep.buffer_get_lines(st.src_buf) or {}
  sync_header(st, lines)
  local sel = mep.visual_selection and mep.visual_selection() or ''
  if sel ~= '' then
    local r = mep.visual_range()
    mep.cmd('normal!')
    local text
    if r and r.kind == 'char' then
      text = string.rep('\n', r.start_row - 1) .. sel .. '\n'
    elseif r then
      local chunk = {}
      for i = r.start_row, r.end_row do chunk[#chunk + 1] = mep.get_line(i) or '' end
      text = string.rep('\n', r.start_row - 1) .. table.concat(chunk, '\n') .. '\n'
    end
    if text then
      mep.language_ui_write_file(st.eval_path, text)
      send(st, '/eval ' .. st.eval_path .. ' ' .. st.src_abs)
    end
    return
  end
  local path = snapshot(st)
  send(st, '/evalat ' .. (mep.cursor()) .. ' ' .. path .. ' ' .. st.src_abs)
end
mep.language_ui_eval_handlers.java = mep.java_ui_eval_block

function mep.java_ui_open()
  local fname = mep.filename()
  local src_abs = mep_lsp_abspath(fname)
  local ext = src_abs:match('%.([%w]+)$') or 'java'
  local dir = mep.language_ui_session_dir('javaui')
  local paths = {vars = dir .. '/vars.txt', methods = dir .. '/methods.txt', types = dir .. '/types.txt',
    imports = dir .. '/imports.txt', classpath = dir .. '/classpath.txt', history = dir .. '/history.txt',
    help = dir .. '/help.txt'}
  for _, p in pairs(paths) do mep.language_ui_write_file(p, '') end
  local src_path = dir .. '/MepJavaConsole.java'
  mep.language_ui_write_file(src_path, console_source)
  local src_buf = mep.current_buffer()
  local lines = mep.buffer_get_lines(src_buf) or {}
  local _, cp, sp = mep.java_ui_header(lines)
  local java_cmd = mep.language_ui_repl_override or mep.opt.java_ui_java
  local lsp = mep.bundled_tool and mep.bundled_tool('mep-java-lsp') or 'mep-java-lsp'
  local argv = {'sh', '-c', launcher, 'mep-java-ui', src_path, dir, mep.opt.java_ui_rlwrap and '1' or '0', java_cmd,
    '--dir', dir, '--class-path', cp, '--sourcepath', sp, '--lsp', lsp}
  local layout = mep.language_ui_layout({
    info_share = mep.opt.java_ui_info_share, bottom_share = mep.opt.java_ui_bottom_share,
    bottom_info_share = mep.opt.java_ui_bottom_info_share,
    console = function() mep.terminal_here_argv(argv, 'jshell') end,
  })
  local tid = mep.current_tab_id()
  ensure_sidebars()
  local st = {
    paths = paths, dir = dir, ext = ext, console_buf = layout.console_buf, editor_pane = layout.editor_pane,
    src_buf = src_buf, src_abs = src_abs, eval_path = dir .. '/eval.' .. ext, argv = argv,
    class_path = cp, source_path = sp,
    vars_text = '', methods_text = '', types_text = '', imports_text = '', classpath_text = '', help_text = '',
    history = {}, history_raw = '',
  }
  state[tid] = st
  render_all()
  mep.language_ui_open_tabs(layout.top_pane, {sidebars.variables, sidebars.history})
  local help_bufs = mep.language_ui_open_tabs(layout.bottom_pane, {sidebars.help})
  st.help_buf = help_bufs[sidebars.help]
  mep.pane_focus(layout.editor_pane)
  mep.notify('Java language UI: jshell console below; Variables/History top-right, Help bottom-right; <leader>re sends the line or selection, <leader>rr runs the file')
  -- <leader>rr / the Run button: the CMD: header's command, else `java
  -- File.java` for a file with a main method, else the file into the session.
  local function run_source(f)
    local path, now = snapshot(st)
    if mep.java_ui_header(now) then mep.cmd('write') end
    sync_header(st, now)
    send(st, '/run ' .. path .. ' ' .. mep_lsp_abspath(f or fname))
  end
  return {
    run_source = run_source,
    prefer_session_run = true,
    close = function()
      state[tid] = nil
)MEPLUA" + R"MEPLUA(      mep.language_ui_teardown(layout)
      mep.notify('Java language UI mode closed')
    end,
  }
end
mep.language_ui_modes.java = {open = mep.java_ui_open}

-- gh: the Javadoc of the name under the cursor (a session variable's type
-- for `x.method`), from the workspace and source path via mep-java-lsp
-- --doc, else the JDK's, in the Help tab.
function mep.java_ui_help_at_cursor()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then return false end
  local sym = mep.help_symbol_at_cursor('[%w_%.$]')
  if sym then sym = sym:gsub('^[%.]+', ''):gsub('[%.]+$', '') end
  if not sym or sym == '' or not sym:match('^[%a_$]') then mep.notify('gh: no Java name under the cursor', 'warn') return true end
  local from_source = mep.current_buffer() == st.src_buf
  send(st, '/help ' .. sym .. (from_source and (' ' .. snapshot(st)) or ''))
  if from_source then
    if st.help_buf then mep.reveal_buffer(st.help_buf, true) end
  elseif st.help_buf then
    mep.jump_to_buffer(st.help_buf)
  end
  return true
end
mep.help_register_provider('java', mep.java_ui_help_at_cursor)

mep.command('MepJavaRestart', function()
  if not state[mep.current_tab_id()] then return end
  mep.language_ui_toggle(); mep.language_ui_toggle()
end)

-- This tab's session as the panels see it (nil when no Java mode is open).
function mep.java_ui_info()
  local st = state[mep.current_tab_id()]
  if not st then return nil end
  return {directory = st.dir, console_buf = st.console_buf, argv = st.argv, class_path = st.class_path,
    vars = st.vars_text, methods = st.methods_text, types = st.types_text, history = st.history, help = st.help_text}
end

-- Poll loop: re-reads the panel files on a timer, only while a Java mode is
-- open on the active tab.
function mep.java_ui_poll(st)
  st = st or state[mep.current_tab_id()]
  if not st then return end
  local changed, help_changed = false, false
  for _, key in ipairs({'vars', 'methods', 'types', 'imports', 'classpath', 'help'}) do
    local text = mep.language_ui_read_file(st.paths[key])
    if text and text ~= st[key .. '_text'] then
      st[key .. '_text'] = text
      changed = true
      if key == 'help' then help_changed = true end
    end
  end
  local raw = mep.language_ui_read_file(st.paths.history)
  if raw and raw ~= st.history_raw then
    st.history_raw = raw
    local hist = {}
    for entry in raw:gmatch('[^\30]+') do hist[#hist + 1] = entry end
    st.history = hist
    changed = true
  end
  if changed then render_all() end
  if help_changed and st.help_buf then mep.reveal_buffer(st.help_buf, true) end
end
do
  local last_poll = 0
  mep.on_frame(function()
    local st = state[mep.current_tab_id()]
    if not st then return end
    local now = mep.now()
    if now - last_poll < mep.opt.language_ui_poll_interval then return end
    last_poll = now
    mep.java_ui_poll(st)
  end)
end
)MEPLUA";
    return chunk.c_str();
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
