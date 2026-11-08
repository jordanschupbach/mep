// KotlinCheck: the Kotlin oracle for `just grammar-diff kotlin --oracle`,
// `just kotlin-parse-check` and the formatter comparison. It parses each
// file with the Kotlin compiler's own PSI parser (KtPsiFactory over
// KotlinCoreEnvironment: the parser behind kotlinc, the IDE and ktfmt --
// nothing is resolved, type checked or run) and reports its PsiErrorElements.
// Build it with `just kotlin-check` (into build/grammar-diff/kotlin).
//
//   kotlin-check FILE...           exit 1 if any file has a syntax error;
//                                  errors go to stderr as path:line:col: msg
//   kotlin-check --batch           read paths from stdin; for each print one
//                                  line: path TAB OK | path TAB line:col TAB msg
//                                  (every error: one line each; --first: only
//                                  the first per file)
//   kotlin-check --tree FILE       the PSI tree (element type, offsets) as
//                                  mep's port prints it, for tree parity
//
// A file named *.kts is parsed as a script, as kotlinc does.
import com.intellij.lang.ASTNode;
import com.intellij.openapi.Disposable;
import com.intellij.openapi.util.Disposer;
import com.intellij.openapi.util.text.StringUtil;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiErrorElement;
import com.intellij.psi.PsiFile;
import com.intellij.psi.impl.source.tree.LeafPsiElement;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.PrintStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import org.jetbrains.kotlin.cli.common.CLIConfigurationKeys;
import org.jetbrains.kotlin.cli.common.messages.MessageCollector;
import org.jetbrains.kotlin.cli.jvm.compiler.EnvironmentConfigFiles;
import org.jetbrains.kotlin.cli.jvm.compiler.KotlinCoreEnvironment;
import org.jetbrains.kotlin.config.CompilerConfiguration;
import org.jetbrains.kotlin.psi.KtPsiFactory;

public final class KotlinCheck {
  private static KtPsiFactory factory;

  private static PsiFile parse(String path, String text) {
    String name = Path.of(path).getFileName().toString();
    if (!name.endsWith(".kt") && !name.endsWith(".kts")) name = name + ".kt";
    // The PSI wants \n line ends (as the IDE's document does); ktfmt
    // converts the same way before parsing.
    return factory.createFile(name, StringUtil.convertLineSeparators(text));
  }

  private static void collect(PsiElement e, List<PsiErrorElement> out) {
    if (e instanceof PsiErrorElement) out.add((PsiErrorElement) e);
    for (PsiElement c = e.getFirstChild(); c != null; c = c.getNextSibling()) collect(c, out);
  }

  // 1-based line and column (in UTF-16 units, as the PSI counts) of offset.
  private static String lineCol(String text, int offset) {
    int line = 1, col = 1;
    for (int i = 0; i < offset && i < text.length(); i++) {
      if (text.charAt(i) == '\n') { line++; col = 1; } else col++;
    }
    return line + ":" + col;
  }

  // UTF-8 byte offset of each UTF-16 index of the text (and its end).
  private static int[] utf8Offsets(String text) {
    int[] at = new int[text.length() + 1];
    int b = 0;
    for (int i = 0; i < text.length(); i++) {
      at[i] = b;
      char c = text.charAt(i);
      if (c < 0x80) b += 1;
      else if (c < 0x800) b += 2;
      else if (Character.isHighSurrogate(c) && i + 1 < text.length() && Character.isLowSurrogate(text.charAt(i + 1))) {
        at[++i] = b;  // the low half shares the code point's start
        b += 4;
      } else b += 3;
    }
    at[text.length()] = b;
    return at;
  }

  private static int[] u8;

  // One line per node, depth-indented: element type, start, end (UTF-8 byte
  // offsets into the \n-normalised text), and an error's message. A KDoc
  // comment is printed as one leaf: its inside is KDoc's own grammar.
  private static void dump(ASTNode n, int depth, StringBuilder out) {
    for (int i = 0; i < depth; i++) out.append("  ");
    out.append(n.getElementType().toString()).append(' ')
        .append(u8[n.getStartOffset()]).append(' ').append(u8[n.getStartOffset() + n.getTextLength()]);
    if (n.getElementType().toString().equals("ERROR_ELEMENT")) {
      PsiElement p = n.getPsi();
      if (p instanceof PsiErrorElement) out.append(' ').append(((PsiErrorElement) p).getErrorDescription());
    }
    out.append('\n');
    if (n.getElementType().toString().equals("KDoc")) return;
    for (ASTNode c = n.getFirstChildNode(); c != null; c = c.getTreeNext()) dump(c, depth + 1, out);
  }

  public static void main(String[] args) throws Exception {
    // The environment prints nothing we want; keep stdout for results.
    PrintStream realOut = new PrintStream(System.out, false, StandardCharsets.UTF_8);
    Disposable disposable = Disposer.newDisposable();
    CompilerConfiguration config = new CompilerConfiguration();
    config.put(CLIConfigurationKeys.MESSAGE_COLLECTOR_KEY, MessageCollector.Companion.getNONE());
    KotlinCoreEnvironment env = KotlinCoreEnvironment.createForProduction(
        disposable, config, EnvironmentConfigFiles.JVM_CONFIG_FILES);
    factory = new KtPsiFactory(env.getProject(), false);

    boolean batch = false, first = false, tree = false;
    List<String> paths = new ArrayList<>();
    for (String a : args) {
      if (a.equals("--batch")) batch = true;
      else if (a.equals("--first")) first = true;
      else if (a.equals("--tree")) tree = true;
      else paths.add(a);
    }
    if (batch) {
      BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
      for (String line; (line = in.readLine()) != null; ) if (!line.isEmpty()) paths.add(line);
    }
    int status = 0;
    for (String path : paths) {
      String text;
      try {
        text = new String(Files.readAllBytes(Path.of(path)), StandardCharsets.UTF_8);
      } catch (Exception e) {
        System.err.println(path + ": " + e);
        status = 2;
        continue;
      }
      PsiFile file;
      try {
        file = parse(path, text);
      } catch (Throwable e) {
        // The parser itself threw (a stack overflow on deep nesting).
        if (batch) realOut.println(path + "\tCRASH\t" + e.getClass().getSimpleName());
        else System.err.println(path + ": parser threw " + e);
        status = Math.max(status, 1);
        continue;
      }
      String norm = file.getText();
      if (tree) {
        StringBuilder sb = new StringBuilder();
        u8 = utf8Offsets(norm);
        dump(file.getNode(), 0, sb);
        realOut.print(sb);
        continue;
      }
      List<PsiErrorElement> errors = new ArrayList<>();
      collect(file, errors);
      if (errors.isEmpty()) {
        if (batch) realOut.println(path + "\tOK");
        continue;
      }
      status = Math.max(status, 1);
      for (PsiErrorElement e : errors) {
        String where = lineCol(norm, e.getTextRange().getStartOffset());
        if (batch) realOut.println(path + "\t" + where + "\t" + e.getErrorDescription());
        else System.err.println(path + ":" + where + ": " + e.getErrorDescription());
        if (first) break;
      }
    }
    realOut.flush();
    Disposer.dispose(disposable);
    System.exit(status);
  }
}
