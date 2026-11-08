// Fills the google-java-format reference cache that tools/java_gjf_ref.py
// reads (format_compare.py's 'java' preset): one JVM over a list of files,
// instead of GJF's JVM start-up per file.
//
//   JAVA=$(sed -n 's/^exec -a "google-java-format" "\([^"]*\)".*/\1/p' $(which google-java-format))
//   $JAVA <the --add-exports flags of the google-java-format wrapper> \
//     -cp google-java-format-1.36.1-all-deps.jar tools/java_gjf_cache.java FILES.txt CACHE_DIR
//
// (tools/java_gjf_ref.py does exactly that.) Each file is formatted the way
// `google-java-format FILE` prints it -- the CLI's Main in-process, default
// options -- and stored as CACHE_DIR/<sha256 of the bytes>.out, or .err
// (GJF's diagnostics) when GJF refuses it.
import com.google.googlejavaformat.java.Main;
import java.io.*;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

public class java_gjf_cache {
  static String sha256(byte[] data) throws Exception {
    byte[] d = MessageDigest.getInstance("SHA-256").digest(data);
    StringBuilder sb = new StringBuilder();
    for (byte b : d) sb.append(String.format("%02x", b));
    return sb.toString();
  }

  public static void main(String[] args) throws Exception {
    List<String> files = new ArrayList<>();
    for (String l : Files.readAllLines(Path.of(args[0]))) if (!l.isEmpty()) files.add(l);
    Path cache = Path.of(args[1]);
    Files.createDirectories(cache);
    int threads = args.length > 2 ? Integer.parseInt(args[2]) : Runtime.getRuntime().availableProcessors();
    ExecutorService ex = Executors.newFixedThreadPool(threads);
    AtomicInteger done = new AtomicInteger();
    List<Future<?>> futs = new ArrayList<>();
    for (String f : files) {
      futs.add(ex.submit(() -> {
        try {
          byte[] data = Files.readAllBytes(Path.of(f));
          String h = sha256(data);
          Path out = cache.resolve(h + ".out"), err = cache.resolve(h + ".err");
          if (!Files.exists(out) && !Files.exists(err)) {
            ByteArrayOutputStream ob = new ByteArrayOutputStream(), eb = new ByteArrayOutputStream();
            PrintWriter ow = new PrintWriter(new OutputStreamWriter(ob, "UTF-8"));
            PrintWriter ew = new PrintWriter(new OutputStreamWriter(eb, "UTF-8"));
            int rc;
            try {
              rc = new Main(ow, ew, InputStream.nullInputStream()).format(f);
            } catch (Throwable t) {
              ew.println(f + ": google-java-format threw " + t);
              rc = 1;
            }
            ow.flush();
            ew.flush();
            Path tmp = cache.resolve(h + "." + Thread.currentThread().threadId() + ".tmp");
            Files.write(tmp, rc == 0 ? ob.toByteArray() : eb.toByteArray());
            Files.move(tmp, rc == 0 ? out : err, StandardCopyOption.ATOMIC_MOVE);
          }
        } catch (Exception e) {
          System.err.println(f + ": " + e);
        }
        int n = done.incrementAndGet();
        if (n % 1000 == 0) System.err.println("  " + n + "/" + files.size());
      }));
    }
    for (Future<?> fu : futs) fu.get();
    ex.shutdown();
  }
}
