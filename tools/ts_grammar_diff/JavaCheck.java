// JavaCheck: the Java oracle for `just grammar-diff java --oracle` and
// `just java-parse-check`. Parses each file with javac's own parser
// (JavacTask.parse(): nothing is attributed, compiled or run; annotation
// processing off) and reports the syntax errors javac's parser gives, the
// way `javac -XDshould-stop.at=PARSE` would. Build it with `just java-check`
// (into build/grammar-diff/java), which compiles it with the pinned JDK 25.
//
//   java-check FILE...      errors as `path:line:col: error: message` on
//                           stderr; exit 1 if any file has one
//   java-check --batch      paths on stdin, one per line; for each, on
//                           stdout: `@@FILE <path>`, then one line per error:
//                           `<line>:<col>:<code>:<message>` (col 1-based,
//                           in chars; the message's newlines as `\n`)
//
// The source level is javac's default (the JDK's own release); preview
// features are off, so they are errors, as javac would report them.
import com.sun.source.util.JavacTask;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Locale;
import javax.tools.Diagnostic;
import javax.tools.DiagnosticCollector;
import javax.tools.JavaCompiler;
import javax.tools.JavaFileObject;
import javax.tools.StandardJavaFileManager;
import javax.tools.ToolProvider;

public class JavaCheck {
  static final List<String> OPTIONS = List.of("-proc:none", "-encoding", "UTF-8", "-Xlint:none", "-nowarn");

  static List<Diagnostic<? extends JavaFileObject>> parse(JavaCompiler compiler, StandardJavaFileManager fm,
      String path) throws Exception {
    DiagnosticCollector<JavaFileObject> dc = new DiagnosticCollector<>();
    JavacTask task = (JavacTask) compiler.getTask(null, fm, dc, OPTIONS, null, fm.getJavaFileObjects(path));
    task.parse();
    return dc.getDiagnostics();
  }

  public static void main(String[] args) throws Exception {
    Locale.setDefault(Locale.ENGLISH);
    JavaCompiler compiler = ToolProvider.getSystemJavaCompiler();
    StandardJavaFileManager fm = compiler.getStandardFileManager(null, Locale.ENGLISH, StandardCharsets.UTF_8);
    if (args.length == 1 && args[0].equals("--batch")) {
      BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
      StringBuilder out = new StringBuilder();
      for (String path; (path = in.readLine()) != null;) {
        if (path.isEmpty()) continue;
        out.setLength(0);
        out.append("@@FILE ").append(path).append('\n');
        try {
          for (Diagnostic<? extends JavaFileObject> d : parse(compiler, fm, path)) {
            if (d.getKind() != Diagnostic.Kind.ERROR) continue;
            out.append(d.getLineNumber()).append(':').append(d.getColumnNumber()).append(':')
                .append(d.getCode()).append(':')
                .append(d.getMessage(Locale.ENGLISH).replace("\\", "\\\\").replace("\n", "\\n")).append('\n');
          }
        } catch (Throwable t) {
          out.append("0:0:crash:").append(t.toString().replace("\n", "\\n")).append('\n');
        }
        System.out.print(out);
        System.out.flush();
      }
      return;
    }
    int status = 0;
    for (String path : args) {
      for (Diagnostic<? extends JavaFileObject> d : parse(compiler, fm, path)) {
        if (d.getKind() != Diagnostic.Kind.ERROR) continue;
        System.err.println(path + ":" + d.getLineNumber() + ":" + d.getColumnNumber() + ": error: "
            + d.getMessage(Locale.ENGLISH));
        status = 1;
      }
    }
    System.exit(status);
  }
}
