// JavaParseDump: javac's own token stream and syntax tree, printed in the
// format `mep-java-parse-test --dump-tokens/--dump-tree` prints mep's, so
// tools/java_parse_check.py can diff mep's Java lexer and parser
// (src/java_ast.*, src/java_parse*) against javac's (JDK 25) over the corpus.
// It only observes javac (public JavacTask API plus the scanner/tree classes
// opened with --add-exports); nothing of javac is copied.
//
//   java JavaParseDump --tokens   paths on stdin -> per file `@@FILE path`,
//                                 then `KIND start end` per token and
//                                 `  COMMENT style start end` before it
//   java JavaParseDump --tree     paths on stdin -> per file `@@FILE path`,
//                                 then the tree, one node per line:
//                                 `<indent>field: KIND start pos end attrs`
//
// Offsets are UTF-8 byte offsets into the file (javac's are UTF-16 chars;
// converted here). Run with:
//   java --add-exports jdk.compiler/com.sun.tools.javac.{parser,tree,util,code,api}=ALL-UNNAMED \
//        tools/JavaParseDump.java --tree < paths
import com.sun.source.tree.CompilationUnitTree;
import com.sun.source.util.JavacTask;
import com.sun.tools.javac.api.JavacTaskImpl;
import com.sun.tools.javac.parser.JavaTokenizer;
import com.sun.tools.javac.parser.Scanner;
import com.sun.tools.javac.parser.ScannerFactory;
import com.sun.tools.javac.parser.Tokens;
import com.sun.tools.javac.tree.EndPosTable;
import com.sun.tools.javac.tree.JCTree;
import com.sun.tools.javac.tree.TreeInfo;
import com.sun.tools.javac.util.Context;
import com.sun.tools.javac.util.Name;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import javax.tools.DiagnosticCollector;
import javax.tools.JavaCompiler;
import javax.tools.JavaFileObject;
import javax.tools.StandardJavaFileManager;
import javax.tools.ToolProvider;

public class JavaParseDump {
  static int[] byteOff;  // char index -> byte offset
  static String curPath;

  static void computeOffsets(String s) {
    byteOff = new int[s.length() + 1];
    int b = 0;
    for (int i = 0; i < s.length(); i++) {
      byteOff[i] = b;
      char c = s.charAt(i);
      if (c < 0x80) b += 1;
      else if (c < 0x800) b += 2;
      else if (Character.isHighSurrogate(c) && i + 1 < s.length() && Character.isLowSurrogate(s.charAt(i + 1))) {
        byteOff[i + 1] = b;  // the pair's low half maps to the code point's start
        b += 4;
        i++;
      } else b += 3;
    }
    byteOff[s.length()] = b;
  }

  static int off(int p) {
    if (p < 0) return -1;
    if (p > byteOff.length - 1) return byteOff[byteOff.length - 1];
    return byteOff[p];
  }

  static final List<int[]> comments = new ArrayList<>();

  static class SavingTokenizer extends JavaTokenizer {
    SavingTokenizer(ScannerFactory fac, char[] buf, int len) { super(fac, buf, len); }
    @Override
    protected Tokens.Comment processComment(int pos, int endPos, Tokens.Comment.CommentStyle style) {
      comments.add(new int[] {style.ordinal(), pos, endPos});
      return super.processComment(pos, endPos, style);
    }
  }

  static class OpenScanner extends Scanner {
    OpenScanner(ScannerFactory fac, JavaTokenizer t) { super(fac, t); }
  }

  static void dumpTokens(String src, StringBuilder out) {
    Context ctx = new Context();
    ctx.put(javax.tools.DiagnosticListener.class, (javax.tools.DiagnosticListener<JavaFileObject>) d -> {
      if (d.getKind() == javax.tools.Diagnostic.Kind.ERROR)
        out.append("  ERR ").append(off((int) d.getPosition())).append(' ').append(d.getCode()).append('\n');
    });
    com.sun.tools.javac.util.Log.instance(ctx).useSource(new com.sun.tools.javac.file.JavacFileManager(ctx, true, StandardCharsets.UTF_8)
        .getJavaFileObjects(Path.of(curPath)).iterator().next());
    ScannerFactory fac = ScannerFactory.instance(ctx);
    char[] buf = src.toCharArray();
    comments.clear();
    SavingTokenizer tz = new SavingTokenizer(fac, buf, buf.length);
    Scanner sc = new OpenScanner(fac, tz);
    Tokens.Comment.CommentStyle[] styles = Tokens.Comment.CommentStyle.values();
    int ci = 0;
    while (true) {
      sc.nextToken();
      Tokens.Token t = sc.token();
      for (; ci < comments.size(); ci++) {
        int[] c = comments.get(ci);
        out.append("  COMMENT ").append(styles[c[0]]).append(' ').append(off(c[1])).append(' ').append(off(c[2])).append('\n');
      }
      out.append(t.kind.name()).append(' ').append(off(t.pos)).append(' ').append(off(t.endPos));
      if (t.kind == Tokens.TokenKind.IDENTIFIER) out.append(' ').append(t.name());
      out.append('\n');
      if (t.kind == Tokens.TokenKind.EOF) break;
    }
  }

  static final Set<String> SKIP = Set.of("sym", "type", "sourcefile", "modle", "locn", "packge", "toplevelScope",
      "namedImportScope", "starImportScope", "moduleImportScope", "lineMap", "docComments", "endPositions",
      "polyKind", "refPolyKind", "target", "owner", "varargsElement", "constructor", "constructorType", "operator",
      "importScope", "directive", "record", "fullComponentTypes", "erasedExprOriginalType", "attribute",
      "completesNormally", "canCompleteNormally", "hasUnconditionalPattern", "isExhaustive", "patternSwitch",
      "wasEnumSelector", "finallyCanCompleteNormally", "patternMatchingCatch", "startPos", "endpos", "pos",
      "ownerAccessible", "referentType", "overloadKind", "allowNull", "syntheticGuard", "wasMethodReference",
      "kind_ReferenceKind", "localEnv", "typeargs_none", "needsCond", "kind", "opcode", "declaredUsingVar");

  static String kindName(JCTree t) {
    try {
      return t.getKind().name();
    } catch (Throwable e) {
      return t.getTag().name();
    }
  }

  static void dumpTree(JCTree t, EndPosTable ep, String field, int depth, StringBuilder out) {
    out.append("  ".repeat(depth));
    if (field != null) out.append(field).append(": ");
    if (t == null) {
      out.append("null\n");
      return;
    }
    out.append(kindName(t)).append(' ').append(off(TreeInfo.getStartPos(t))).append(' ').append(off(t.pos)).append(' ')
        .append(off(TreeInfo.getEndPos(t, ep)));
    List<Object[]> kids = new ArrayList<>();
    List<Class<?>> chain = new ArrayList<>();
    for (Class<?> c = t.getClass(); c != null && c != Object.class; c = c.getSuperclass()) chain.add(0, c);
    for (Class<?> c : chain) {
      for (Field f : c.getDeclaredFields()) {
        if (Modifier.isStatic(f.getModifiers())) continue;
        String n = f.getName();
        if (SKIP.contains(n) && !(n.equals("kind") && t instanceof JCTree.JCWildcard)) continue;
        if (n.equals("kind") && t instanceof JCTree.JCWildcard) {
          // JCWildcard.kind is a TypeBoundKind tree; print its BoundKind
        }
        try {
          f.setAccessible(true);
          Object v = f.get(t);
          if (v instanceof JCTree.TypeBoundKind tbk) {
            out.append(' ').append(n).append('=').append(tbk.kind.name());
          } else if (v instanceof JCTree jt) {
            kids.add(new Object[] {n, jt});
          } else if (v instanceof com.sun.tools.javac.util.List<?> l) {
            if (l.isEmpty()) continue;
            int i = 0;
            for (Object o : l) {
              if (o instanceof JCTree jt) kids.add(new Object[] {n + "[" + i + "]", jt});
              else if (o instanceof com.sun.tools.javac.util.List<?> l2) {
                int k = 0;
                for (Object o2 : l2) kids.add(new Object[] {n + "[" + i + "][" + (k++) + "]", o2});
              }
              i++;
            }
          } else if (v == null) {
            if (JCTree.class.isAssignableFrom(f.getType())) kids.add(new Object[] {n, null});
          } else if (v instanceof Name nm) {
            out.append(' ').append(n).append('=').append(nm.toString());
          } else if (v instanceof Boolean b) {
            if (b) out.append(' ').append(n);
          } else if (v instanceof Long l) {
            out.append(' ').append(n).append("=0x").append(Long.toHexString(l));
          } else if (v instanceof Enum<?> e) {
            out.append(' ').append(n).append('=').append(e.name());
          } else if (n.equals("value")) {
            String s = String.valueOf(v);
            if (v instanceof Character ch) s = "c" + (int) ch.charValue();
            if (v instanceof String) s = "\"" + s.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r") + "\"";
            out.append(" value=").append(s);
          }
        } catch (Throwable e) {
          // inaccessible: skip
        }
      }
    }
    if (t instanceof JCTree.JCVariableDecl vd && vd.declaredUsingVar()) out.append(" declaredUsingVar");
    out.append('\n');
    for (Object[] k : kids) dumpTree((JCTree) k[1], ep, (String) k[0], depth + 1, out);
  }

  public static void main(String[] args) throws Exception {
    Locale.setDefault(Locale.ENGLISH);
    boolean tokens = args.length > 0 && args[0].equals("--tokens");
    JavaCompiler compiler = ToolProvider.getSystemJavaCompiler();
    StandardJavaFileManager fm = compiler.getStandardFileManager(null, Locale.ENGLISH, StandardCharsets.UTF_8);
    BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
    StringBuilder out = new StringBuilder();
    for (String path; (path = in.readLine()) != null;) {
      if (path.isEmpty()) continue;
      out.setLength(0);
      out.append("@@FILE ").append(path).append('\n');
      try {
        curPath = path;
        String src = new String(Files.readAllBytes(Path.of(path)), StandardCharsets.UTF_8);
        computeOffsets(src);
        if (tokens) {
          dumpTokens(src, out);
        } else {
          DiagnosticCollector<JavaFileObject> dc = new DiagnosticCollector<>();
          JavacTask task = (JavacTask) compiler.getTask(null, fm, dc,
              List.of("-proc:none", "-encoding", "UTF-8", "-Xlint:none", "-nowarn"), null, fm.getJavaFileObjects(path));
          for (CompilationUnitTree cu : task.parse()) {
            JCTree.JCCompilationUnit jcu = (JCTree.JCCompilationUnit) cu;
            dumpTree(jcu, jcu.endPositions, null, 0, out);
          }
        }
      } catch (Throwable t) {
        out.append("CRASH ").append(t.toString().replace("\n", "\\n")).append('\n');
      }
      System.out.print(out);
      System.out.flush();
    }
  }
}
