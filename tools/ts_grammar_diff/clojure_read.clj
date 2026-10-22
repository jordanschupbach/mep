;; grammar_diff.py's oracle for Clojure: exits 0 if every form in the file
;; reads, 1 (with the reader's message) if not. Nothing is evaluated:
;; edamame reads `#=(...)` as data, any `#tag` as its value, `::alias/kw`
;; with any alias, and reader conditionals the way the JVM reader does
;; (the :clj branch; a map's #?(...) entries counted after choosing).
(require '[edamame.core :as e])

(let [path (first *command-line-args*)
      text (slurp path)]
  (try
    (e/parse-string-all text {:all true
                              :read-cond :allow
                              :features #{:clj}
                              :auto-resolve (fn [_] 'user)
                              :readers (fn [_] identity)
                              :row-key nil
                              :col-key nil
                              :end-row-key nil
                              :end-col-key nil})
    (System/exit 0)
    (catch Exception ex
      (binding [*out* *err*]
        (println (str path ": " (ex-message ex))))
      (System/exit 1))))
