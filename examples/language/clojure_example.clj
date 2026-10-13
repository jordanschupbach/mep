;; A small Clojure tour: records, threading macros, and sequence functions.
;; Runs under babashka (bb), which starts instantly.

(defrecord Point [x y])

(defn distance [a b]
  (let [dx (- (:x a) (:x b))
        dy (- (:y a) (:y b))]
    (Math/sqrt (+ (* dx dx) (* dy dy)))))

(println "hello, world")

(let [a (->Point 0 0)
      b (->Point 3 4)]
  (println (format "distance = %.2f" (distance a b))))

(def nums [1 2 3 4 5])

(println "sum =" (reduce + nums))
(println "squares =" (map #(* % %) nums))
(println "evens =" (filter even? nums))

(->> ["apple" "banana" "avocado" "cherry"]
     (group-by first)
     (map (fn [[letter words]] [letter (count words)]))
     (into (sorted-map))
     (println "by first letter ="))
