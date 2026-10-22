;; The JVM Clojure reader as an oracle for mep's own (src/clojure_reader.cpp):
;; reads every file named on stdin, one per line, and prints
;;
;;   <path>\tOK
;;   <path>\tERR\t<line>\t<message>
;;   <path>\tSKIP          (a #= or a record literal: the read stops there)
;;
;; with the first reader error's line (1-based, as the reader counts it)
;; and its message. Run with `clojure -M tools/clojure_reader_check.clj`;
;; one JVM reads them all.
;;
;; Nothing in a file is evaluated: *read-eval* is false, so `#=(...)` and
;; record literals stop the read with their "not allowed" error -- valid
;; syntax, but the rest of the file is not read, so it is SKIPped. `::alias/kw` resolves any alias,
;; and reader conditionals read the :clj branch, as `load` does for .cljc.
(ns clojure-reader-check
  (:import [clojure.lang LispReader LispReader$ReaderException LineNumberingPushbackReader]
           [java.io StringReader]))

(def resolver
  (reify clojure.lang.LispReader$Resolver
    (currentNS [_] 'user)
    (resolveClass [_ s] s)
    (resolveAlias [_ s] s)
    (resolveVar [_ s] s)))

(def allowed-messages
  #{"EvalReader not allowed when *read-eval* is false."
    "Record construction syntax can only be used when *read-eval* == true"})

(defn check [path]
  (let [text (slurp path)
        rdr (LineNumberingPushbackReader. (StringReader. text))
        eof (Object.)
        opts {:eof eof :read-cond :allow :features #{:clj}}]
    (binding [*read-eval* false
              *reader-resolver* resolver
              *default-data-reader-fn* (fn [_tag v] v)
              *ns* (create-ns 'user)]
      (try
        (loop []
          (let [form (LispReader/read rdr opts)]
            (when-not (identical? form eof) (recur))))
        (str path "\tOK")
        (catch LispReader$ReaderException e
          (let [cause (or (.getCause e) e)
                msg (or (.getMessage cause) (str (class cause)))]
            (if (allowed-messages msg)
              (str path "\tSKIP")
              (str path "\tERR\t" (.-line e) "\t" (first (clojure.string/split-lines msg))))))
        (catch Throwable e
          (str path "\tERR\t0\t" (first (clojure.string/split-lines (str (.getMessage e))))))))))

(doseq [path (line-seq (java.io.BufferedReader. *in*))]
  (println (check path)))
