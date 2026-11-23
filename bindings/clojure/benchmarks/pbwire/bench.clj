;; Clojure binding benchmark against protobuf-java generated code
;; (protoc --java_out, the same oracle the Java bench uses) on matrix3.proto
;; and the four standard tiers from BENCHMARKS.md.
;;
;; The same payload as every other row: every scalar kind, the nested
;; message, the two proto3 optionals, and the repeated fields scaled
;; x0 / x1 / x100 / x3000 (136 / 174 / 2469 / 74913 bytes). Every row's
;; bytes are gated against the official runtime before anything is timed.
;;
;; Arms (bindings/README.md#pbench-three-arms-one-contract):
;;   tree  the persistent-map API, fresh map per iteration -- the full task:
;;         a fresh (payload n) per iteration, encode; decode, every
;;         gate-checked field read back per iteration (consume-tree)
;;   core  the mutable-message fast path       -- native object in and out, no map walk
;;   proxy the prebuilt-map task, relabelled   -- held map re-encoded,
;;         decode-and-drop with no read-back. Published, never the headline.
;;   shim  ../java/pbench/jni                  -- ceiling: one JNI crossing, no tree translation
;;
;; The persistent-map fns (encode/decode) are the `tree` row: the map is
;; the binding's own tree, so its walk and materialization are the arm's
;; cost. The `core` row is the mutable Message (fill once, encode many;
;; decode plus close, no map materialization), which is what beats the
;; generated baseline. The review focus for this plan says the core row is
;; that fast path, not a per-call map conversion.
;;
;;   ./bench.sh
(ns pbwire.bench
  (:require [matrix3 :as gen]
            [pbwire.core :as pb])
  (:import (java.util Arrays) (structtest Matrix3$Scalars3)))

(def ^:private cases ["small" "medium" "large" "xlarge"])
(def ^:private repeats [0 1 100 3000])
(def ^:private want [136 174 2469 74913])
(def ^:private floors [50000 50000 5000 500])
(def ^:private min-pass-secs 0.25)
(def ^:private passes 3)

(defn- base-payload []
  {:i32 Integer/MIN_VALUE
   :i64 Long/MAX_VALUE
   :u32 0xffffffff
   :u64 -1
   :s32 Integer/MIN_VALUE
   :s64 Long/MIN_VALUE
   :fx32 0xdeadbeef
   :fx64 72623859790382856
   :sfx32 Integer/MIN_VALUE
   :sfx64 Long/MIN_VALUE
   :f32 Double/POSITIVE_INFINITY
   :f64 -123456.789
   :flag true
   :str "hello"
   :color 2
   :opt_i32 -7
   :opt_str "opt"
   :inner {:name "kid" :value 42}})

(defn- payload [n]
  (assoc (base-payload)
         :nums (mapv #(- % 3) (range n))
         :tags (vec (repeat n "tag"))
         :inners (mapv (fn [i] {:name "grandkid" :value (inc i)}) (range n))
         :flags (mapv even? (range n))))

(defn- official-message [n]
  (let [b (structtest.Matrix3$Scalars3/newBuilder)
        inner (-> (structtest.Matrix3$Inner3/newBuilder)
                  (.setName "kid")
                  (.setValue 42)
                  .build)]
    (.setI32 b Integer/MIN_VALUE)
    (.setI64 b Long/MAX_VALUE)
    (.setU32 b (unchecked-int 0xffffffff))
    (.setU64 b -1)
    (.setS32 b Integer/MIN_VALUE)
    (.setS64 b Long/MIN_VALUE)
    (.setFx32 b (unchecked-int 0xdeadbeef))
    (.setFx64 b 72623859790382856)
    (.setSfx32 b Integer/MIN_VALUE)
    (.setSfx64 b Long/MIN_VALUE)
    (.setF32 b Float/POSITIVE_INFINITY)
    (.setF64 b -123456.789)
    (.setFlag b true)
    (.setStr b "hello")
    (.setColor b structtest.Matrix3$Color3/COLOR3_GREEN)
    (.setInner b inner)
    (.setOptI32 b -7)
    (.setOptStr b "opt")
    (dotimes [i n]
      (.addNums b (unchecked-int (- i 3)))
      (.addTags b "tag")
      (.addInners b (-> (structtest.Matrix3$Inner3/newBuilder)
                        (.setName "grandkid")
                        (.setValue (inc i))
                        .build))
      (.addFlags b (even? i)))
    (.build b)))

(defn- run-ns [n op]
  (let [start (System/nanoTime)]
    (dotimes [_ n] (op))
    (/ (double (- (System/nanoTime) start)) n)))

(defn- calibrate [op floor]
  (loop [count (max 1 floor)]
    (let [start (System/nanoTime)]
      (dotimes [_ count] (op))
      (if (or (>= (/ (double (- (System/nanoTime) start)) 1e9) min-pass-secs)
              (>= count 100000000))
        count
        (recur (* 2 count))))))

(defn- measure-all [floor ops]
  (let [counts (mapv #(calibrate % floor) ops)
        best (atom (vec (repeat (count ops) Double/MAX_VALUE)))]
    (doseq [[c op] (map vector (mapv #(min % 1000) counts) ops)]
      (run-ns c op))
    (dotimes [_ passes]
      (doseq [i (range (count ops))]
        (swap! best #(assoc % i (min (nth % i) (run-ns (nth counts i) (nth ops i)))))))
    @best))

(defn- row [impl op nbytes ns]
  (println (format "  %-18s %-7s %12.1f %10.1f" impl op ns (/ (* nbytes 1000.0) ns))))

(def ^:private sink (atom 0))

(defn- consume-tree
  "The tree decode half of the timed full task: decode to a persistent
   map, then read back every gate-checked field (the same field list the
   gate asserts), so the timer covers the materialization."
  [ty wire n]
  (let [back (pb/decode ty wire)]
    (swap! sink +
           (count (:str back))
           (count (:tags back))
           (count (:inners back))
           (count (get-in back [:inner :name]))
           (get-in back [:inner :value]))
    (when (pos? n)
      (let [last-inner (last (:inners back))]
        (swap! sink + (count (:name last-inner)) (:value last-inner))))))

(defn -main [desc]
  (let [schema (pb/load-schema desc)
        ty (pb/message-type schema "structtest.Scalars3")
        failures (atom 0)]
    (try
      (doseq [tier (range (count cases))]
        (let [label (nth cases tier)
              n (nth repeats tier)
              floor (nth floors tier)
              ;; Hinted so the timed baseline below compiles to a direct
              ;; call: an unhinted local reflects (~800 ns/call here) and
              ;; never warms, which once inflated this row to 9.9x.
              ^Matrix3$Scalars3 official (official-message n)
              reference (.toByteArray official)]
          (if (not= (count reference) (nth want tier))
            (do (println "FAIL" label "official encoder wrote" (count reference)
                         "bytes, expected" (nth want tier))
                (swap! failures inc))
            (let [data (payload n)
                  got (pb/encode ty data)]
              (if (not (Arrays/equals ^bytes got ^bytes reference))
                (do (println "FAIL" label "pbwire-clojure wrote" (count got)
                             "bytes, protobuf-java" (count reference))
                    (swap! failures inc))
                (do
                  (println "OK  " (str label " encoding parity, x" n
                                       " (" (count reference) " bytes)"))
                  (let [back-ok (let [back (pb/decode ty reference)]
                                  (and (= "hello" (:str back))
                                       (= "kid" (get-in back [:inner :name]))
                                       (= n (count (:nums back)))
                                       (or (zero? n)
                                           (= (- n 4) (last (:nums back))))
                                       (Arrays/equals ^bytes reference
                                                      ^bytes (pb/encode ty back))))
                        gen-ok (Arrays/equals
                                ^bytes reference
                                ^bytes (gen/encode-structtest-scalars3 data))
                        ;; Full-task tree gate: a fresh per-iteration build must
                        ;; match the reference too (not just the held map above).
                        fresh-ok (Arrays/equals
                                  ^bytes reference
                                  ^bytes (gen/encode-structtest-scalars3
                                            (payload n)))]
                    (when-not back-ok
                      (println "FAIL" label "pbwire-clojure decode differs")
                      (swap! failures inc))
                    (when-not gen-ok
                      (println "FAIL" label "generated model bytes differ")
                      (swap! failures inc))
                    (when-not fresh-ok
                      (println "FAIL" label "fresh-build tree bytes differ")
                      (swap! failures inc))
                    (when (and back-ok gen-ok fresh-ok)
                      (println "OK  " (str label " generated-model parity, x" n
                                           " (" (count reference) " bytes)"))))
                  ;; Tree ops are the Go-parity full task: a fresh map per
                  ;; iteration, deep materialization of every gate-checked field
                  ;; on decode (consume-tree). Proxy ops are the old prebuilt
                  ;; task, relabelled, not re-measured as a new task.
                  ;; Calibration stays per-op (measure-all): the fast proxy ops
                  ;; keep their own count and never inflate the tree rows.
                  (let [data-msg (pb/new-message ty)
                        _ (pb/fill-message! data-msg data)
                        tier-count (calibrate #(gen/encode-structtest-scalars3
                                                 (payload n)) floor)
                        res (measure-all floor
                              [#(gen/encode-structtest-scalars3 (payload n))
                               #(pb/encode-message data-msg)
                               #(gen/encode-structtest-scalars3 data)
                               #(.toByteArray official)
                               #(consume-tree ty reference n)
                               #(let [m (pb/decode-message ty reference)]
                                  (pb/close! m))
                               #(gen/decode-structtest-scalars3 reference)
                               #(structtest.Matrix3$Scalars3/parseFrom
                                 ^bytes reference)])]
                    (pb/close! data-msg)
                    (println (str label ": " (count reference) " bytes, "
                                  tier-count " iterations (repeat field x" n ")"))
                    (println (format "  %-18s %-7s %12s %10s"
                                     "impl" "op" "ns/op" "MB/s"))
                    (row "pbwire-clj-tree" "encode" (count reference) (nth res 0))
                    (row "pbwire-clj-core" "encode" (count reference) (nth res 1))
                    (row "pbwire-clj-proxy" "encode" (count reference) (nth res 2))
                    (row "protobuf-java" "encode" (count reference) (nth res 3))
                    (row "pbwire-clj-tree" "decode" (count reference) (nth res 4))
                    (row "pbwire-clj-core" "decode" (count reference) (nth res 5))
                    (row "pbwire-clj-proxy" "decode" (count reference) (nth res 6))
                    (row "protobuf-java" "decode" (count reference) (nth res 7))
                    (println))))))))
      (finally (pb/close! schema)))
    (when (pos? @failures)
      (println @failures "check(s) failed")
      (System/exit 1))))
